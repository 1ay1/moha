# AgenttyStandalone.cmake — the pre-submodule setup phase: standalone/static
# link knobs, CPU ISA baseline, LTO/IPO gating, mimalloc/maya toggles, and the
# platform aliases (macOS-GCC _Static_assert, Android/Termux deployment). ALL
# of this must run BEFORE add_subdirectory(maya/...) — it is included as one
# contiguous block from the root to preserve that ordering exactly.

# ── Standalone binary plumbing ─────────────────────────────────────────
# AGENTTY_STANDALONE=ON produces a binary with no third-party shared-library
# dependencies — drop it on any compatible machine (matching libc
# version) and it runs. On every platform it forces the right static-
# linking knobs:
#
#   Linux      OpenSSL + nghttp2 statically linked. libstdc++ and libgcc
#              folded in via -static-libstdc++ / -static-libgcc. libc
#              stays dynamic (fully-static glibc breaks the NSS resolver
#              and DNS lookups; if you need a 100% static binary, build
#              against musl with -DAGENTTY_FULLY_STATIC=ON).
#   macOS      OpenSSL + nghttp2 statically linked. libSystem stays
#              dynamic (the only ABI Apple supports for distribution).
#   Windows    Forces AGENTTY_STATIC_RUNTIME=ON (/MT) so the MSVC CRT is
#              statically embedded; nghttp2 + OpenSSL come from the
#              x64-windows-static vcpkg triplet.
#
# Build with `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DAGENTTY_STANDALONE=ON`.
option(AGENTTY_STATIC_RUNTIME "Link the MSVC runtime statically (/MT)" OFF)
option(AGENTTY_STANDALONE     "Produce a standalone binary with no third-party shared-library deps" OFF)
option(AGENTTY_FULLY_STATIC   "Fully static link (Linux only, requires musl toolchain)" OFF)
# By default a fully-static Linux build is `-static -no-pie` (ET_EXEC): a true
# standalone binary that runs on every Linux userland (glibc, musl, Pi OS).
# Flip this ON only to target Android/Bionic (Termux), which refuses ET_EXEC
# and needs a PIE — but ONLY on a musl toolchain whose -static-pie genuinely
# links libc statically (Alpine's default-PIE GCC does NOT; it produces the
# v0.2.7 crasher, which the build-time guard then rejects).
option(AGENTTY_STATIC_PIE      "Emit a static-PIE (ET_DYN) instead of ET_EXEC for the fully-static build — Termux/Android only" OFF)

# mimalloc override — Microsoft's production allocator, fetched by CMake from
# upstream main. agentty churns std::string everywhere (RenderOp, SSE parsing,
# JSON, and the Element view tree), which benefits from mimalloc's sharded
# free lists and eager page purging. The static library overrides malloc/free
# and global C++ new/delete.
# Default ON; turn OFF to build against the plain system allocator.
option(AGENTTY_USE_MIMALLOC   "Route malloc/free and operator new/delete through CMake-fetched mimalloc" ON)

# CPU ISA baseline. Default avx2 (Haswell+/Zen1+, ~2013), which covers ~every
# desktop/laptop in use. Drop to "avx" for Sandy/Ivy Bridge (2011–2012) or
# older VM hosts, "sse2" for truly ancient, "native" to let the compiler pick
# based on the build machine. MSVC only exposes a handful of /arch values;
# GCC/Clang understand -march= directly.
set(AGENTTY_ARCH "avx2" CACHE STRING
    "CPU baseline: native, avx2 (default), avx, or sse2.")
set_property(CACHE AGENTTY_ARCH PROPERTY STRINGS native avx2 avx sse2)

if(AGENTTY_STANDALONE AND MSVC)
    # On Windows, standalone implies static MSVC runtime — there's no
    # other way to ship a "drop it on any machine" .exe.
    set(AGENTTY_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
endif()
if(AGENTTY_STATIC_RUNTIME AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Tell find_package(OpenSSL) to prefer .a over .so/.dylib when standalone
# is requested. We try static first (QUIET so a missing .a doesn't error
# out); if that fails we silently fall back to the dynamic variant later
# and warn the user that one runtime dep slipped through. Most distros
# (Arch, Debian default, Fedora) only package OpenSSL as .so; users on
# Alpine, vcpkg-static, or who installed openssl-static get the full
# standalone build.
set(AGENTTY_STANDALONE_OPENSSL_FALLBACK FALSE)
if(AGENTTY_STANDALONE)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

# Link-time optimization — enabled by default in Release/RelWithDebInfo, off
# in Debug (would drag build times without shipping value). Supported by
# GCC, Clang, MSVC, and AppleClang; CMake picks the right flag per toolchain.
# Set before the maya/acp-cpp/mcp-cpp subdirectories so the whole tree LTO-
# links as one unit on the platforms where it pays off.
include(CheckIPOSupported)
check_ipo_supported(RESULT AGENTTY_HAS_IPO OUTPUT AGENTTY_IPO_ERR)

# macOS + GCC is the one toolchain where LTO is all cost and no benefit.
# Apple's ld can't consume GCC's LTO bytecode, so `-flto` does NOT optimize
# across modules — the fat objects' per-TU -O3 code is what actually links —
# yet the GIMPLE IR rides along as a dead __GNU_LTO LOAD segment (tens of MB)
# that `strip`/`strip -x` cannot remove (it's a load command, not symbols).
# check_ipo_supported() passes (its probe link "works"), so without this
# guard the macOS release ships ~3x its real size with zero speed gain.
# Disable IPO for this combo only: the binary drops to its true code size and
# is byte-for-byte as optimized. Linux GCC/Clang (where the shipping static
# binaries are built — full LTO + maya's heavy flag set) and MSVC (/GL+/LTCG)
# are unaffected. macOS's real ceiling is AppleClang+ThinLTO, which can't
# build this tree yet (no cxx_std_26 advertised), so -O3 is the macOS max.
set(AGENTTY_IPO_OK ${AGENTTY_HAS_IPO})
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(AGENTTY_IPO_OK FALSE)
    message(STATUS "agentty: LTO disabled on macOS+GCC "
                   "(Apple ld can't consume GCC LTO IR — would only bloat the binary)")
endif()
# MinGW / MSYS2 GCC (ucrt64) is an LTO minefield on GCC 16.x: linking the full
# agentty.exe partitions whole-program GIMPLE into ~200 LTRANS jobs and lto1.exe
# then dies mid-stream with an internal compiler error —
#   "cannot read 'LTO_section_decls' from …ltransNN.o"
# — a toolchain regression, NOT our code (every one of the 476 TUs compiles
# clean; only the final -flto=auto whole-program link stage crashes). It's
# non-deterministic in which partition trips, so there's no single TU to blame
# or split out. The Windows release binary we actually ship is the MSVC build
# (/GL+/LTCG, unaffected); the MSYS2 job is a *compile-only* portability gate.
# Per-TU -O3 code is what links here anyway (MinGW gains little from cross-TU
# LTO), so dropping IPO for this combo makes the gate pass with byte-identical
# optimization and zero risk to any shipped artifact.
if(MINGW OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    set(AGENTTY_IPO_OK FALSE)
    message(STATUS "agentty: LTO disabled on MinGW/MSYS2 GCC "
                   "(GCC 16 lto1 ICE 'cannot read LTO_section_decls' on the "
                   "whole-program link — toolchain bug, not code)")
endif()
# Whole-tree sanitizer builds (-DAGENTTY_SANITIZE_ALL=...) must also disable
# LTO here, BEFORE add_subdirectory(maya) below compiles maya's objects.
# AGENTTY_SANITIZE_ALL is read early on purpose: a `-D` on the cmake command
# line (which is how CI and every documented sanitizer invocation set it) is
# visible from the very start of script evaluation, long before its own
# `set(... CACHE STRING ...)` declaration later in this file runs — so this
# check is not a forward reference, it sees the real value. Without this
# guard, maya compiled here with IPO=TRUE produces GCC slim-LTO bytecode
# objects in libmaya.a; the sanitizer flags added later apply `-fno-lto` at
# LINK time only (with no matching `-flto`), so the linker can't consume
# those bytecode objects: "plugin needed to handle lto object". LTO buys
# sanitizer builds nothing anyway (they're a correctness gate, not a
# release artifact), so turning it off here is strictly a win.
if(AGENTTY_SANITIZE_ALL)
    set(AGENTTY_IPO_OK FALSE)
    message(STATUS "agentty: LTO disabled for sanitizer build "
                   "(AGENTTY_SANITIZE_ALL=${AGENTTY_SANITIZE_ALL} — avoids an "
                   "LTO-bytecode/-fno-lto link mismatch in libmaya.a)")
endif()
if(DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    # A packager/user set it explicitly on the command line (e.g. the AUR
    # agentty-git PKGBUILD passes OFF because GCC 16's LTO streamer ICEs on
    # C++26 decltype in main.cpp: "tree code 'decltype_type' is not supported
    # in LTO streams"). Their word is final — don't re-derive it.
    message(STATUS "agentty: CMAKE_INTERPROCEDURAL_OPTIMIZATION="
                   "${CMAKE_INTERPROCEDURAL_OPTIMIZATION} (user-set, respected)")
elseif(AGENTTY_IPO_OK AND (CMAKE_BUILD_TYPE STREQUAL "Release"
                     OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
                     OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    # Set a DEFINED false value, not just "leave it unset". maya/CMakeLists.txt
    # has its own `if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION) set(...
    # ON)` fallback (so a bare `add_subdirectory(maya)` from an external
    # project still gets LTO) — if we only skip the TRUE branch above without
    # ever defining the variable, that fallback fires inside
    # add_subdirectory(maya) below and silently re-enables LTO out from under
    # a build we deliberately turned it off for (sanitizer link mismatch,
    # macOS+GCC bloat, or the MinGW lto1 ICE above), producing GCC slim-LTO
    # bytecode objects in libmaya.a that the later `-fno-lto`-only link flags
    # can't consume ("plugin needed to handle lto object"). Defining it FALSE
    # here is the actual override maya's guard is designed to respect.
    #
    # This `else` used to be `elseif(NOT AGENTTY_IPO_OK)`, which left DEBUG in
    # exactly the hole the paragraph above describes: IPO_OK is true and the
    # build type is not a release one, so neither branch ran, the variable
    # stayed undefined, and maya's fallback turned LTO ON for the debug build.
    # A whole-program LTO link in the edit-build loop is the single most
    # expensive thing it could have been doing — and it also made every debug
    # object slim-LTO bytecode, which is why no fast linker could be used.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

# ── PIE relocation fix (issue #20 + the mold/objlib R_X86_64_32 failure) ──
# On modern Linux distros (Fedora, Arch, …) the compiler DRIVER defaults to
# building a PIE executable. agentty compiles its shared TUs once into OBJECT
# libraries and links those objects into the PIE exe AND every test. If those
# objects are not position-independent, linking the PIE fails with:
#     relocation R_X86_64_32 against `<sym>' can not be used when making a PIE
#     object; recompile with -fPIC
# This bites in several independent ways, so gating it narrowly (as we did for
# just GNU+LTO) left real toolchains broken:
#   • GCC LTO leaks a 32-bit abs reloc against a hidden static-local guard that
#     ld.bfd can't place in a PIE (the original issue #20).
#   • NON-LTO GCC/Clang + `mold` (which is STRICTER than ld.bfd about abs
#     relocs) rejects the objlib objects outright — the failure reported by a
#     Fedora 44 / GCC 16 / clang 22 contributor building the default dynamic
#     tree with mold.
#   • -fPIE alone is NOT enough: GCC/Clang emit a direct 32-bit reloc for
#     symbols they assume live in the main exe, which breaks the moment an
#     object is reused in a different link (test exes) or the linker is picky.
# Compiling the WHOLE program -fPIC (not just -fPIE) forces PIC-safe
# (GOT/PC-relative) relocations for every symbol, which every linker
# (ld.bfd / gold / lld / mold) accepts in a PIE. It's the portable fix: no
# dependency on which linker is installed, works identically on GCC and Clang,
# and costs a TUI nothing measurable. So apply it to the ENTIRE Linux dynamic
# (PIE) path — every compiler, LTO or not.
#
# Scope guard: the fully-static release build links -no-pie (ET_EXEC) and must
# stay non-PIC (that's what keeps the GitHub-release standalone bins a true
# static ET_EXEC), so it is EXCLUDED here. Apple/MSVC handle PIC/PIE their own
# way and are excluded too. Belt-and-suspenders: also set the CMake property so
# targets defined without agentty's helpers (imported/fetched deps that we
# link) inherit PIC on this path.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND NOT (AGENTTY_FULLY_STATIC AND NOT AGENTTY_STATIC_PIE))
    message(STATUS "agentty: compiling -fPIC for the Linux dynamic (PIE) build "
                   "so OBJECT-lib objects link into the PIE exe + tests under "
                   "any linker incl. mold (fixes R_X86_64_32 PIE reloc, issue #20)")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fPIC>)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

include(FetchContent)

set(MAYA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# STANDALONE builds must be portable across every chip of the target arch.
# Maya's default `-march=native -mtune=native` would otherwise bake the
# producing host's microarchitecture into libmaya.a (most visible on
# aarch64 — a Graviton3-built binary SIGILLs on a Cortex-A72). Force the
# native-tuning gate off so maya falls back to the compiler's default
# baseline (`-march=x86-64` / `armv8-a`).
if(AGENTTY_STANDALONE)
    set(MAYA_NATIVE_TUNING OFF CACHE BOOL "" FORCE)
endif()

# macOS SDK + GCC, catch-all: the macOS 26 SDK's <mach/*.h> arm64
# size-assert macros expand to `_Static_assert(...)`, a C keyword g++
# rejects in C++ mode (only clang accepts it as an extension). ANY TU
# pulling in mach headers (subprocess.cpp via <spawn.h>, tls.cpp, etc.)
# trips it. Alias it directory-wide HERE — before every add_subdirectory
# below (maya, acp-cpp, mcp-cpp) and before the agentty / test targets —
# so all of them, including future submodules, inherit it from one place
# instead of each re-discovering the breakage. AppleClang doesn't need it
# (and can't build this tree — it doesn't advertise cxx_std_26).
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_definitions(_Static_assert=static_assert)
endif()

# ── Android / Termux: raise the deployment API to 28 ───────────────────────
# Two Bionic symbols agentty's dependency graph needs are API-gated:
#   • <spawn.h> / posix_spawn*   → __INTRODUCED_IN(28)
#   • strtod_l / strtof_l        → __INTRODUCED_IN(26)   (libc++ <locale>
#     calls these UNCONDITIONALLY, pulled in transitively via <iostream> /
#     <format>, so the whole C++ standard library fails to compile below 26)
#
# It is NOT enough to -D__ANDROID_API__=28: that only moves the preprocessor
# macro, while clang's __attribute__((availability)) diagnostics compare
# against the DEPLOYMENT TARGET baked into the target triple's API suffix
# (e.g. aarch64-linux-android24). If that stays below the symbol's
# introduction level you get:
#     error: 'strtod_l' is unavailable: introduced in Android 26
#     fatal error: 'spawn.h' file not found
# even with the macro raised. The sanctioned Termux fix (termux-packages
# #23401) is to pass --target=<arch>-linux-android28 so the deployment
# target itself moves up. Modern Termux always runs on API ≥ 28 devices, so
# deploying at 28 is safe.
#
# Derive <arch> from the compiler's own default triple so this works on
# aarch64 / arm / x86_64 without hardcoding. Applied to BOTH compile and link
# (the CRT objects carry the API level too). Directory-wide + before every
# add_subdirectory so maya / acp-cpp / mcp-cpp and the agentty/test targets
# all inherit it. No-op on every non-Android platform.
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android"
           OR DEFINED ENV{TERMUX_VERSION}
           OR EXISTS "/data/data/com.termux/files/usr")
    # Ask the compiler for its default target triple (e.g.
    # "aarch64-linux-android24") and rewrite the trailing API number to 28.
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
        OUTPUT_VARIABLE _agentty_android_triple
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_agentty_android_triple MATCHES "^(.+-linux-android)[0-9]*$")
        set(_agentty_android_target "${CMAKE_MATCH_1}28")
    elseif(_agentty_android_triple MATCHES "android")
        set(_agentty_android_target "${_agentty_android_triple}")
    else()
        # Fallback: derive from the processor if -dumpmachine gave us nothing
        # android-shaped (unusual). aarch64 is the overwhelming Termux case.
        set(_agentty_android_target "aarch64-linux-android28")
    endif()
    message(STATUS "agentty: Android/Termux detected — deploying at "
                   "${_agentty_android_target} (exposes spawn.h + strtod_l)")
    add_compile_options("--target=${_agentty_android_target}")
    add_link_options("--target=${_agentty_android_target}")
    # Keep the preprocessor macro in lock-step for any code that reads it
    # directly (undef first so the toolchain's own -D doesn't conflict).
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:-U__ANDROID_API__>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-D__ANDROID_API__=28>")

    # BELT-AND-SUSPENDERS: also bake the target + API macro straight into
    # CMAKE_<LANG>_FLAGS. add_compile_options() only reaches targets defined
    # in THIS directory and subdirectories added AFTER the call; anything
    # that slips in via a different mechanism (a FetchContent project that
    # resets flags, a submodule that calls project() and re-detects the
    # compiler, an out-of-order add_subdirectory) can miss it. CMAKE_*_FLAGS
    # is string-prepended to EVERY compile in EVERY (sub)directory
    # unconditionally, so mcp-cpp / maya / acp-cpp all get the deployment
    # target no matter how their own CMake is structured. Idempotent: guard
    # against double-append on reconfigure.
    set(_agentty_android_flags
        "--target=${_agentty_android_target} -U__ANDROID_API__ -D__ANDROID_API__=28")
    if(NOT CMAKE_CXX_FLAGS MATCHES "--target=${_agentty_android_target}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_agentty_android_flags}")
    endif()
    if(NOT CMAKE_C_FLAGS MATCHES "--target=${_agentty_android_target}")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_agentty_android_flags}")
    endif()
    if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "--target=${_agentty_android_target}")
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} --target=${_agentty_android_target}")
    endif()
    if(NOT CMAKE_SHARED_LINKER_FLAGS MATCHES "--target=${_agentty_android_target}")
        set(CMAKE_SHARED_LINKER_FLAGS
            "${CMAKE_SHARED_LINKER_FLAGS} --target=${_agentty_android_target}")
    endif()
endif()

