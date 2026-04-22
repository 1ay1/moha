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

# simdjson — used on the SSE hot path (content_block_delta) where we parse
# hundreds of small JSON docs per second during streaming. nlohmann is fine
# for config + cold events; simdjson's ondemand API is 3–5× faster for the
# "open doc, read two fields, throw away" pattern that dominates here.
set(SIMDJSON_DEVELOPER_MODE   OFF CACHE INTERNAL "")
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.10.1
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json simdjson)

if(NOT TARGET maya::maya)
    add_library(maya::maya ALIAS maya)
endif()

# Treat maya's headers as system so its warnings don't surface in moha builds.
set_target_properties(maya PROPERTIES SYSTEM TRUE)

# Same treatment for simdjson's headers — upstream's `operator "" _padded`
# trips -Wdeprecated-literal-operator under C++23, and it isn't our bug to
# fix. SYSTEM suppresses diagnostics from the included headers.
if(TARGET simdjson)
    set_target_properties(simdjson PROPERTIES SYSTEM TRUE)
endif()
if(TARGET simdjson_static)
    set_target_properties(simdjson_static PROPERTIES SYSTEM TRUE)
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# nghttp2 — HTTP/2 protocol engine for the in-house http client. Prefer the
# upstream CMake config (vcpkg / Homebrew / nghttp2's own export); fall back
# to pkg-config (Linux distros), then a manual find_path/find_library scan.
find_package(nghttp2 CONFIG QUIET)
if(NOT TARGET nghttp2::nghttp2)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(NGHTTP2 IMPORTED_TARGET libnghttp2)
        if(TARGET PkgConfig::NGHTTP2)
            add_library(nghttp2::nghttp2 ALIAS PkgConfig::NGHTTP2)
        endif()
    endif()
endif()
if(NOT TARGET nghttp2::nghttp2)
    find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h)
    find_library(NGHTTP2_LIBRARY NAMES nghttp2 nghttp2_static)
    if(NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
        add_library(nghttp2::nghttp2 UNKNOWN IMPORTED)
        set_target_properties(nghttp2::nghttp2 PROPERTIES
            IMPORTED_LOCATION "${NGHTTP2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP2_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR
            "nghttp2 not found — install libnghttp2-dev (Debian/Ubuntu), "
            "nghttp2 (Homebrew/Arch), or vcpkg install nghttp2.")
    endif()
endif()

# ── Source groups, by domain ──────────────────────────────────────────────
# The header tree under include/moha/ mirrors this grouping one-for-one:
#   domain/   — pure value types (no I/O, no UI). Headers only.
#   io/       — sockets, TLS, HTTP/2, OAuth, on-disk persistence.
#   provider/ — wire-format adapters for upstream LLM APIs.
#   diff/     — unified-diff parser used by the edit tool & review modal.
#   tool/     — the agent's capability surface: registry + per-tool impls.
#   runtime/  — the Elm-style app: model, update, subscriptions, view tree.

set(MOHA_IO_SOURCES
    src/io/http.cpp
    src/io/tls.cpp
    src/io/auth.cpp
    src/io/persistence.cpp
)

set(MOHA_PROVIDER_SOURCES
    src/provider/anthropic/transport.cpp
)

set(MOHA_DIFF_SOURCES
    src/diff/diff.cpp
)

set(MOHA_TOOL_SOURCES
    src/tool/registry.cpp
    src/tool/util/utf8.cpp
    src/tool/util/fs_helpers.cpp
    src/tool/util/glob_match.cpp
    src/tool/util/subprocess.cpp
    src/tool/util/bash_validate.cpp
    src/tool/util/arg_reader.cpp
    src/tool/util/partial_json.cpp
    src/tool/util/fuzzy_match.cpp
    src/tool/tools/read.cpp
    src/tool/tools/write.cpp
    src/tool/tools/edit.cpp
    src/tool/tools/bash.cpp
    src/tool/tools/grep.cpp
    src/tool/tools/glob.cpp
    src/tool/tools/list_dir.cpp
    src/tool/tools/todo.cpp
    src/tool/tools/web_fetch.cpp
    src/tool/tools/web_search.cpp
    src/tool/tools/find_definition.cpp
    src/tool/tools/diagnostics.cpp
    src/tool/tools/git.cpp
)

set(MOHA_RUNTIME_SOURCES
    src/runtime/main.cpp

    src/runtime/app/deps.cpp
    src/runtime/app/init.cpp
    src/runtime/app/cmd_factory.cpp
    src/runtime/app/update.cpp
    src/runtime/app/subscribe.cpp

    src/runtime/view/helpers.cpp
    src/runtime/view/thread.cpp
    src/runtime/view/composer.cpp
    src/runtime/view/statusbar.cpp
    src/runtime/view/changes.cpp
    src/runtime/view/pickers.cpp
    src/runtime/view/diff_review.cpp
    src/runtime/view/permission.cpp
    src/runtime/view/view.cpp
)

add_executable(moha
    ${MOHA_IO_SOURCES}
    ${MOHA_PROVIDER_SOURCES}
    ${MOHA_DIFF_SOURCES}
    ${MOHA_TOOL_SOURCES}
    ${MOHA_RUNTIME_SOURCES}
)

target_include_directories(moha PRIVATE include)
target_link_libraries(moha PRIVATE
    maya::maya
    nlohmann_json::nlohmann_json
    simdjson::simdjson
    nghttp2::nghttp2
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
if(WIN32)
    # ws2_32   — Winsock2 (sockets, WSAPoll)
    # crypt32  — CertOpenSystemStoreW for the Windows root cert loader
    # shell32  — already needed by existing code paths
    target_link_libraries(moha PRIVATE ws2_32 crypt32 shell32)
endif()
if(APPLE)
    # macOS root cert loader uses the Security and CoreFoundation frameworks.
    target_link_libraries(moha PRIVATE
        "-framework Security"
        "-framework CoreFoundation")
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

# ── Profile-guided optimization (opt-in, two phases) ────────────────────
# Phase 1: `cmake -B build-pgogen -DMOHA_PGO=generate` → build + run the
#          app through a realistic workload (a couple of chat turns + tool
#          calls is enough; the counters are written on exit).
# Phase 2: `cmake -B build-pgouse -DMOHA_PGO=use` → rebuild using the
#          counters — typical gain on streaming + layout hot paths is
#          8–12% wall-clock, 3–6% binary size on MSVC/GCC/Clang.
# The profile data directory is kept out of build/ so switching generators
# or blowing away build/ doesn't lose your collected profile.
set(MOHA_PGO "" CACHE STRING
    "Profile-guided optimization: 'generate', 'use', or empty.")
set(MOHA_PGO_DIR "${CMAKE_SOURCE_DIR}/pgo-data" CACHE PATH
    "Directory where PGO profile counters live across generate/use phases.")
if(MOHA_PGO)
    file(MAKE_DIRECTORY "${MOHA_PGO_DIR}")
    if(MSVC)
        if(MOHA_PGO STREQUAL "generate")
            target_link_options(moha PRIVATE "/GENPROFILE:PGD=${MOHA_PGO_DIR}/moha.pgd")
        elseif(MOHA_PGO STREQUAL "use")
            target_link_options(moha PRIVATE "/USEPROFILE:PGD=${MOHA_PGO_DIR}/moha.pgd")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(MOHA_PGO STREQUAL "generate")
            target_compile_options(moha PRIVATE -fprofile-instr-generate)
            target_link_options(moha PRIVATE -fprofile-instr-generate)
        elseif(MOHA_PGO STREQUAL "use")
            # Expect `llvm-profdata merge -output=moha.profdata ${MOHA_PGO_DIR}/*.profraw`
            # to be run between the two builds.
            target_compile_options(moha PRIVATE "-fprofile-instr-use=${MOHA_PGO_DIR}/moha.profdata")
            target_link_options(moha PRIVATE "-fprofile-instr-use=${MOHA_PGO_DIR}/moha.profdata")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(MOHA_PGO STREQUAL "generate")
            target_compile_options(moha PRIVATE "-fprofile-generate=${MOHA_PGO_DIR}")
            target_link_options(moha PRIVATE "-fprofile-generate=${MOHA_PGO_DIR}")
        elseif(MOHA_PGO STREQUAL "use")
            target_compile_options(moha PRIVATE
                "-fprofile-use=${MOHA_PGO_DIR}" -fprofile-correction)
            target_link_options(moha PRIVATE "-fprofile-use=${MOHA_PGO_DIR}")
        endif()
    endif()
    message(STATUS "moha: PGO phase = ${MOHA_PGO} (data dir: ${MOHA_PGO_DIR})")
endif()
