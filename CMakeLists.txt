cmake_minimum_required(VERSION 3.28)
project(moha VERSION 0.1.0 LANGUAGES CXX)

# CMake (as of 4.2) has no /std:c++26 mapping for MSVC yet. Ask for C++23
# there and opt into /std:c++latest so MSVC 14.50+ exposes available C++26
# library bits (std::expected, std::format, etc). Other compilers get C++26.
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── MSVC Release: strip default /Ob2 so our /Ob3 doesn't trigger D9025 ──
#    CMake's default MSVC Release flags are "/MD /O2 /Ob2 /DNDEBUG". We
#    want our per-target /Ob3 (aggressive inlining) instead. We also want
#    to handle /O2 ourselves so the per-target Release flags below are
#    authoritative. /MD vs /MT is handled via CMAKE_MSVC_RUNTIME_LIBRARY
#    at the top of this file — see below.
if(MSVC)
    foreach(flag_var
            CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS_MINSIZEREL
            CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_C_FLAGS_MINSIZEREL)
        string(REGEX REPLACE "/O2" "" ${flag_var} "${${flag_var}}")
        string(REGEX REPLACE "/Ob[0-3]" "" ${flag_var} "${${flag_var}}")
    endforeach()
endif()

# ── Static CRT + whole-program optimization plumbing ───────────────────
# Opt in with -DMOHA_STATIC_RUNTIME=ON (paired with x64-windows-static
# vcpkg triplet) for a single-file exe with no CRT DLL dependency.
option(MOHA_STATIC_RUNTIME "Link the MSVC runtime statically (/MT)" OFF)
if(MOHA_STATIC_RUNTIME AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Link-time optimization — enabled by default in Release/RelWithDebInfo, off
# in Debug (would drag build times without shipping value). Supported by
# GCC, Clang, MSVC, and AppleClang; CMake picks the right flag per toolchain.
include(CheckIPOSupported)
check_ipo_supported(RESULT MOHA_HAS_IPO OUTPUT MOHA_IPO_ERR)
if(MOHA_HAS_IPO AND (CMAKE_BUILD_TYPE STREQUAL "Release"
                     OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
                     OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
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
    target_compile_options(moha PRIVATE
        /W4
        /utf-8                 # treat source and exec charsets as UTF-8
        /std:c++latest         # opt into C++26 library bits beyond /std:c++23
        /permissive-           # strict conformance
        /Zc:preprocessor       # conforming preprocessor
        /Zc:__cplusplus        # report real __cplusplus value
        /Zc:inline             # drop unreferenced COMDATs at compile time
        /Zc:throwingNew        # assume ::new never returns null
        /EHsc
        /bigobj                # maya's templates blow past default sections
        /MP                    # parallel compilation across TUs
        /wd4100                # unreferenced formal parameter — common in lambdas
        /wd4127                # conditional expression is constant (if constexpr paths)
        /wd4324                # structure padded due to alignment specifier
        # ── Release-only aggressive optimization ─────────────────────
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:
            /O2                #  max-speed optimization
            /Ob3               #  aggressive inlining beyond /Ob2
            /Oi                #  intrinsic functions
            /Ot                #  favor speed over size
            /Oy                #  omit frame pointer (frees a GPR)
            /GL                #  whole-program optimization (pairs with /LTCG)
            /GF                #  eliminate duplicate strings
            /Gy                #  function-level linking (linker /OPT:ICF/REF fodder)
            /Gw                #  package globals for linker to fold/strip
            /GS-               #  no stack-buffer cookies — TUI, not a daemon
            /GR                #  keep RTTI (context.hpp uses typeid)
            /fp:fast           #  relax FP strictness — no errno / reassociation
            /arch:AVX2         #  baseline AVX2/BMI2/FMA — Haswell+ / Zen1+
            /Qpar              #  enable auto-parallelizer for hot loops
        >
    )
    target_link_options(moha PRIVATE
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:
            /LTCG              # link-time codegen (consumes /GL .obj files)
            /OPT:REF           # remove unreferenced functions/data
            /OPT:ICF=3         # fold identical COMDATs (3 passes)
            /INCREMENTAL:NO    # full link only — smaller exe, better IPO
            /DEBUG:NONE        # no PDB in Release
        >
    )
    target_compile_definitions(moha PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:NDEBUG>
    )
else()
    target_compile_options(moha PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations)
    # GCC -Wmaybe-uninitialized produces false positives on std::variant moves
    # of designated-initialized aggregates (maya's Element{TextElement{...}} pattern).
    # The warnings escape -isystem because they fire during late optimization.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(moha PRIVATE -Wno-maybe-uninitialized)
    endif()
endif()

# Opt-in sanitizer build: `cmake -B build-san -DMOHA_SANITIZE=address,undefined`.
# Separate from CMAKE_BUILD_TYPE so CI / contributors can flip it without
# redefining the whole toolchain.
set(MOHA_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers for moha (e.g. address,undefined or thread). Empty to disable.")
if(MOHA_SANITIZE AND NOT MSVC)
    message(STATUS "moha: building with -fsanitize=${MOHA_SANITIZE}")
    target_compile_options(moha PRIVATE
        -fsanitize=${MOHA_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(moha PRIVATE -fsanitize=${MOHA_SANITIZE})
endif()
