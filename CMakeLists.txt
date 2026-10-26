cmake_minimum_required(VERSION 3.21)

project(
    sarrai-common
    VERSION 0.0.1
    DESCRIPTION "Shared ImGui overlay, skins and free camera for SKSE plugins"
    LANGUAGES CXX)

# Built only inside a plugin's build: the plugin adds CommonLibSSE-NG (its
# CommonLibSSE::CommonLibSSE target) first, then add_subdirectory(lib/sarrai-common).
if(NOT TARGET CommonLibSSE::CommonLibSSE)
    message(FATAL_ERROR "sarrai-common: add CommonLibSSE-NG before add_subdirectory(lib/sarrai-common)")
endif()

# Dear ImGui with the Win32 + DX11 backends (the plugin's vcpkg.json lists imgui with
# dx11-binding and win32-binding).
find_package(imgui CONFIG REQUIRED)

add_library(sarrai-common STATIC src/FreeCamera.cpp src/Overlay.cpp src/Theme.cpp)

target_compile_features(sarrai-common PUBLIC cxx_std_23)
target_include_directories(
    sarrai-common
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_precompile_headers(sarrai-common PRIVATE src/PCH.h)

# imgui's vcpkg port does not link the system libraries its backends need.
# windowscodecs (WIC) decodes the skin textures and row icons (src/Theme.cpp).
target_link_libraries(sarrai-common PUBLIC CommonLibSSE::CommonLibSSE imgui::imgui d3d11 dwmapi imm32 windowscodecs)

if(MSVC)
    target_compile_options(sarrai-common PRIVATE /Zi "$<$<CONFIG:Release>:/Zc:inline;/JMC->")
endif()

# sarrai_install_assets(<folder>): installs the skins' fonts (with their OFL licences) and
# textures (with their credits) into <folder>/fonts and <folder>/textures, the folder the plugin
# passes to Overlay::Config::assetFolder: "${DEPLOY_DIR}/SKSE/Plugins/Livery".
function(sarrai_install_assets folder)
    get_target_property(_source sarrai-common SOURCE_DIR)
    install(DIRECTORY "${_source}/assets/" DESTINATION "${folder}")
endfunction()
