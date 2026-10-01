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
    src/emulation.cpp
    src/key_mapper.cpp
    src/platform_paths.mm
    src/screen_surface_metal.mm
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
    "-framework AudioToolbox"
    "-framework CoreAudio"
)

# The keyboard mapping and the emulation thread's queues, without a window.
add_executable(test_native_input
    tests/test_native_input.cpp
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
