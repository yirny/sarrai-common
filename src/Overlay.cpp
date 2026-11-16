// SPDX-License-Identifier: GPL-3.0-or-later
//
// FILE MAP
//   Hook targets     - Address Library IDs for D3D init and input dispatch
//   ImGui lifecycle  - OnD3DInit creates the context on the game's device/window
//   Frame            - DrawFrame draws the plugin's window once per frame while it is open,
//                      without the controller keys the Win32 backend reads itself
//   Menu             - OverlayMenu: a movie-less game menu; the game shows its own
//                      cursor over it, and its PostDisplay calls DrawFrame;
//                      its AdvanceMovie calls Config::advance;
//                      the HUD is hidden while it is open, and the game's menus are
//                      shown even if the console's ToggleMenus hid them; a Cursor Menu
//                      closed while it is open is shown again (CursorCloseSink,
//                      ShowCursorMenuAgain)
//   Input          - InputDispatchHook routes events to the window; while it is
//                      open the game sees no input at all; after the close it gets the
//                      releases of the buttons it saw go down before (GiveOwedReleases)
//   Install          - checks both call sites, writes the two trampoline calls (SE/AE only),
//                      registers the menu
//
// The IDs and offsets match two independent, widely used ImGui mods (Wheeler, SKSE Menu
// Framework), and dMenu NG checked them, with the callees below, on 1.6.1170, 1.7.99 and
// 1.7.104 (its src/bin/RuntimeCompatibility.cpp); see CLAUDE.md before changing.
#include "PCH.h"

#include "sarrai/Overlay.h"

#include "sarrai/Theme.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>  // InputEventsQueue, IsGamepadKey

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	std::atomic_bool g_renderReady = false;
	std::atomic_bool g_menuRegistered = false;

	// Set once by Install, before either hook is written; read-only afterwards.
	Overlay::Config g_config;

	// --- Hook targets ---------------------------------------------------------

	struct D3DInitHook
	{
		static constexpr auto          id = REL::RelocationID(75595, 77226);
		static constexpr auto          offset = REL::VariantOffset(0x9, 0x275, 0x0);
		static constexpr std::uint64_t calleeAE = 77396;  // the function the call at the site reaches on AE

		static void                                    thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct InputDispatchHook
	{
		static constexpr auto          id = REL::RelocationID(67315, 68617);
		static constexpr auto          offset = REL::VariantOffset(0x7B, 0x7B, 0x0);
		static constexpr std::uint64_t calleeAE = 68655;

		static void                                    thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_events);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct WndProcHook
	{
		static LRESULT        thunk(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam);
		static inline WNDPROC func = nullptr;
	};

	// --- ImGui lifecycle ------------------------------------------------------

	// Contract: runs on the render thread, once, right after the game created
	// its D3D11 device and swap chain. Everything ImGui needs lives in the
	// renderer singleton at that point.
	void OnD3DInit()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			logger::error("ImGui: renderer singleton missing");
			return;
		}

		auto& data = renderer->GetRuntimeData();
		auto* swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
		auto* device = reinterpret_cast<ID3D11Device*>(data.forwarder);
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
		if (!swapChain || !device || !context) {
			logger::error("ImGui: swap chain, device or context missing");
			return;
		}

		DXGI_SWAP_CHAIN_DESC desc{};
		if (FAILED(swapChain->GetDesc(&desc))) {
			logger::error("ImGui: GetDesc failed");
			return;
		}

		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;  // never write imgui.ini into the game folder
		io.LogFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		io.MouseDrawCursor = false;  // the game's Cursor Menu draws the cursor (OverlayMenu uses it)

		Theme::LoadFonts(g_config.assetFolder / L"fonts");  // style and scale are set per frame by Theme::Apply

		if (!ImGui_ImplWin32_Init(desc.OutputWindow) || !ImGui_ImplDX11_Init(device, context)) {
			logger::error("ImGui: backend init failed");
			return;
		}

		WndProcHook::func = reinterpret_cast<WNDPROC>(
			SetWindowLongPtrA(desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));

		Theme::LoadTextures(device, g_config.assetFolder / L"textures", g_config.iconFiles(), g_config.defaultIconKey);
		g_renderReady = true;
		logger::info("ImGui initialised ({}x{})", desc.BufferDesc.Width, desc.BufferDesc.Height);
	}

	// --- Frame ----------------------------------------------------------------

	// The Win32 backend also reads the controller itself (XInput) and queues its buttons as
	// gamepad keys right after the OS cursor. ImGui holds back a cursor position queued after a
	// key change until the next frame, so on a button change the OS cursor stood for a frame,
	// not the game's cursor that prepareFrame's input feed queues next: the hover and the focus
	// highlight went out, and a window being dragged jumped to it. The window takes the game's
	// input only (InputDispatchHook), so those keys go.
	// Contract: render thread, right after ImGui_ImplWin32_NewFrame.
	void DropBackendGamepadKeys()
	{
		auto& queue = ImGui::GetCurrentContext()->InputEventsQueue;
		for (int i = queue.Size - 1; i >= 0; --i) {
			if (queue[i].Type == ImGuiInputEventType_Key && ImGui::IsGamepadKey(queue[i].Key.Key)) {
				queue.erase(queue.Data + i);
			}
		}
	}

	// Contract: called only from OverlayMenu::PostDisplay, so only while the menu
	// is open. No ImGui frame runs while the window is closed.
	void DrawFrame()
	{
		if (!g_renderReady || !g_config.isOpen()) {
			return;
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();  // sets io.DisplaySize; also queues the OS cursor, which prepareFrame's input feed overrides
		DropBackendGamepadKeys();
		g_config.prepareFrame(ImGui::GetIO().DisplaySize.y);
		ImGui::NewFrame();

		g_config.draw();

		ImGui::Render();
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	void D3DInitHook::thunk()
	{
		func();
		OnD3DInit();
	}

	// --- Menu -----------------------------------------------------------------

	// The HUD movie hidden while the menu is open, and its visibility before.
	// Contract: touched only from OverlayMenu::ProcessMessage (UI message
	// processing, game thread); the GPtr keeps the movie alive until restored.
	RE::GPtr<RE::GFxMovieView> g_hud;
	bool                       g_hudVisible = false;

	// Hides the HUD (compass, activate prompt, bars) as Tailor 3.0 does while its
	// menu is open. A repeated show must not overwrite the saved visibility.
	void HideHud()
	{
		if (g_hud) {
			return;
		}
		auto* ui = RE::UI::GetSingleton();
		auto  hud = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
		if (hud && hud->uiMovie) {
			g_hud = hud->uiMovie;
			g_hudVisible = g_hud->GetVisible();
			g_hud->SetVisible(false);
		}
	}

	void RestoreHud()
	{
		if (g_hud) {
			g_hud->SetVisible(g_hudVisible);
			g_hud = nullptr;
		}
	}

	// Whether the game's menus were hidden (the console's ToggleMenus) as the menu opened.
	// Contract: touched only from OverlayMenu::ProcessMessage, as g_hud is.
	bool g_menusHidden = false;

	// Contract: while the game's menus are hidden the engine calls no PostDisplay, so the
	// window is neither drawn nor fed input, and InputDispatchHook still gives the game
	// nothing: the window could never close. Show them while the menu is open.
	void ShowHiddenMenus()
	{
		auto* ui = RE::UI::GetSingleton();
		if (ui && !ui->IsShowingMenus()) {
			ui->ShowMenus(true);
			g_menusHidden = true;
		}
	}

	void RestoreHiddenMenus()
	{
		if (!std::exchange(g_menusHidden, false)) {
			return;
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->ShowMenus(false);
		}
	}

	// The Cursor Menu closed while the window was open (set by CursorCloseSink), and the reshow
	// was logged since this menu's show.
	// Contract: the event's thread is not documented, so the first is atomic; the second is
	// touched only from OverlayMenu (game thread), as g_hud is.
	std::atomic_bool g_cursorClosed = false;
	bool             g_cursorReshowLogged = false;

	// Buttons pressed while the window was open, by ButtonKey: their releases stay with the
	// window (RecordButton). Emptied as the menu shows.
	// Contract: game thread (the input dispatch and OverlayMenu::ProcessMessage).
	std::vector<std::uint64_t> g_pressedInWindow;

	// The releases the window took of buttons the game saw go down before it opened, given to
	// the game after the close (GiveOwedReleases).
	// Contract: game thread (the input dispatch).
	struct OwedRelease
	{
		RE::INPUT_DEVICE                      device;
		std::uint32_t                         idCode;
		float                                 heldSecs;  // as the release said
		std::chrono::steady_clock::time_point releasedAt;
	};
	std::vector<OwedRelease> g_owedReleases;

	// The game shows its Cursor Menu as this menu opens, but closes it again as another menu
	// closes, though this menu is still open: the Fader Menu after a save loaded (the cursor
	// showed, then went about a second later), or another mod's HUD menu closing as the window
	// opened, before this menu's first frame. The cursor is then gone and the mouse moves it no
	// more. Every other event returns after a bool and the window's open state.
	class CursorCloseSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && !a_event->opening && g_config.isOpen() && a_event->menuName == RE::CursorMenu::MENU_NAME) {
				g_cursorClosed = true;
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		static CursorCloseSink& Get()
		{
			static CursorCloseSink sink;
			return sink;
		}
	};

	// Shows the Cursor Menu again once per close CursorCloseSink saw.
	// Contract: game thread, from OverlayMenu::AdvanceMovie, so only while this menu is open: the
	// close that follows this menu's own close is never undone.
	void ShowCursorMenuAgain()
	{
		if (!g_cursorClosed.exchange(false)) {
			return;
		}
		if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(RE::CursorMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
			if (!std::exchange(g_cursorReshowLogged, true)) {
				logger::info("The Cursor Menu closed while {} was open; shown again", g_config.menuName);
			}
		}
	}

	// A game menu without a movie, flagged like Tailor 3.0's own ImGui menu.
	// While it is open the game shows its cursor (Cursor Menu, depth 13, drawn
	// above this one) and switches controls to menu mode, so mouse events move
	// the cursor instead of the camera.
	class OverlayMenu final : public RE::IMenu
	{
	public:
		OverlayMenu()
		{
			using Flag = RE::UI_MENU_FLAGS;
			menuFlags.set(Flag::kUsesCursor, Flag::kUpdateUsesCursor, Flag::kUsesMenuContext, Flag::kCustomRendering,
				Flag::kDisablePauseMenu);
			inputContext.set(Context::kMenuMode);
			depthPriority = 11;
		}

		static RE::IMenu* Create() { return new OverlayMenu(); }

		void PostDisplay() override { DrawFrame(); }

		// Contract: the engine advances every open menu once a frame on the game
		// thread; Tailor 3.0 holds its preview target from CursorMenu's AdvanceMovie.
		void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override
		{
			ShowCursorMenuAgain();
			g_config.advance();
			RE::IMenu::AdvanceMovie(a_interval, a_currentTime);
		}

		RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
		{
			switch (*a_message.type) {
			case RE::UI_MESSAGE_TYPE::kShow:
				HideHud();
				ShowHiddenMenus();
				g_cursorClosed = false;  // a close seen after the last window's own close
				g_cursorReshowLogged = false;
				g_pressedInWindow.clear();
				break;
			case RE::UI_MESSAGE_TYPE::kHide:
			case RE::UI_MESSAGE_TYPE::kForceHide:
				RestoreHud();  // every way the menu closes passes here
				RestoreHiddenMenus();
				// The plugin marks its window closed before it hides the menu, so an
				// open window here means the game closed the menu on its own: close the
				// window too, or time stays frozen with nothing on screen.
				if (g_config.isOpen()) {
					SKSE::GetTaskInterface()->AddTask([] { g_config.close(); });
				}
				break;
			case RE::UI_MESSAGE_TYPE::kUserEvent:
			case RE::UI_MESSAGE_TYPE::kScaleformEvent:
				return RE::UI_MESSAGE_RESULTS::kHandled;  // input reaches ImGui through InputDispatchHook
			default:
				break;
			}
			return RE::IMenu::ProcessMessage(a_message);
		}
	};

	void SendMenuMessage(RE::UI_MESSAGE_TYPE a_type)
	{
		if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(g_config.menuName, a_type, nullptr);
		}
	}

	// --- Input ----------------------------------------------------------------

	// A button by device and ID, as g_pressedInWindow and g_owedReleases keep it.
	std::uint64_t ButtonKey(const RE::ButtonEvent& a_button)
	{
		return (static_cast<std::uint64_t>(a_button.GetDevice()) << 32) | a_button.GetIDCode();
	}

	// Records what the window took of a button event: a press, so its release stays with the
	// window too; or the release of a button the game saw go down before the window opened (the
	// hotkey, its modifier, a key held meanwhile), which the game is owed once the window has
	// closed. Without that the game holds the button down after the close (seen in game:
	// movement locked with the hotkey on the POV key, Shift held).
	void RecordButton(const RE::InputEvent& a_event)
	{
		const auto* button = a_event.AsButtonEvent();
		if (!button) {
			return;
		}
		const auto key = ButtonKey(*button);
		if (button->IsDown()) {
			g_pressedInWindow.push_back(key);
			return;
		}
		if (button->IsPressed()) {
			return;  // held
		}
		if (const auto pressed = std::ranges::find(g_pressedInWindow, key); pressed != g_pressedInWindow.end()) {
			g_pressedInWindow.erase(pressed);
			return;
		}
		g_owedReleases.push_back({ button->GetDevice(), button->GetIDCode(), button->HeldDuration(), std::chrono::steady_clock::now() });
	}

	// Gives the game the releases it is owed, one event each with the gameplay mapping of its
	// button and the time it was held up to now (a long hold, so a tap's action, the POV toggle,
	// does not fire). A release handed over while the window was open did not free the button
	// (seen in game): the game's own controls take no input then, or take it under the menu
	// mapping.
	// Contract: game thread, from the input dispatch, once the window's menu is off the stack.
	void GiveOwedReleases(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher)
	{
		auto* controls = RE::ControlMap::GetSingleton();
		if (!controls) {
			g_owedReleases.clear();
			return;
		}
		static RE::ButtonEvent* release = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, "", 0, 0.0f, 0.0f);  // kept for reuse
		if (!release) {
			g_owedReleases.clear();
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		for (const auto& owed : g_owedReleases) {
			const auto held = owed.heldSecs + std::chrono::duration<float>(now - owed.releasedAt).count();
			release->device = owed.device;
			release->SetUserEvent(RE::BSFixedString(controls->GetUserEventName(owed.idCode, owed.device)));
			release->SetIDCode(owed.idCode);
			release->GetRuntimeData().value = 0.0f;
			release->GetRuntimeData().heldDownSecs = held;
			release->next = nullptr;
			RE::InputEvent* const one[] = { release };
			func(a_dispatcher, one);
		}
		g_owedReleases.clear();
	}

	// Contract: game thread. While the window is open every event goes to the window and
	// none to the game, so the player cannot act (attack, draw, move); the window moves the
	// game's cursor itself. The dispatch still runs, with an empty list, as it does for a
	// batch with nothing to show. After the close, the first dispatch with the window's
	// menu gone first gives the game the releases it is owed.
	void InputDispatchHook::thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_events)
	{
		if (!a_events || !*a_events || !g_config.isOpen()) {
			if (!g_owedReleases.empty() && !g_config.isOpen()) {
				if (auto* ui = RE::UI::GetSingleton(); ui && !ui->IsMenuOpen(g_config.menuName)) {
					GiveOwedReleases(a_dispatcher);
				}
			}
			func(a_dispatcher, a_events);
			return;
		}
		g_config.queueInput(a_events);
		for (auto* event = *a_events; event; event = event->next) {
			RecordButton(*event);
		}
		RE::InputEvent* const none[] = { nullptr };
		func(a_dispatcher, none);
	}

	LRESULT WndProcHook::thunk(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
	{
		if (a_msg == WM_KILLFOCUS && g_renderReady) {
			ImGui::GetIO().ClearInputKeys();
		}
		return func(a_hwnd, a_msg, a_wParam, a_lParam);
	}

	// --- Install --------------------------------------------------------------

	// Whether the site of `T` holds a `call rel32` and, on AE, one that reaches `T::calleeAE` or
	// leaves the game's code: the latter is another plugin's hook on the same call (HotkeyAtlas
	// hooks the input dispatch), which this hook then calls in turn. A game build that moved
	// the call fails this, and the overlay stays off instead of crashing. On SE only the opcode
	// is checked (dMenu NG checks no callee there either). Logs a site that fails or is chained.
	template <class T>
	bool CheckSite()
	{
		const REL::Relocation<std::uintptr_t> site{ T::id, T::offset };
		const auto                            address = site.address();
		bool                                  expected = *reinterpret_cast<const std::uint8_t*>(address) == 0xE8;
		if (expected && REL::Module::IsAE()) {
			const auto target = address + 5 + *reinterpret_cast<const std::int32_t*>(address + 1);
			const auto code = REL::Module::get().segment(REL::Segment::textx);
			if (target >= code.address() && target < code.address() + code.size()) {
				expected = target == REL::Relocation<std::uintptr_t>{ REL::ID(T::calleeAE) }.address();
			} else {
				logger::info("The hook site {}+{:#x} already holds another plugin's hook; {} chains onto it", T::id.id(), T::offset.offset(),
					g_config.pluginName);
			}
		}
		if (!expected) {
			logger::error("The hook site {}+{:#x} does not hold the expected call on this game build: overlay disabled", T::id.id(), T::offset.offset());
		}
		return expected;
	}

	template <class T>
	void WriteCall(SKSE::Trampoline& a_trampoline)
	{
		const REL::Relocation<std::uintptr_t> target{ T::id, T::offset };
		T::func = a_trampoline.write_call<5>(target.address(), T::thunk);
	}
}

namespace Overlay
{
	void Install(Config a_config)
	{
		if (REL::Module::IsVR()) {
			logger::info("Skyrim VR: overlay disabled");
			return;
		}
		g_config = std::move(a_config);
		// Both sites are checked before either is written: one hook without the other would be a
		// window that cannot be drawn, or cannot be used.
		const bool d3dInitOk = CheckSite<D3DInitHook>();
		const bool inputOk = CheckSite<InputDispatchHook>();
		if (!d3dInitOk || !inputOk) {
			return;
		}

		auto& trampoline = SKSE::GetTrampoline();
		WriteCall<D3DInitHook>(trampoline);
		WriteCall<InputDispatchHook>(trampoline);
		logger::info("Hooks installed");
	}

	void RegisterMenu()
	{
		if (REL::Module::IsVR()) {
			return;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			logger::error("UI singleton missing: overlay disabled");
			return;
		}
		ui->Register(g_config.menuName.c_str(), OverlayMenu::Create);
		ui->AddEventSink<RE::MenuOpenCloseEvent>(&CursorCloseSink::Get());
		g_menuRegistered = true;
		logger::info("{} menu registered", g_config.pluginName);
	}

	void ShowMenu()
	{
		SendMenuMessage(RE::UI_MESSAGE_TYPE::kShow);
	}

	void HideMenu()
	{
		SendMenuMessage(RE::UI_MESSAGE_TYPE::kHide);
	}

	bool IsRenderReady()
	{
		return g_renderReady && g_menuRegistered;
	}
}
