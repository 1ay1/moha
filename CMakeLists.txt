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

# ── Standalone binary plumbing ─────────────────────────────────────────
# MOHA_STANDALONE=ON produces a binary with no third-party shared-library
# dependencies — drop it on any compatible machine (matching libc
# version) and it runs. On every platform it forces the right static-
# linking knobs:
#
#   Linux      OpenSSL + nghttp2 statically linked. libstdc++ and libgcc
#              folded in via -static-libstdc++ / -static-libgcc. libc
#              stays dynamic (fully-static glibc breaks the NSS resolver
#              and DNS lookups; if you need a 100% static binary, build
#              against musl with -DMOHA_FULLY_STATIC=ON).
#   macOS      OpenSSL + nghttp2 statically linked. libSystem stays
#              dynamic (the only ABI Apple supports for distribution).
#   Windows    Forces MOHA_STATIC_RUNTIME=ON (/MT) so the MSVC CRT is
#              statically embedded; nghttp2 + OpenSSL come from the
#              x64-windows-static vcpkg triplet.
#
# Build with `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DMOHA_STANDALONE=ON`.
option(MOHA_STATIC_RUNTIME "Link the MSVC runtime statically (/MT)" OFF)
option(MOHA_STANDALONE     "Produce a standalone binary with no third-party shared-library deps" OFF)
option(MOHA_FULLY_STATIC   "Fully static link (Linux only, requires musl toolchain)" OFF)

# mimalloc override — Microsoft's fast allocator. MSVCRT's malloc is notably
# slow on the small-string churn pattern (std::string everywhere in RenderOp,
# SSE parsing, JSON). Linking mimalloc-static and including mimalloc-new-delete
# in main.cpp routes global operator new/delete through mimalloc; measured
# 15–25% wins on allocator-heavy Windows workloads. Default ON when the
# package is installed; harmless on POSIX (mimalloc works there too but the
# delta vs. glibc/jemalloc is smaller).
option(MOHA_USE_MIMALLOC   "Route operator new/delete through mimalloc (big Win32 win)" ON)

# CPU ISA baseline. Default avx2 (Haswell+/Zen1+, ~2013), which covers ~every
# desktop/laptop in use. Drop to "avx" for Sandy/Ivy Bridge (2011–2012) or
# older VM hosts, "sse2" for truly ancient, "native" to let the compiler pick
# based on the build machine. MSVC only exposes a handful of /arch values;
# GCC/Clang understand -march= directly.
set(MOHA_ARCH "avx2" CACHE STRING
    "CPU baseline: native, avx2 (default), avx, or sse2.")
set_property(CACHE MOHA_ARCH PROPERTY STRINGS native avx2 avx sse2)

if(MOHA_STANDALONE AND MSVC)
    # On Windows, standalone implies static MSVC runtime — there's no
    # other way to ship a "drop it on any machine" .exe.
    set(MOHA_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
endif()
if(MOHA_STATIC_RUNTIME AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Tell find_package(OpenSSL) to prefer .a over .so/.dylib when standalone
# is requested. We try static first (QUIET so a missing .a doesn't error
# out); if that fails we silently fall back to the dynamic variant later
# and warn the user that one runtime dep slipped through. Most distros
# (Arch, Debian default, Fedora) only package OpenSSL as .so; users on
# Alpine, vcpkg-static, or who installed openssl-static get the full
# standalone build.
set(MOHA_STANDALONE_OPENSSL_FALLBACK FALSE)
if(MOHA_STANDALONE)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
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

# Mark third-party headers SYSTEM so their warnings don't pollute our build.
# nlohmann_json v3.11.3's binary_writer.hpp uses std::is_trivial which GCC 15
# deprecates under C++26 — the upstream fix is on master but unreleased, and we
# don't want every TU that includes <nlohmann/json.hpp> to re-emit the warning.
if(TARGET nlohmann_json)
    get_target_property(_nj_iface nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
    if(_nj_iface)
        set_target_properties(nlohmann_json PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_nj_iface}")
    endif()
endif()
if(TARGET simdjson)
    get_target_property(_sj_iface simdjson INTERFACE_INCLUDE_DIRECTORIES)
    if(_sj_iface)
        set_target_properties(simdjson PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_sj_iface}")
    endif()
endif()

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

# OpenSSL: if MOHA_STANDALONE asked for static and the static archive is
# missing, retry with shared libs and flag the fallback so the user sees
# a clear note at the end of configure.
if(MOHA_STANDALONE)
    find_package(OpenSSL QUIET)
    if(NOT OpenSSL_FOUND)
        unset(OPENSSL_USE_STATIC_LIBS)
        unset(OPENSSL_LIBRARIES CACHE)
        unset(OPENSSL_CRYPTO_LIBRARY CACHE)
        unset(OPENSSL_SSL_LIBRARY CACHE)
        find_package(OpenSSL REQUIRED)
        set(MOHA_STANDALONE_OPENSSL_FALLBACK TRUE)
    endif()
else()
    find_package(OpenSSL REQUIRED)
endif()

# nghttp2 — HTTP/2 protocol engine for the in-house http client. Prefer the
# upstream CMake config (vcpkg / Homebrew / nghttp2's own export); fall back
# to pkg-config (Linux distros), then a manual find_path/find_library scan.
find_package(nghttp2 CONFIG QUIET)
# Skip pkg-config for fully-static builds: pkg-config returns the dynamic
# library path by default (libnghttp2.so), which the `-static` link below
# can't accept ("attempted static link of dynamic object").  The manual
# find_library path further down (gated on MOHA_STANDALONE) explicitly
# prefers libnghttp2.a, so let it take over.
if(NOT TARGET nghttp2::nghttp2 AND NOT MOHA_FULLY_STATIC)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(NGHTTP2 IMPORTED_TARGET libnghttp2)
        if(TARGET PkgConfig::NGHTTP2)
            add_library(nghttp2::nghttp2 ALIAS PkgConfig::NGHTTP2)
        endif()
    endif()
endif()
# mimalloc — optional, but default-on when available. find_package(mimalloc)
# comes from the upstream CMake export (vcpkg / brew / apt). If missing, we
# silently continue without it rather than failing — it's a perf knob, not a
# correctness requirement.
set(MOHA_HAS_MIMALLOC FALSE)
if(MOHA_USE_MIMALLOC)
    find_package(mimalloc CONFIG QUIET)
    if(TARGET mimalloc-static OR TARGET mimalloc)
        set(MOHA_HAS_MIMALLOC TRUE)
    else()
        message(STATUS "moha: MOHA_USE_MIMALLOC=ON but mimalloc package not "
                       "found — building against system allocator.")
    endif()
endif()

if(NOT TARGET nghttp2::nghttp2)
    find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h)
    # Standalone builds prefer the static archive (libnghttp2.a /
    # nghttp2_static.lib); regular builds prefer the shared library so
    # devs don't need a static archive installed.
    if(MOHA_STANDALONE)
        if(MSVC)
            find_library(NGHTTP2_LIBRARY NAMES nghttp2_static nghttp2)
        else()
            find_library(NGHTTP2_LIBRARY NAMES libnghttp2.a nghttp2_static nghttp2)
        endif()
    else()
        find_library(NGHTTP2_LIBRARY NAMES nghttp2 nghttp2_static)
    endif()
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
    src/tool/util/sandbox.cpp
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

# Everything except the entry point — so tests can link the same runtime
# without fighting main().
set(MOHA_RUNTIME_NOMAIN_SOURCES
    src/runtime/airgap.cpp
    src/runtime/app/deps.cpp
    src/runtime/app/init.cpp
    src/runtime/app/cmd_factory.cpp
    src/runtime/app/update.cpp
    src/runtime/app/update/stream.cpp
    src/runtime/app/update/modal.cpp
    src/runtime/app/update/tool.cpp
    src/runtime/app/update/login.cpp
    src/runtime/app/subscribe.cpp

    src/runtime/view/cache.cpp
    src/runtime/view/helpers.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_args.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_helpers.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_body_preview.cpp
    src/runtime/view/thread/turn/agent_timeline/agent_timeline.cpp
    src/runtime/view/thread/turn/permission.cpp
    src/runtime/view/thread/turn/turn.cpp
    src/runtime/view/thread/welcome_screen.cpp
    src/runtime/view/thread/activity_indicator.cpp
    src/runtime/view/thread/conversation.cpp
    src/runtime/view/thread/thread.cpp
    src/runtime/view/composer.cpp
    src/runtime/view/status_bar/title_chip.cpp
    src/runtime/view/status_bar/phase_chip.cpp
    src/runtime/view/status_bar/token_stream_sparkline.cpp
    src/runtime/view/status_bar/context_gauge.cpp
    src/runtime/view/status_bar/status_banner.cpp
    src/runtime/view/status_bar/shortcut_row.cpp
    src/runtime/view/status_bar/model_badge.cpp
    src/runtime/view/status_bar/status_bar.cpp
    src/runtime/view/changes_strip.cpp
    src/runtime/view/pickers.cpp
    src/runtime/view/diff_review.cpp
    src/runtime/view/login.cpp
    src/runtime/view/view.cpp
)

set(MOHA_RUNTIME_SOURCES
    src/runtime/main.cpp
    ${MOHA_RUNTIME_NOMAIN_SOURCES}
)

add_executable(moha
    ${MOHA_IO_SOURCES}
    ${MOHA_PROVIDER_SOURCES}
    ${MOHA_DIFF_SOURCES}
    ${MOHA_TOOL_SOURCES}
    ${MOHA_RUNTIME_SOURCES}
)

# Reducer tests — opt in with -DMOHA_BUILD_TESTS=ON. Shares the full runtime
# source set so update(...)'s transitive Deps / cmd seam is linkable; swaps
# main.cpp for the test harness.
option(MOHA_BUILD_TESTS "Build reducer tests" OFF)
if(MOHA_BUILD_TESTS)
    add_executable(moha_reducer_tests
        ${MOHA_IO_SOURCES}
        ${MOHA_PROVIDER_SOURCES}
        ${MOHA_DIFF_SOURCES}
        ${MOHA_TOOL_SOURCES}
        ${MOHA_RUNTIME_NOMAIN_SOURCES}
        tests/reducer_tests.cpp
    )
    target_include_directories(moha_reducer_tests PRIVATE include)
    target_link_libraries(moha_reducer_tests PRIVATE
        maya::maya
        nlohmann_json::nlohmann_json
        simdjson::simdjson
        nghttp2::nghttp2
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    if(WIN32)
        target_link_libraries(moha_reducer_tests PRIVATE ws2_32 crypt32 shell32)
    endif()
endif()

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
if(MOHA_HAS_MIMALLOC)
    # Prefer static target so operator new/delete routing is linked in the
    # exe without an accompanying DLL / dylib.
    if(TARGET mimalloc-static)
        target_link_libraries(moha PRIVATE mimalloc-static)
    else()
        target_link_libraries(moha PRIVATE mimalloc)
    endif()
    target_compile_definitions(moha PRIVATE MOHA_USE_MIMALLOC=1)
    message(STATUS "moha: mimalloc allocator override enabled.")
endif()
if(WIN32)
    # ws2_32   — Winsock2 (sockets, WSAPoll)
    # crypt32  — CertOpenSystemStoreW for the Windows root cert loader
    # shell32  — already needed by existing code paths
    # winmm    — timeBeginPeriod / timeEndPeriod in main.cpp (the
    #            `#pragma comment(lib, "winmm.lib")` only works on MSVC).
    target_link_libraries(moha PRIVATE ws2_32 crypt32 shell32 winmm)
    # Static OpenSSL on Windows additionally pulls in these system libs
    # (used by libcrypto's CSP / certificate APIs). Innocuous in dynamic
    # builds — the linker dedupes — but required when MOHA_STANDALONE=ON.
    if(MOHA_STANDALONE)
        target_link_libraries(moha PRIVATE bcrypt secur32 advapi32 user32)
    endif()
endif()
if(APPLE)
    # macOS root cert loader uses the Security and CoreFoundation frameworks.
    target_link_libraries(moha PRIVATE
        "-framework Security"
        "-framework CoreFoundation")
endif()

# ── Standalone link knobs (per-platform) ───────────────────────────────
if(MOHA_STANDALONE AND NOT MSVC)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Fold libstdc++ and libgcc into the binary so it runs on machines
        # with a different distro / ABI version. libc stays dynamic — a
        # fully-static glibc binary breaks the NSS resolver (DNS, /etc/
        # nsswitch) and getpwuid_r at runtime. For 100%-static, opt into
        # MOHA_FULLY_STATIC and build with a musl toolchain.
        target_link_options(moha PRIVATE
            -static-libstdc++ -static-libgcc)
        if(MOHA_FULLY_STATIC)
            target_link_options(moha PRIVATE -static)
        endif()
        # Static OpenSSL pulls in libdl and libpthread (both already
        # implicit) plus libz (compression) for some builds. Add libz
        # defensively; harmless when not needed.
        find_library(ZLIB_STATIC NAMES libz.a z)
        if(ZLIB_STATIC)
            target_link_libraries(moha PRIVATE ${ZLIB_STATIC})
        endif()
    elseif(APPLE)
        # macOS doesn't allow truly-static system frameworks (libSystem
        # is required to be dynamic), but third-party libs can be static.
        # OPENSSL_USE_STATIC_LIBS already handled OpenSSL above. Nothing
        # else to add here — the resulting binary depends only on
        # /usr/lib/libSystem.B.dylib which is on every macOS.
    endif()
endif()

# Diagnostic: print a one-liner at configure time so contributors know
# what they're building. Helps catch the "I forgot the flag" case.
if(MOHA_STANDALONE)
    message(STATUS "moha: STANDALONE build — third-party deps statically linked.")
    if(MOHA_STANDALONE_OPENSSL_FALLBACK)
        message(WARNING
            "moha: STANDALONE requested OpenSSL static libs (.a) but only the "
            "shared library was found — the binary will still depend on libssl/"
            "libcrypto at runtime. To get a fully standalone exe, install the "
            "static OpenSSL package (Alpine: openssl-libs-static; Arch AUR: "
            "openssl-static; Fedora: openssl-static; or build from source).")
    endif()
elseif(MOHA_STATIC_RUNTIME AND MSVC)
    message(STATUS "moha: static MSVC runtime (/MT).")
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
            $<$<STREQUAL:${MOHA_ARCH},avx2>:/arch:AVX2>    # Haswell+ / Zen1+
            $<$<STREQUAL:${MOHA_ARCH},avx>:/arch:AVX>      # Sandy/Ivy Bridge
            # MSVC has no /arch:SSE2 (it's the default on x64) or /arch:native;
            # both map to "no flag" — the default x64 codegen already assumes
            # SSE2. `native` on MSVC degrades to the default baseline.
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
    target_compile_options(moha PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-deprecated-declarations
        # Designated init like `TextElement{.content=…, .style=…}` is the
        # intentional pattern across the view layer — leaving cache fields
        # default-initialized is correct, not a bug. -Wmissing-field-
        # initializers (a -Wextra default) doesn't model that.
        -Wno-missing-field-initializers
    )
    # GCC -Wmaybe-uninitialized produces false positives on std::variant moves
    # of designated-initialized aggregates (maya's Element{TextElement{...}} pattern).
    # The warnings escape -isystem because they fire during late optimization.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(moha PRIVATE -Wno-maybe-uninitialized)
    endif()

    # macOS SDK + GCC: <mach/port.h> uses `_Static_assert` (C keyword) in
    # arm64 macros that fire from any TU pulling in mach headers (e.g.
    # subprocess.cpp via <spawn.h>). Alias the C spelling to its C++
    # equivalent so the SDK headers parse under GCC's C++ frontend.
    if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_definitions(moha PRIVATE
            _Static_assert=static_assert)
    endif()
endif()

# nghttp2.h is a C header whose multi-line function signatures trip MSVC's
# conforming preprocessor (/Zc:preprocessor). Revert to the legacy
# preprocessor for just this TU; the rest of the build keeps /Zc:preprocessor.
if(MSVC)
    set_source_files_properties(src/io/http.cpp PROPERTIES
        COMPILE_FLAGS "/Zc:preprocessor- /DNGHTTP2_STATICLIB")
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
