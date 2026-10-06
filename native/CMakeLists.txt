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
    "-framework Carbon"
)

set(NATIVE_SOURCES
    src/app/main.mm
    src/app/app.cpp
    src/sound/audio_output.mm
    src/applesoft/basic_language.cpp
    src/applesoft/basic_window.cpp
    src/applesoft/code_editor.cpp
    src/debugger/cpu_debugger.cpp
    src/drives/disk_drives.cpp
    src/drives/disk_inspector_data.cpp
    src/drives/disk_platter.cpp
    src/display/display.cpp
    src/debugger/condition_rules.cpp
    src/debugger/console_command.cpp
    src/debugger/console_window.cpp
    src/debugger/debug_breakpoints.cpp
    src/develop/dev_project.cpp
    src/develop/develop.cpp
    src/debugger/debug_symbols.cpp
    src/display/display_settings.cpp
    src/sound/drive_sounds.cpp
    src/app/emulation.cpp
    src/debugger/ensoniq_window.cpp
    src/sound/equalizer.cpp
    src/sound/equalizer_window.cpp
    src/slots/expansion_slots.cpp
    src/controls/game_port.cpp
    src/drives/hard_drives.cpp
    src/drives/disk35_drives.cpp
    src/drives/disk_inspector.cpp
    src/controls/joystick.cpp
    src/controls/key_mapper.cpp
    src/drives/media_store.cpp
    src/debugger/memory_viewer.cpp
    src/debugger/rule_builder.cpp
    src/app/menu_model.cpp
    src/debugger/mockingboard_window.cpp
    src/app/native_menu.mm
    src/app/native_toolbar.mm
    src/display/no_signal_frame.cpp
    src/app/platform_paths.mm
    src/states/save_states.cpp
    src/display/screen_renderer_metal.mm
    src/slots/slot_layout.cpp
    src/debugger/soft_switch_window.cpp
    src/states/state_store.cpp
    src/ui/ui_controls.cpp
    src/ui/ui_theme.mm
    src/sound/volume_map.cpp
)

# The app's version is the one the release process bumps, the browser's, so
# every build says the same as the release it came from.
file(READ ${CMAKE_SOURCE_DIR}/src/js/config/version.js VERSION_JS)
string(REGEX MATCH "VERSION = \"([^\"]+)\"" _ "${VERSION_JS}")
set(APPLEM_VERSION "${CMAKE_MATCH_1}")
if(NOT APPLEM_VERSION)
    set(APPLEM_VERSION "${PROJECT_VERSION}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/js/config/version.js)

# When the app was built, for the About panel: a source file written on every
# build (not at configure), since a __DATE__ only moves when its file is
# recompiled.
set(BUILD_STAMP ${CMAKE_CURRENT_BINARY_DIR}/build_stamp.cpp)
add_custom_target(applem_build_stamp
    COMMAND ${CMAKE_COMMAND} -DOUT=${BUILD_STAMP} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_stamp.cmake
    BYPRODUCTS ${BUILD_STAMP}
    COMMENT "Stamping the build time")

add_executable(ApplEmNative MACOSX_BUNDLE ${NATIVE_SOURCES} ${BUILD_STAMP})
add_dependencies(ApplEmNative applem_build_stamp)
set_target_properties(ApplEmNative PROPERTIES
    OUTPUT_NAME "ApplEm"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "ApplEm"
    MACOSX_BUNDLE_GUI_IDENTIFIER "uk.co.retrotech71.applem.native"
    MACOSX_BUNDLE_BUNDLE_VERSION "${APPLEM_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${APPLEM_VERSION}"
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
    src/controls/game_port.cpp
    src/controls/key_mapper.cpp
)
target_include_directories(test_native_input PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_input PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_native_input PRIVATE a2e_core imgui)
add_test(NAME test_native_input COMMAND test_native_input
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The debugger's symbols, address parsing and breakpoint list.
add_executable(test_native_debugger
    tests/test_native_debugger.cpp
    src/debugger/condition_rules.cpp
    src/debugger/console_command.cpp
    src/debugger/debug_symbols.cpp
    src/debugger/debug_breakpoints.cpp
)
target_include_directories(test_native_debugger PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_debugger PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_native_debugger PRIVATE a2e_core)
add_test(NAME test_native_debugger COMMAND test_native_debugger
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The BASIC editor's language: colours, statements, Format, Renum, completion,
# and the text the editor takes in (which is why ImGui is linked, unused
# until a frame is drawn).
add_executable(test_native_basic
    tests/test_native_basic.cpp
    src/applesoft/basic_language.cpp
    src/applesoft/code_editor.cpp
)
target_include_directories(test_native_basic PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_basic PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_native_basic PRIVATE a2e_core imgui)
add_test(NAME test_native_basic COMMAND test_native_basic
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The output equaliser's filters, response and settings.
add_executable(test_native_equalizer
    tests/test_native_equalizer.cpp
    src/sound/equalizer.cpp
)
target_include_directories(test_native_equalizer PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_equalizer PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME test_native_equalizer COMMAND test_native_equalizer
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Renders test pictures through the CRT chain for scripts/compare-crt.mjs,
# which puts the same pictures through the browser's renderer and compares.
add_executable(crt_render
    tools/crt_render.mm
    src/display/display_settings.cpp
    src/display/no_signal_frame.cpp
    src/display/screen_renderer_metal.mm
)
target_include_directories(crt_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(crt_render PRIVATE -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(crt_render PRIVATE a2e_core imgui)

# The display settings' rules: presets, profiles, storage, the shader scales.
add_executable(test_native_display
    tests/test_native_display.cpp
    src/display/display_settings.cpp
    src/display/no_signal_frame.cpp
)
target_include_directories(test_native_display PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_display PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_native_display PRIVATE a2e_core)
add_test(NAME test_native_display COMMAND test_native_display
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The debugging console, typed into over a real machine.
add_executable(test_native_console
    tests/test_native_console.cpp
    src/drives/media_store.cpp
    src/debugger/console_command.cpp
    src/debugger/console_window.cpp
    src/debugger/cpu_debugger.cpp
    src/debugger/rule_builder.cpp
    src/debugger/condition_rules.cpp
    src/debugger/debug_symbols.cpp
    src/debugger/debug_breakpoints.cpp
    src/app/emulation.cpp
    src/sound/audio_output.mm
    src/sound/drive_sounds.cpp
    src/sound/equalizer.cpp
    src/ui/ui_controls.cpp
    src/ui/ui_theme.mm
    src/app/platform_paths.mm
    src/sound/volume_map.cpp
)
target_include_directories(test_native_console PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_console PRIVATE -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(test_native_console PRIVATE a2e_core imgui
    "-framework AudioToolbox" "-framework CoreAudio" "-framework AppKit")
add_test(NAME test_native_console COMMAND test_native_console
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The media store, the save naming rules and the seek click.
add_executable(test_native_media
    tests/test_native_media.cpp
    src/slots/slot_layout.cpp
    src/states/state_store.cpp
    src/drives/disk_drives.cpp
    src/drives/disk_inspector_data.cpp
    src/drives/disk_platter.cpp
    src/drives/hard_drives.cpp
    src/drives/disk35_drives.cpp
    src/drives/disk_inspector.cpp
    src/sound/drive_sounds.cpp
    src/drives/media_store.cpp
    src/sound/audio_output.mm
    src/app/emulation.cpp
    src/sound/equalizer.cpp
    src/ui/ui_controls.cpp
    src/ui/ui_theme.mm
    src/app/platform_paths.mm
    src/sound/volume_map.cpp
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

# A program built with the user's own tools: the project file, the build's
# output and errors, the boot volume, and starting it on a machine.
add_executable(test_native_dev
    tests/test_native_dev.cpp
    src/develop/dev_project.cpp
    src/drives/media_store.cpp
)
target_include_directories(test_native_dev PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/catch2
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(test_native_dev PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_native_dev PRIVATE a2e_core)
add_test(NAME test_native_dev COMMAND test_native_dev
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
