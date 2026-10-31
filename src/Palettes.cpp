// SPDX-License-Identifier: GPL-3.0-or-later
//
// FILE MAP
//   Tables   - the skins' names, each palette key with its colour and comment, each skin's own
//              colours and the shipped palettes
//   Files    - the palette folder, reading a palette file, writing a shipped one as shipped
//   Palettes - Load, the chosen palette, Choose and RestoreChosen
//
// A palette file is an INI with one section, [Palette]: Skin names the skin (Modern, Vanilla or
// Nordic; Modern when left out) and each other key is a colour of Theme::Palette as #RRGGBB or
// #RRGGBBAA. Only the keys the skin reads are written and read; one left out, or not a colour,
// keeps the skin's own colour. The shipped files are written by the plugin, never installed, so
// a player's edits survive an update; RestoreChosen writes one back.

#include "PCH.h"

#include "sarrai/Palettes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
	constexpr auto kPaletteSection = L"Palette";  // the section of a palette file that holds its skin and colours
	constexpr auto kPaletteSkinKey = L"Skin";     // the skin a palette file is for; Modern when left out
	constexpr auto kDefaultPalette = L"Modern";   // chosen when the file the plugin's INI names is missing

	// --- Tables ---------------------------------------------------------------

	struct SkinOption
	{
		Theme::Skin    skin;
		const char*    label;  // a dropdown and the log
		const wchar_t* name;   // a palette file's Skin value, and the file name of the skin's own palette
	};

	// The first is the default.
	constexpr std::array<SkinOption, 3> kSkinOptions{ {
		{ Theme::Skin::kModern, "Modern", L"Modern" },
		{ Theme::Skin::kVanilla, "Vanilla", L"Vanilla" },
		{ Theme::Skin::kNordic, "Nordic", L"Nordic" },
	} };

	const SkinOption& OptionOf(Theme::Skin a_skin)
	{
		for (const auto& option : kSkinOptions) {
			if (option.skin == a_skin) {
				return option;
			}
		}
		return kSkinOptions[0];
	}

	// Which skins read a palette key: a bit per Theme::Skin.
	constexpr unsigned SkinBit(Theme::Skin a_skin)
	{
		return 1u << static_cast<unsigned>(a_skin);
	}
	constexpr unsigned kAllSkins = SkinBit(Theme::Skin::kModern) | SkinBit(Theme::Skin::kVanilla) | SkinBit(Theme::Skin::kNordic);
	constexpr unsigned kNotNordic = kAllSkins & ~SkinBit(Theme::Skin::kNordic);
	constexpr unsigned kModernOnly = SkinBit(Theme::Skin::kModern);

	// A palette file's colour keys, each a colour of Theme::Palette, read only for the skins
	// that use it, with the comment a shipped file has above it: Modern's, then Vanilla's and
	// Nordic's (nullptr: Modern's).
	struct PaletteKey
	{
		const wchar_t* key;
		std::uint32_t Theme::Palette::* color;
		unsigned                        skins;
		const wchar_t*                  comment;
		const wchar_t*                  squareComment = nullptr;
	};
	constexpr std::array<PaletteKey, 12> kPaletteKeys{ {
		{ L"Window", &Theme::Palette::window, kAllSkins, L"The window." },
		{ L"Surface", &Theme::Palette::surface, kAllSkins, L"Popups, dropdown lists and tooltips." },
		{ L"Text", &Theme::Palette::text, kAllSkins, L"Text, and faint, the fills of fields, buttons and the scroll bar.",
			L"Text, a link or label under the mouse, and faint, the fills of fields, buttons and the scroll bar." },
		{ L"Label", &Theme::Palette::label, kAllSkins, L"Field and column labels, list headings, links and greyed text." },
		{ L"Emphasis", &Theme::Palette::emphasis, kAllSkins, L"Names inside greyed text or a link: this colour under the mouse, a little darker otherwise." },
		{ L"Accent", &Theme::Palette::accent, kAllSkins,
			L"Ticks, a selected row's rule, a link or label under the mouse, the dropdown arrow, the frame of a field that narrows a list.",
			L"The arrow at a selected row, ticks, and in Nordic the corner knots, the heading and tab ropes and the button frames." },
		{ L"AccentLight", &Theme::Palette::accentLight, kModernOnly, L"The text cursor, a scroll bar or slider held." },
		{ L"Rule", &Theme::Palette::rule, kAllSkins, L"The rules and frames of the window, its fields and lists." },
		{ L"Hover", &Theme::Palette::hover, kAllSkins, L"A row under the mouse.",
			L"A row under the mouse: Vanilla's glow over it, Nordic's line under it (this opacity at its left end)." },
		{ L"Selected", &Theme::Palette::selected, kNotNordic,
			L"A selected row (fading out towards its right end), the selected tab, the fill of a field that narrows a list.",
			L"The bar over a selected row, fading in from its left end to this opacity." },
		{ L"Stripe", &Theme::Palette::stripe, kAllSkins, L"Every second outfit row, laid over it." },
		{ L"Error", &Theme::Palette::error, kAllSkins, L"Error text and fields." },
	} };

	// A palette every plugin ships (WriteMissingPalettes).
	struct ShippedPalette
	{
		const wchar_t* stem;
		Theme::Skin    skin;
		const wchar_t* about;  // the file's first comment line after "; <Mod> palette: ", ending in '\n'
		Theme::Palette colours;
	};

	// Each skin's own colours: what its palette files start from, a key left out keeping them.
	// A colour the skin does not use keeps Modern's.
	constexpr Theme::Palette kModernColours{};  // Theme::Palette's defaults
	constexpr Theme::Palette kVanillaColours{
		.window = 0x000000CC,
		.surface = 0x000000EB,
		.text = 0xFFFFFFFF,
		.label = 0xA5A5A5FF,
		.emphasis = 0xB9B19AFF,
		.accent = 0xFFFFFFFF,
		.rule = 0xFFFFFF46,
		.hover = 0xFFFFFF40,
		.selected = 0xFFFFFF46,
		.stripe = 0xFFFFFF10,
		.error = 0xFF6666FF,
	};
	constexpr Theme::Palette kNordicColours{
		.window = 0x000000CC,
		.surface = 0x0A0A0AEB,
		.text = 0xF0F0F0FF,
		.label = 0xA5A5A5FF,
		.emphasis = 0xB9B19AFF,
		.accent = 0xDEDEDEFF,
		.rule = 0xC8C8C85A,
		.hover = 0xE5E5E565,
		.stripe = 0xFFFFFF10,
		.error = 0xFF6666FF,
	};
	constexpr Theme::Palette kEdgeColours{
		.window = 0x1E1E1CF0,
		.surface = 0x262522FA,
		.text = 0xDCD6C2FF,
		.label = 0x8C8A7CFF,
		.emphasis = 0xC7C3A2FF,
		.accent = 0xD2BC72FF,
		.accentLight = 0xF0DBA4FF,
		.rule = 0x4A4639FF,
		.hover = 0x423F35FF,
		.selected = 0x3C3931FF,
		.stripe = 0x00000020,
		.error = 0xFF6666FF,
	};

	constexpr std::array<ShippedPalette, 4> kShippedPalettes{ {
		{ L"Modern", Theme::Skin::kModern, L"Modern, a general-purpose dark grey that suits Untarnished UI and Norden UI.\n", kModernColours },
		{ L"Edge", Theme::Skin::kModern, L"Edge, after Edge UI (a SkyUI interface by EugeneUI): dark panels with a muted gold accent.\n", kEdgeColours },
		{ L"Vanilla", Theme::Skin::kVanilla,
			L"Vanilla, after the game's own menus. The game's art (scroll bar, checkbox, equipped mark) keeps its own colours.\n",
			kVanillaColours },
		{ L"Nordic", Theme::Skin::kNordic,
			L"Nordic, after NORDIC UI. The game's art (scroll bar, checkbox) and NORDIC UI's equipped mark keep their own colours.\n",
			kNordicColours },
	} };

	const Theme::Palette& SkinColours(Theme::Skin a_skin)
	{
		switch (a_skin) {
		case Theme::Skin::kVanilla:
			return kVanillaColours;
		case Theme::Skin::kNordic:
			return kNordicColours;
		default:
			return kModernColours;
		}
	}

	// What every shipped palette file says after its first line; {0} is the plugin's name.
	constexpr auto kPaletteHelp =
		L";\n"
		L"; A palette sets the look of {0}'s window: the skin it is drawn in and that skin's\n"
		L"; colours. Pick one in SKSE Menu Framework's {0} page, or in {0}.ini's [UI] Palette by\n"
		L"; its file name without .ini.\n"
		L";\n"
		L"; To make your own, copy this file in this folder under a new name and change its colours;\n"
		L"; it is listed the next time the game starts by its skin and file name (\"Modern - Mine\").\n"
		L"; Each colour is #RRGGBB or #RRGGBBAA, where AA is the opacity (00 clear, FF opaque). A\n"
		L"; colour left out or written otherwise keeps the skin's own.\n"
		L";\n"
		L"; {0} writes this file only when it is missing, so an update never undoes your edits;\n"
		L"; Restore original palette on the {0} page writes it back as {0} ships it.\n";
	constexpr auto kPaletteSkinComment = L"; The skin these colours are for: Modern, Vanilla or Nordic. Modern when left out.\n";

	// A palette file: the skin it is for and that skin's colours.
	struct PaletteOption
	{
		std::wstring   stem;   // the file name without .ini, as the plugin's INI holds it
		std::string    label;  // the skin's name, then " - " and the stem unless the stem is the skin's name; a dropdown and the log
		Theme::Skin    skin = Theme::Skin::kModern;
		Theme::Palette palette;
	};

	// Contract: g_folder and g_modName are set once by Load, read-only afterwards. g_palettes is
	// filled once by Load, before the render thread or a settings page reads it; no palette is
	// added or removed afterwards, so the size and each stem are fixed. RestoreChosen changes a
	// palette's label, skin and colours and Choose the choice, under g_paletteMutex, which any
	// other thread holds to read them.
	std::filesystem::path      g_folder;  // absolute
	std::wstring               g_modName;
	std::vector<PaletteOption> g_palettes;          // sorted by label
	std::size_t                g_paletteIndex = 0;  // into g_palettes
	std::mutex                 g_paletteMutex;

	// --- Files ----------------------------------------------------------------

	// "#RRGGBB" or "#RRGGBBAA" as 0xRRGGBBAA, opaque when AA is left out; nullopt for anything else.
	std::optional<std::uint32_t> ParseColor(std::wstring_view a_text)
	{
		constexpr std::size_t kRgbLength = 7;   // "#RRGGBB"
		constexpr std::size_t kRgbaLength = 9;  // "#RRGGBBAA"

		if (a_text.empty() || a_text.front() != L'#' || (a_text.size() != kRgbLength && a_text.size() != kRgbaLength)) {
			return std::nullopt;
		}
		std::uint32_t value = 0;
		for (const wchar_t c : a_text.substr(1)) {
			std::uint32_t digit = 0;
			if (c >= L'0' && c <= L'9') {
				digit = c - L'0';
			} else if (c >= L'a' && c <= L'f') {
				digit = c - L'a' + 10;
			} else if (c >= L'A' && c <= L'F') {
				digit = c - L'A' + 10;
			} else {
				return std::nullopt;
			}
			value = (value << 4) | digit;
		}
		return a_text.size() == kRgbLength ? (value << 8) | 0xFF : value;
	}

	std::filesystem::path PalettePath(std::wstring_view a_stem)
	{
		return g_folder / (std::wstring{ a_stem } + L".ini");
	}

	// The shipped palette under the file name `a_stem` (ASCII case ignored), or nullptr.
	const ShippedPalette* FindShippedPalette(std::wstring_view a_stem)
	{
		for (const auto& shipped : kShippedPalettes) {
			if (::_wcsicmp(shipped.stem, std::wstring{ a_stem }.c_str()) == 0) {
				return &shipped;
			}
		}
		return nullptr;
	}

	// Whether `a_palette`, read from the file named like `a_shipped`, differs from it in its skin
	// or in a colour that skin reads: what RestoreChosen would change.
	bool PaletteEdited(const PaletteOption& a_palette, const ShippedPalette& a_shipped)
	{
		if (a_palette.skin != a_shipped.skin) {
			return true;
		}
		for (const auto& entry : kPaletteKeys) {
			if ((entry.skins & SkinBit(a_shipped.skin)) != 0 && a_palette.palette.*entry.color != a_shipped.colours.*entry.color) {
				return true;
			}
		}
		return false;
	}

	// Writes `a_shipped` to `a_path` as the plugin ships it, over any file there: its skin, then
	// the colour keys that skin reads. False (logged) when the file cannot be written.
	bool WriteShippedPalette(const std::filesystem::path& a_path, const ShippedPalette& a_shipped)
	{
		const bool     modern = a_shipped.skin == Theme::Skin::kModern;
		std::wofstream file{ a_path };
		if (file) {
			file << L"; " << g_modName << L" palette: " << a_shipped.about << std::format(kPaletteHelp, g_modName);
			file << L"\n[" << kPaletteSection << L"]\n";
			file << kPaletteSkinComment << kPaletteSkinKey << L'=' << OptionOf(a_shipped.skin).name << L'\n';
			for (const auto& entry : kPaletteKeys) {
				if ((entry.skins & SkinBit(a_shipped.skin)) == 0) {
					continue;
				}
				const auto* comment = modern || !entry.squareComment ? entry.comment : entry.squareComment;
				file << L"; " << comment << L'\n' << entry.key << L'=' << std::format(L"#{:08X}", a_shipped.colours.*entry.color) << L'\n';
			}
			file.flush();
		}
		if (!file) {
			logger::warn("Could not write {}", a_path.string());
			return false;
		}
		return true;
	}

	// Writes each shipped palette whose file is missing; a file already there, the player's
	// edits included, stays as it is.
	// Contract: called once by Palettes::Load, before LoadPalettes.
	void WriteMissingPalettes()
	{
		std::error_code ec;
		std::filesystem::create_directories(g_folder, ec);
		for (const auto& shipped : kShippedPalettes) {
			const auto path = PalettePath(shipped.stem);
			if (std::filesystem::exists(path, ec) || ec) {
				continue;
			}
			if (WriteShippedPalette(path, shipped)) {
				logger::info("Wrote the missing palette {}", path.filename().string());
			}
		}
	}

	// A palette file's Skin; left out, Modern; anything else, Modern (logged).
	Theme::Skin ReadPaletteSkin(const std::filesystem::path& a_path, std::string_view a_name)
	{
		wchar_t    value[32]{};
		const auto length = ::GetPrivateProfileStringW(kPaletteSection, kPaletteSkinKey, L"", value, static_cast<DWORD>(std::size(value)), a_path.c_str());
		if (length == 0) {
			return Theme::Skin::kModern;
		}
		for (const auto& option : kSkinOptions) {
			if (::_wcsicmp(value, option.name) == 0) {
				return option.skin;
			}
		}
		logger::warn("Palette {}: Skin = {} is not Modern, Vanilla or Nordic; it is drawn in Modern", a_name, SKSE::stl::utf16_to_utf8(value).value_or(""));
		return Theme::Skin::kModern;
	}

	// A palette file's skin and colours, and its label. Only the colour keys the skin reads are
	// read, from the skin's own colours: one left out, or not a colour (logged), keeps the skin's.
	PaletteOption ReadPalette(const std::filesystem::path& a_path)
	{
		PaletteOption option;
		option.stem = a_path.stem().wstring();
		const auto name = SKSE::stl::utf16_to_utf8(option.stem).value_or("?");
		option.skin = ReadPaletteSkin(a_path, name);
		option.palette = SkinColours(option.skin);
		const auto& skin = OptionOf(option.skin);
		option.label = ::_wcsicmp(option.stem.c_str(), skin.name) == 0 ? std::string{ skin.label } : std::format("{} - {}", skin.label, name);

		wchar_t value[32]{};
		for (const auto& entry : kPaletteKeys) {
			if ((entry.skins & SkinBit(option.skin)) == 0) {
				continue;
			}
			const auto length = ::GetPrivateProfileStringW(kPaletteSection, entry.key, L"", value, static_cast<DWORD>(std::size(value)), a_path.c_str());
			if (length == 0) {
				continue;
			}
			if (const auto color = ParseColor(std::wstring_view{ value, length })) {
				option.palette.*entry.color = *color;
			} else {
				logger::warn("Palette {}: {} = {} is not #RRGGBB or #RRGGBBAA; it keeps the skin's own colour", name,
					SKSE::stl::utf16_to_utf8(entry.key).value_or(""), SKSE::stl::utf16_to_utf8(value).value_or(""));
			}
		}
		return option;
	}

	// The palette whose file is named `a_stem` (ASCII case ignored), if any.
	std::optional<std::size_t> FindPalette(std::wstring_view a_stem)
	{
		for (std::size_t i = 0; i < g_palettes.size(); ++i) {
			if (::_wcsicmp(g_palettes[i].stem.c_str(), std::wstring{ a_stem }.c_str()) == 0) {
				return i;
			}
		}
		return std::nullopt;
	}

	// Reads every .ini in the palette folder, then picks the one named `a_chosen`, else Modern,
	// else the first. With no palette file, Palettes::Skin and Palette are Modern's.
	// Contract: called once by Palettes::Load, before any other thread reads g_palettes.
	void LoadPalettes(std::wstring_view a_chosen)
	{
		std::error_code ec;
		for (std::filesystem::directory_iterator it{ g_folder, ec }, end; !ec && it != end; it.increment(ec)) {
			const auto& path = it->path();
			if (!it->is_regular_file(ec) || ::_wcsicmp(path.extension().c_str(), L".ini") != 0) {
				continue;
			}
			g_palettes.push_back(ReadPalette(path));
		}
		std::ranges::sort(g_palettes, [](const PaletteOption& a_left, const PaletteOption& a_right) {
			return ::_stricmp(a_left.label.c_str(), a_right.label.c_str()) < 0;
		});
		logger::info("Loaded {} palette(s) from {}", g_palettes.size(), g_folder.string());
		if (g_palettes.empty()) {
			logger::info("Palette = Modern (built in)");
			return;
		}

		auto index = FindPalette(a_chosen);
		if (!index) {
			index = FindPalette(kDefaultPalette);
			logger::warn("No palette file named {}; using {}", SKSE::stl::utf16_to_utf8(a_chosen).value_or("?"), g_palettes[index.value_or(0)].label);
		}
		g_paletteIndex = index.value_or(0);
		logger::info("Palette = {}", g_palettes[g_paletteIndex].label);
	}
}

// --- Palettes -----------------------------------------------------------------

namespace Palettes
{
	void Load(const std::filesystem::path& a_folder, std::wstring_view a_chosen, std::wstring_view a_modName)
	{
		// The game's working directory is the Skyrim folder (MO2's virtual Data included); an
		// absolute path keeps the profile API from searching the Windows folder.
		g_folder = std::filesystem::current_path() / a_folder;
		g_modName = a_modName;
		WriteMissingPalettes();
		LoadPalettes(a_chosen);
	}

	Theme::Skin Skin()
	{
		std::scoped_lock lock{ g_paletteMutex };
		return g_palettes.empty() ? Theme::Skin::kModern : g_palettes[g_paletteIndex].skin;
	}

	Theme::Palette Palette()
	{
		std::scoped_lock lock{ g_paletteMutex };
		return g_palettes.empty() ? Theme::Palette{} : g_palettes[g_paletteIndex].palette;  // no file: Modern's colours
	}

	Choices List()
	{
		Choices          choices;
		std::scoped_lock lock{ g_paletteMutex };
		if (g_palettes.empty()) {
			return choices;
		}
		choices.labels.reserve(g_palettes.size());
		for (const auto& palette : g_palettes) {
			choices.labels.push_back(palette.label);
		}
		const auto& chosen = g_palettes[g_paletteIndex];
		choices.chosen = g_paletteIndex;
		choices.file = SKSE::stl::utf16_to_utf8(chosen.stem).value_or("?");
		const auto* shipped = FindShippedPalette(chosen.stem);
		choices.edited = shipped && PaletteEdited(chosen, *shipped);
		return choices;
	}

	std::optional<std::wstring> Choose(std::size_t a_index, const char* a_source)
	{
		std::wstring stem;
		std::string  label;
		{
			std::scoped_lock lock{ g_paletteMutex };
			if (a_index >= g_palettes.size() || a_index == g_paletteIndex) {
				return std::nullopt;
			}
			g_paletteIndex = a_index;
			stem = g_palettes[a_index].stem;
			label = g_palettes[a_index].label;
		}
		logger::info("Palette = {} ({})", label, a_source);
		return stem;
	}

	void RestoreChosen(const char* a_source)
	{
		std::size_t  index = 0;
		std::wstring stem;
		{
			std::scoped_lock lock{ g_paletteMutex };
			if (g_palettes.empty()) {
				return;
			}
			index = g_paletteIndex;
			stem = g_palettes[index].stem;
		}
		const auto* shipped = FindShippedPalette(stem);
		const auto  path = PalettePath(stem);
		if (!shipped || !WriteShippedPalette(path, *shipped)) {
			return;
		}
		auto restored = ReadPalette(path);
		logger::info("Restored palette {} as shipped ({})", restored.label, a_source);
		std::scoped_lock lock{ g_paletteMutex };
		g_palettes[index] = std::move(restored);
	}
}
