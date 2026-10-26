# sarrai-common

Shared code for SKSE plugins built with CommonLibSSE-NG: an ImGui overlay drawn under the game's cursor, three window skins (Modern, Vanilla, Nordic) in palette colours, and a free camera driven by the window. Linked into each plugin as a static library; not a mod of its own.

| Header | What it gives |
|---|---|
| `sarrai/Overlay.h` | D3D11 init and input dispatch hooks, a movie-less game menu that hosts the window (`Overlay::Config` holds the plugin's names, asset folder and callbacks) |
| `sarrai/Theme.h` | `Theme::Apply` and the skinned widgets: lists, rows, fields, dropdowns, tabs, buttons, tooltips |
| `sarrai/FreeCamera.h` | The engine's free camera, moved by the window's mouse and key input |

## Use from a plugin

```cmake
# after add_subdirectory(lib/commonlibsse-ng ...)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/lib/sarrai-common" "${CMAKE_CURRENT_BINARY_DIR}/sarrai-common")
target_link_libraries(${PROJECT_NAME} PRIVATE sarrai-common)
sarrai_install_assets("${DEPLOY_DIR}/SKSE/Plugins/<Mod>")
```

The plugin's `vcpkg.json` needs `imgui` with `dx11-binding` and `win32-binding`. `Overlay::Config::assetFolder` is `Data/SKSE/Plugins/<Mod>`.

## Licence

GPL-3.0-or-later (the Modern skin began as Tailor 3.0's, GPL-3.0). Fonts: OFL (`assets/fonts/*-OFL.txt`). Textures: see `assets/textures/*-CREDITS.txt`.
