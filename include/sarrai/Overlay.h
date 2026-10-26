// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "sarrai/Theme.h"

namespace RE
{
	class InputEvent;
}

// Engine hooks and the game menu that give a plugin an ImGui overlay drawn
// under the game's own cursor (see Overlay.cpp). Skyrim VR is not supported:
// Install() and RegisterMenu() are no-ops there and IsRenderReady() stays
// false, so callers fall back to a UI-less path.
namespace Overlay
{
	// What the overlay asks of the plugin. Every callback is required.
	struct Config
	{
		std::string           menuName;     // the game menu's name, unique among plugins: "LiveryMenu"
		std::string           pluginName;   // in the log lines: "Livery"
		std::filesystem::path assetFolder;  // holds fonts/ and textures/ (Theme), relative to the game folder
		std::string           defaultIconKey;  // the row icon drawn for a key with no PNG (Theme::LoadTextures); "" for none

		// Contract: render thread, once, as the overlay starts: the row icons for Theme::LoadTextures.
		std::vector<Theme::IconFile> (*iconFiles)() = nullptr;

		// Whether the plugin's window is open. Called on the game thread (every input batch,
		// and as the menu hides) and the render thread (every frame the menu is shown), so it
		// must be cheap and thread-safe.
		bool (*isOpen)() = nullptr;

		// Contract: game thread, from the input dispatch, while isOpen(): the batch the
		// game will not see.
		void (*queueInput)(RE::InputEvent* const* events) = nullptr;

		// Contract: render thread, each frame the window is open, after the backends'
		// NewFrame and before ImGui::NewFrame: Theme::Apply and feeding queued input.
		void (*prepareFrame)() = nullptr;

		// Contract: render thread, between ImGui::NewFrame and ImGui::Render.
		void (*draw)() = nullptr;

		// Contract: game thread, once a frame while the menu is open (its AdvanceMovie).
		void (*advance)() = nullptr;

		// Contract: game thread (an SKSE task). The game hid the menu while isOpen() was still
		// true: the plugin must close its window, or time stays frozen with nothing on screen.
		void (*close)() = nullptr;
	};

	// Call once from SKSEPluginLoad, after SKSE::Init allocated the trampoline.
	void Install(Config config);

	// Call once at kDataLoaded: registers the menu that hosts the window.
	void RegisterMenu();

	// Contract: game thread. Queue the menu's show / hide message; the game opens
	// or closes it a frame later.
	void ShowMenu();
	void HideMenu();

	// True once ImGui has been initialised on the game's D3D11 device and the
	// menu is registered.
	bool IsRenderReady();
}
