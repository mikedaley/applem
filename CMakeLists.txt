cmake_minimum_required(VERSION 3.20)

# The oldest macOS the app runs on. Without it the compiler targets whatever
# macOS builds it, and an app built on macOS 27 will not start on anything
# older. It is set before project(), so the core is built for the same macOS
# as the app it is linked into, and set outright rather than defaulted, so a
# build directory that cached another value follows a change here.
set(A2E_MACOS_MINIMUM "15.0")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${A2E_MACOS_MINIMUM}" CACHE STRING "Oldest macOS the app runs on" FORCE)

project(applem LANGUAGES C CXX OBJC OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "ApplEm's native front end is macOS only (Metal)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The emulation core, shared with the browser build (github.com/mikedaley/applem-core).
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/core/CMakeLists.txt)
    message(FATAL_ERROR "The core is missing. Run: git submodule update --init")
endif()
add_subdirectory(core)

enable_testing()
add_subdirectory(native)
