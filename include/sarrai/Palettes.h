// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sarrai/Theme.h"

// The palette files of a plugin's window: each names the skin it is drawn in and that skin's
// colours (the file format is in Palettes.cpp). The files live in a folder of the plugin's own;
// the plugin keeps the chosen one's file name in its INI and passes it to Load.
namespace Palettes
{
	// What a Window skin dropdown shows: every palette's label, sorted, the chosen one, its file
	// name without .ini, and whether RestoreChosen would change it (a shipped palette whose file
	// differs from what the plugin ships). No labels with no palette file.
	struct Choices
	{
		std::vector<std::string> labels;
		std::size_t              chosen = 0;  // into labels
		std::string              file;
		bool                     edited = false;
	};

	// Writes the shipped palettes (Modern, Edge, Vanilla, Nordic) missing in `folder` (relative
	// to the game folder), naming the plugin `modName` in their comments, then reads every .ini
	// there and picks `chosen` (a file name without .ini, ASCII case ignored), else Modern, else
	// the first. With no palette file, Skin and Palette are Modern's.
	// Contract: once, before any other thread calls in here. The files' comments tell the player
	// that `<modName>.ini`'s [UI] Palette picks the palette, so the plugin keeps it there.
	void Load(const std::filesystem::path& folder, std::wstring_view chosen, std::wstring_view modName);

	// The chosen palette's skin and colours. Safe from any thread.
	Theme::Skin    Skin();
	Theme::Palette Palette();

	// Safe from any thread.
	Choices List();

	// Choose picks the palette at `index` into List().labels and returns its file name without
	// .ini for the plugin to write to its INI, or nullopt when nothing changed. RestoreChosen
	// writes the chosen shipped palette's file back as the plugin ships it and rereads it (a file
	// that cannot be written changes nothing). A window Theme::Apply builds from Skin and Palette
	// draws in the result from its next frame. `source` names the caller in the log.
	// Contract: a render thread (a settings page's or the window's).
	std::optional<std::wstring> Choose(std::size_t index, const char* source);
	void                        RestoreChosen(const char* source);
}
