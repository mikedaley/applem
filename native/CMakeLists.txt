# native/CMakeLists.txt - The native macOS front end
#
# Dear ImGui's docking branch, its Cocoa platform backend and its Metal
# renderer, around the shared core (a2e_core). Built from the top-level
# CMakeLists.txt with -DA2E_BUILD_NATIVE=ON; `npm run native:build` does that.

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.h)
    message(FATAL_ERROR
        "Dear ImGui is missing. Run: git submodule update --init native/third_party/imgui")
endif()

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_metal.mm
    ${IMGUI_DIR}/backends/imgui_impl_osx.mm
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(imgui PUBLIC
    "-framework Cocoa"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework GameController"
)

set(NATIVE_SOURCES
    src/main.mm
    src/app.cpp
    src/audio_output.mm
    src/disk_drives.cpp
    src/display.cpp
    src/display_settings.cpp
    src/drive_sounds.cpp
    src/emulation.cpp
    src/expansion_slots.cpp
    src/game_port.cpp
    src/hard_drives.cpp
    src/joystick.cpp
    src/key_mapper.cpp
    src/media_store.cpp
    src/menu_model.cpp
    src/native_menu.mm
    src/native_toolbar.mm
    src/no_signal_frame.cpp
    src/platform_paths.mm
    src/save_states.cpp
    src/screen_renderer_metal.mm
    src/slot_layout.cpp
    src/state_store.cpp
    src/ui_controls.cpp
    src/ui_theme.mm
)

add_executable(ApplEmNative MACOSX_BUNDLE ${NATIVE_SOURCES})
set_target_properties(ApplEmNative PROPERTIES
    OUTPUT_NAME "ApplEm"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "ApplEm"
    MACOSX_BUNDLE_GUI_IDENTIFIER "uk.co.retrotech71.applem.native"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "icon.icns"
)

# The CRT shader is compiled when the app starts, from the bundle's copy, so
# building needs no Metal toolchain.
set(CRT_SHADER ${CMAKE_CURRENT_SOURCE_DIR}/shaders/crt.metal)
target_sources(ApplEmNative PRIVATE ${CRT_SHADER})
set_source_files_properties(${CRT_SHADER} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
    HEADER_FILE_ONLY TRUE)

# The browser build's disk library, offered under Recent.
file(GLOB DISK_LIBRARY ${CMAKE_SOURCE_DIR}/public/disks/*)
foreach(disk ${DISK_LIBRARY})
    target_sources(ApplEmNative PRIVATE ${disk})
    set_source_files_properties(${disk} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources/disks
        HEADER_FILE_ONLY TRUE)
endforeach()

# The Tauri build's icon is the app's icon.
set(APP_ICON ${CMAKE_SOURCE_DIR}/src-tauri/icons/icon.icns)
if(EXISTS ${APP_ICON})
    target_sources(ApplEmNative PRIVATE ${APP_ICON})
    set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

target_include_directories(ApplEmNative PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The core's headers name parameters an interface's default ignores, which
# -Wextra reports in every file that includes them.
target_compile_options(ApplEmNative PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_link_libraries(ApplEmNative PRIVATE
    a2e_core
    imgui
    "-framework UniformTypeIdentifiers"
    "-framework AudioToolbox"
    "-framework CoreAudio"
)

# The keyboard mapping and the emulation thread's queues, without a window.
add_executable(test_native_input
    tests/test_native_input.cpp
    src/game_port.cpp
    src/key_mapper.cpp
)
target_include_directories(test_native_input PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_input PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_native_input PRIVATE a2e_core imgui)
add_test(NAME test_native_input COMMAND test_native_input
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Renders test pictures through the CRT chain for scripts/compare-crt.mjs,
# which puts the same pictures through the browser's renderer and compares.
add_executable(crt_render
    tools/crt_render.mm
    src/display_settings.cpp
    src/no_signal_frame.cpp
    src/screen_renderer_metal.mm
)
target_include_directories(crt_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(crt_render PRIVATE -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(crt_render PRIVATE a2e_core imgui)

# The display settings' rules: presets, profiles, storage, the shader scales.
add_executable(test_native_display
    tests/test_native_display.cpp
    src/display_settings.cpp
    src/no_signal_frame.cpp
)
target_include_directories(test_native_display PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_display PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_native_display PRIVATE a2e_core)
add_test(NAME test_native_display COMMAND test_native_display
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The media store, the save naming rules and the seek click.
add_executable(test_native_media
    tests/test_native_media.cpp
    src/slot_layout.cpp
    src/state_store.cpp
    src/disk_drives.cpp
    src/hard_drives.cpp
    src/drive_sounds.cpp
    src/media_store.cpp
    src/audio_output.mm
    src/emulation.cpp
    src/ui_controls.cpp
    src/ui_theme.mm
    src/platform_paths.mm
)
target_include_directories(test_native_media PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_media PRIVATE -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(test_native_media PRIVATE a2e_core imgui
    "-framework AudioToolbox" "-framework CoreAudio")
add_test(NAME test_native_media COMMAND test_native_media
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
