# sarrai-common — Claude Code Rules

Static library shared by the user's SKSE plugins (Livery, Tableau, later others): the ImGui overlay (D3D11 init and input hooks, a movie-less game menu), the window skins (Theme: Modern / Vanilla / Nordic, palettes, fonts, widgets), the free camera, and the skins' fonts and textures (`assets/`). Each plugin takes it as the git submodule `lib/sarrai-common`, links it with `add_subdirectory` and ships `assets/` in its own `Data/SKSE/Plugins/<Mod>/` (`sarrai_install_assets`). Nothing here is released on its own.

No build of its own: build a plugin that uses it (`cmake --preset release-windows` then `cmake --build --preset release-windows` in Livery or Tableau).

## Rules

- The plugin repos' `CLAUDE.md` apply here in full (output discipline, pre-change approval, high-impact list, readability, contract comments, review). Their high-impact list covers every change to `Overlay.cpp` (hooks, per-frame and per-input code) and to `CMakeLists.txt`.
- **Every change lands in every plugin**: list which plugins use what you change, build each one, and bump its `lib/sarrai-common` submodule on a branch there. A change only one plugin needs stays in that plugin.
- **No plugin terms**: nothing here knows outfits, paintings or a plugin's settings. Pass what differs through arguments or `Overlay::Config` (names, folders, callbacks). Names that came from Livery (`OutfitRow`, `SituationHeading`, `OutfitMarks`) stay until another plugin needs them; generalize then.
- Hooks: Address Library IDs through CommonLibSSE-NG only (`REL::RelocationID`); never hard-code addresses or guess offsets / IDs. Normal play (no window open) returns early before any allocation, lookup or logging.
- `assets/`: textures are the game's, NORDIC UI's and SkyUI's art under their credits files; fonts are OFL with their licences. Keep each credits or licence file beside the files it covers.
- Docs, comments, commits: English. Conventional Commits `feat|fix|chore|refactor`, followed by hand; commit on a branch, never push unless asked.
