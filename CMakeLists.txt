cmake_minimum_required(VERSION 3.28)
project(moha VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

set(MAYA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# Prefer the in-tree submodule when present; only fall back to fetch.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/maya/CMakeLists.txt")
    add_subdirectory(maya)
else()
    FetchContent_Declare(
        maya
        GIT_REPOSITORY https://github.com/1ay1/maya.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(maya)
endif()

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json)

if(NOT TARGET maya::maya)
    add_library(maya::maya ALIAS maya)
endif()

# Treat maya's headers as system so its warnings don't surface in moha builds.
set_target_properties(maya PROPERTIES SYSTEM TRUE)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(moha
    src/main.cpp

    # ── io: HTTP / persistence / auth / diffing ──────────────────────
    src/io/anthropic.cpp
    src/io/auth.cpp
    src/io/persistence.cpp
    src/io/diff.cpp

    # ── tool: registry + definitions ─────────────────────────────────
    src/tool/registry.cpp

    # ── app: Elm-style runtime (init / update / subscribe / cmd) ─────
    src/app/deps.cpp
    src/app/init.cpp
    src/app/cmd_factory.cpp
    src/app/update.cpp
    src/app/subscribe.cpp

    # ── view: per-panel Element factories ────────────────────────────
    src/view/helpers.cpp
    src/view/thread.cpp
    src/view/composer.cpp
    src/view/statusbar.cpp
    src/view/changes.cpp
    src/view/pickers.cpp
    src/view/diff_review.cpp
    src/view/permission.cpp
    src/view/view.cpp
)

target_include_directories(moha PRIVATE include)
target_link_libraries(moha PRIVATE
    maya::maya
    nlohmann_json::nlohmann_json
    CURL::libcurl
    OpenSSL::Crypto
    Threads::Threads
)
if(WIN32)
    target_link_libraries(moha PRIVATE shell32)
endif()

if(MSVC)
    target_compile_options(moha PRIVATE /W4 /utf-8)
else()
    target_compile_options(moha PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations)
    # GCC -Wmaybe-uninitialized produces false positives on std::variant moves
    # of designated-initialized aggregates (maya's Element{TextElement{...}} pattern).
    # The warnings escape -isystem because they fire during late optimization.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(moha PRIVATE -Wno-maybe-uninitialized)
    endif()
endif()
