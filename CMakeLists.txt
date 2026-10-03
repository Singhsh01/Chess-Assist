cmake_minimum_required(VERSION 3.16)

project(chess-assist VERSION 1.0.0 DESCRIPTION "3D chess board with a live Stockfish assistant" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif ()

option(CHESS_ASSIST_BUILD_TESTS "Build the C++ unit tests" ON)

# --- Dependencies ------------------------------------------------------------
# Prefer system packages (Homebrew: `brew install glfw glm`, apt:
# `libglfw3-dev libglm-dev`, or vcpkg); fall back to fetching sources.
include(FetchContent)

find_package(glm CONFIG QUIET)
if (NOT TARGET glm::glm)
    message(STATUS "glm not found - fetching")
    FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
    FetchContent_MakeAvailable(glm)
endif ()

find_package(glfw3 CONFIG QUIET)
if (NOT TARGET glfw)
    message(STATUS "glfw not found - fetching")
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
    FetchContent_MakeAvailable(glfw)
endif ()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# --- Warnings (from upstream chess-3d) ----------------------------------------
if (MSVC)
    set(PROJECT_WARNINGS_CXX /W4 /permissive-)
else ()
    set(PROJECT_WARNINGS_CXX -Wall -Wextra -Wnon-virtual-dtor -Wcast-align
            -Wunused -Woverloaded-virtual -Wpedantic
            -Wformat=2 -Wimplicit-fallthrough)
endif ()

set(BIN_DIR ${CMAKE_SOURCE_DIR}/bin)

# --- Core library: rules, SAN parser, Stockfish bridge (no graphics) ----------
add_library(chess_core STATIC
        src/board.cpp
        src/log.cpp
        src/BoardManager.cpp
        src/StockfishEngine.cpp)
target_include_directories(chess_core PUBLIC include external/include)
target_link_libraries(chess_core PUBLIC glm::glm Threads::Threads)
target_compile_options(chess_core PRIVATE ${PROJECT_WARNINGS_CXX})

# --- Dear ImGui (vendored, v1.92.9b) ------------------------------------------
add_library(imgui STATIC
        external/imgui/imgui.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_tables.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/backends/imgui_impl_glfw.cpp
        external/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC external/imgui external/imgui/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# --- Application ----------------------------------------------------------------
add_executable(chess-assist
        src/main.cpp
        src/game.cpp
        src/camera.cpp
        src/renderer.cpp
        src/RenderPipeline.cpp
        src/WinningAssistantHUD.cpp
        src/AutomationServer.cpp)
# Single-header third-party implementations (glad, stb, cgltf) - no warnings.
add_library(third_party STATIC src/third_party.cpp)
target_include_directories(third_party PUBLIC external/include)
if (NOT MSVC)
    target_compile_options(third_party PRIVATE -w)  # vendored code: silence warnings
endif ()
target_link_libraries(chess-assist PRIVATE chess_core imgui third_party glfw OpenGL::GL)
target_compile_options(chess-assist PRIVATE ${PROJECT_WARNINGS_CXX})
target_compile_definitions(chess-assist PRIVATE
        CHESS_ASSIST_VERSION="${PROJECT_VERSION}"
        CHESS_ASSIST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
        CHESS_ASSIST_WEB_DIR="${CMAKE_SOURCE_DIR}/web")
if (WIN32)
    target_link_libraries(chess-assist PRIVATE ws2_32)
endif ()
# Recreate bin/ if it was deleted after configuring.
add_custom_command(TARGET chess-assist PRE_LINK COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR})
set_target_properties(chess-assist PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${BIN_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${BIN_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${BIN_DIR})

# --- Tests ------------------------------------------------------------------------
if (CHESS_ASSIST_BUILD_TESTS)
    enable_testing()
    add_executable(chess-assist-tests tests/core_tests.cpp)
    target_link_libraries(chess-assist-tests PRIVATE chess_core)
    target_compile_options(chess-assist-tests PRIVATE ${PROJECT_WARNINGS_CXX})
    set_target_properties(chess-assist-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${BIN_DIR})
    add_custom_command(TARGET chess-assist-tests PRE_LINK COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR})
    add_test(NAME core_tests COMMAND chess-assist-tests)
endif ()

# --- Install: bin/chess-assist + resources/ + web/ (found via <exe>/../) ------
include(GNUInstallDirs)
install(TARGETS chess-assist RUNTIME DESTINATION bin)
install(DIRECTORY resources web DESTINATION .)
install(FILES LICENSE THIRD_PARTY_NOTICES.md README.md DESTINATION share/doc/chess-assist)
