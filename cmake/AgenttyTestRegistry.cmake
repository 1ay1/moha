# AgenttyTestRegistry.cmake — one declaration per test, aggregates derived.
#
# Replaces the old 4-list system (agentty_add_full_test + AGENTTY_MIGRATED_TESTS
# + hand-listed `tests`/`sanitizer_tests` aggregates + the explicit agentty_tests
# source list). A test's name is now written EXACTLY ONCE, in an agentty_test()
# call; every downstream set is computed from a global registry at finalize.
#
# ── API ────────────────────────────────────────────────────────────────────
#   agentty_test(<name>
#       [MODE consolidated|standalone|raw]  # default consolidated
#       [SRCS <extra .cpp> ...]             # extra TUs beyond tests/<name>.cpp
#       [OBJS <$<TARGET_OBJECTS:x>> ...]    # extra object libs (standalone)
#       [LIBS <lib> ...]                    # extra link libraries (standalone)
#       [TIMEOUT <seconds>]                 # default 60; ctest entries only
#       [LABELS <label> ...]                # ctest labels; `sanitizer`/`perf` drive derived sets
#       [UNIX_LIBS <lib> ...]               # extra libs on UNIX AND NOT APPLE (e.g. util for openpty)
#       [ENV "K=V" ...]                     # ctest ENVIRONMENT property
#       [NO_TEST]                           # build but register NO ctest entry (probes / capture tools)
#       [GATE <VAR>]                        # only define when ${VAR} is truthy (e.g. AGENTTY_MCP)
#   )
#     - consolidated → source appended to the agentty_tests doctest binary (no exe).
#     - standalone   → own EXCLUDE_FROM_ALL exe with the FULL shared-object link
#                      recipe applied once (forkers / PTY / fuzzers / benches / acp / mcp).
#     - raw          → caller defines the add_executable + add_test itself
#                      (narrow-source sanitizer tests that must NOT link the full
#                      shared set); we only record it in the aggregates/labels.
#
#   agentty_add_ctest(<name> COMMAND <argv...> [TIMEOUT n] [LABELS ...])
#       # a ctest entry running an EXISTING target with args (reveal_stream_gate arms).
#
#   agentty_finalize_tests()
#       # define agentty_tests from the accumulated consolidated sources, and
#       # build the derived `tests` / `tests_gating` / `sanitizer_tests` targets.
#
# Registry state lives in DIRECTORY properties (append-only).

define_property(DIRECTORY PROPERTY AGENTTY_T_CONSOLIDATED_SRCS
    BRIEF_DOCS "sources folded into agentty_tests" FULL_DOCS "AgenttyTestRegistry")
define_property(DIRECTORY PROPERTY AGENTTY_T_STANDALONE
    BRIEF_DOCS "standalone test target names" FULL_DOCS "AgenttyTestRegistry")
define_property(DIRECTORY PROPERTY AGENTTY_T_SANITIZER
    BRIEF_DOCS "sanitizer-labelled test names" FULL_DOCS "AgenttyTestRegistry")
define_property(DIRECTORY PROPERTY AGENTTY_T_PERF
    BRIEF_DOCS "perf/probe/bench names (skippable on the fast gate)" FULL_DOCS "AgenttyTestRegistry")

# ── The full-stack link recipe, applied to any target that links the shared
#    object set (standalone tests + agentty_tests). Mirrors the original
#    agentty_add_full_test body exactly so codegen is byte-identical.
function(_agentty_test_link_full name)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(${name} PRIVATE
        AGENTTY_VERSION="${PROJECT_VERSION}"
        # Source path for the picker row-style convention guard: those rules
        # are view-layer style constants with no runtime seam to assert
        # against, so the test reads the source. The pickers were split into
        # several files under this dir; the test concatenates them all.
        AGENTTY_PICKERS_SRC_DIR="${CMAKE_SOURCE_DIR}/src/runtime/view/pickers"
        AGENTTY_MCP=1)
    target_link_libraries(${name} PRIVATE
        maya::maya
        nlohmann_json::nlohmann_json
        simdjson::simdjson
        nghttp2::nghttp2
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads)
    # registry.cpp routes the whole tool set through the mcp-cpp bridge.
    if(TARGET mcp::mcp)
        target_link_libraries(${name} PRIVATE mcp::mcp)
    endif()
    if(TARGET mcp::tools)
        target_link_libraries(${name} PRIVATE mcp::tools)
    endif()
    if(AGENTTY_HAS_RAGCPP)
        target_link_libraries(${name} PRIVATE ragcpp::ragcpp)
        target_compile_definitions(${name} PRIVATE AGENTTY_HAS_RAGCPP=1)
    else()
        target_compile_definitions(${name} PRIVATE AGENTTY_HAS_RAGCPP=0)
    endif()
    if(AGENTTY_HAS_MIMALLOC)
        target_link_libraries(${name} PRIVATE mimalloc-static)
        target_compile_definitions(${name} PRIVATE AGENTTY_USE_MIMALLOC=1)
    endif()
    if(WIN32)
        target_link_libraries(${name} PRIVATE
            ws2_32 crypt32 shell32 winmm user32 gdi32 gdiplus shlwapi)
    endif()
    if(APPLE)
        target_link_libraries(${name} PRIVATE
            "-framework Security" "-framework CoreFoundation")
    endif()
endfunction()

function(agentty_test name)
    cmake_parse_arguments(T
        "NO_TEST"
        "MODE;TIMEOUT;GATE"
        "SRCS;OBJS;LIBS;LABELS;UNIX_LIBS;ENV"
        ${ARGN})

    if(T_GATE AND NOT ${T_GATE})
        return()
    endif()
    if(NOT T_MODE)
        set(T_MODE consolidated)
    endif()

    set(_primary "${CMAKE_SOURCE_DIR}/tests/${name}.cpp")

    if(T_MODE STREQUAL "consolidated")
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_CONSOLIDATED_SRCS
            "${_primary}" ${T_SRCS})
        return()
    endif()

    # Both standalone and raw record into the aggregates + label sets.
    set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_STANDALONE ${name})
    if("sanitizer" IN_LIST T_LABELS)
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_SANITIZER ${name})
    endif()
    if("perf" IN_LIST T_LABELS)
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_PERF ${name})
    endif()

    if(T_MODE STREQUAL "raw")
        return()   # caller owns add_executable + add_test
    endif()

    # ── standalone: full shared-object link ─────────────────────────────────
    add_executable(${name} EXCLUDE_FROM_ALL
        ${_primary} ${T_SRCS} ${AGENTTY_SHARED_OBJECTS} ${T_OBJS})
    _agentty_test_link_full(${name})
    target_link_libraries(${name} PRIVATE ${T_LIBS})
    # Test binaries are never shipped: no LTO (faster + avoids the GCC
    # -flto=auto init-order hazard that bit agentty_tests). LTO stays on the
    # shipped `agentty` only.
    set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    # And no module scanning: it makes every TU uncacheable by ccache (the scan
    # adds -fmodule-mapper, which ccache rejects). See agentty_objlib().
    set_target_properties(${name} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    if(T_UNIX_LIBS AND UNIX AND NOT APPLE)
        target_link_libraries(${name} PRIVATE ${T_UNIX_LIBS})
    endif()

    if(NOT T_NO_TEST)
        add_test(NAME ${name} COMMAND ${name})
        if(NOT T_TIMEOUT)
            set(T_TIMEOUT 60)
        endif()
        set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT})
        if(T_LABELS)
            set_tests_properties(${name} PROPERTIES LABELS "${T_LABELS}")
        endif()
        if(T_ENV)
            set_tests_properties(${name} PROPERTIES ENVIRONMENT "${T_ENV}")
        endif()
    endif()
endfunction()

# Raw ctest entry that runs an EXISTING target with arguments (no new binary).
function(agentty_add_ctest name)
    cmake_parse_arguments(C "" "TIMEOUT" "COMMAND;LABELS" ${ARGN})
    add_test(NAME ${name} COMMAND ${C_COMMAND})
    if(NOT C_TIMEOUT)
        set(C_TIMEOUT 120)
    endif()
    set_tests_properties(${name} PROPERTIES TIMEOUT ${C_TIMEOUT})
    if(C_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${C_LABELS}")
    endif()
endfunction()

# Record a raw target (defined by the caller) into the sanitizer set.
function(agentty_mark_sanitizer)
    foreach(name ${ARGN})
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_SANITIZER ${name})
    endforeach()
endfunction()

# ── The folded standalone-test binary ───────────────────────────────────────
# agentty_standalone_tests: ONE exe holding the full-stack standalone tests
# (forkers / PTY / fuzzers / e2e / benches) that can't be doctest cases in a
# shared process. Each is still run in its OWN process per ctest entry via
# argv dispatch (agentty_standalone_tests <name> [args]) — so fork/PTY/chdir
# isolation is preserved — but ~16 heavy 100 MB+ links collapse into one.
#
#   agentty_fold_test(<name> [TIMEOUT n] [LABELS ..] [ARGS] [UNIX_LIBS ..])
# records the test; agentty_finalize_fold() builds the single binary + the
# per-test ctest entries. Extra link libs/objs the fold needs (mcp::mcp,
# acp::acp, agentty_acp_obj) are added once on the binary in AgenttyTests.cmake.
define_property(DIRECTORY PROPERTY AGENTTY_FOLD_NAMES
    BRIEF_DOCS "folded standalone test names" FULL_DOCS "AgenttyTestRegistry")

function(agentty_fold_test name)
    cmake_parse_arguments(F "ARGS" "TIMEOUT;SKIP_CODE" "LABELS;UNIX_LIBS" ${ARGN})
    set_property(DIRECTORY APPEND PROPERTY AGENTTY_FOLD_NAMES ${name})
    # Rename this TU's main() to <name>_main so the dispatcher can call it.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/tests/${name}.cpp
        PROPERTIES COMPILE_DEFINITIONS "main=${name}_main")
    if(F_UNIX_LIBS)
        set_property(DIRECTORY APPEND PROPERTY AGENTTY_FOLD_UNIX_LIBS ${F_UNIX_LIBS})
    endif()
    # ctest entry: run this test as its own process via the shared binary.
    if(NOT F_TIMEOUT)
        set(F_TIMEOUT 60)
    endif()
    add_test(NAME ${name} COMMAND agentty_standalone_tests ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${F_TIMEOUT})
    if(F_SKIP_CODE)
        # A test may bail with this exit code when its environment can't
        # produce the geometry it needs (e.g. a PTY-driven repro whose
        # composer refuses to wrap) — ctest reports "Skipped", not FAILED.
        set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE ${F_SKIP_CODE})
    endif()
    if(F_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${F_LABELS}")
        if("perf" IN_LIST F_LABELS)
            set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_PERF ${name})
        endif()
    endif()
endfunction()

# Build the one binary from all recorded fold TUs + the dispatcher. Extra
# libs/objs are passed by the caller (union of every folded test's needs).
function(agentty_finalize_fold)
    cmake_parse_arguments(FF "" "" "OBJS;LIBS" ${ARGN})
    get_property(_names DIRECTORY PROPERTY AGENTTY_FOLD_NAMES)
    set(_srcs ${CMAKE_SOURCE_DIR}/tests/agentty_standalone_tests_main.cpp)
    foreach(_n ${_names})
        list(APPEND _srcs ${CMAKE_SOURCE_DIR}/tests/${_n}.cpp)
    endforeach()
    add_executable(agentty_standalone_tests EXCLUDE_FROM_ALL
        ${_srcs} ${AGENTTY_SHARED_OBJECTS} ${FF_OBJS})
    _agentty_test_link_full(agentty_standalone_tests)
    target_link_libraries(agentty_standalone_tests PRIVATE ${FF_LIBS})
    get_property(_ulibs DIRECTORY PROPERTY AGENTTY_FOLD_UNIX_LIBS)
    if(_ulibs AND UNIX AND NOT APPLE)
        target_link_libraries(agentty_standalone_tests PRIVATE ${_ulibs})
    endif()
    # Test binaries never ship — no LTO (faster + dodges the GCC init-order
    # hazard, same rationale as agentty_tests).
    set_target_properties(agentty_standalone_tests PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION OFF)
    # The dispatcher #includes tests/agentty_standalone_tests.def.
    target_include_directories(agentty_standalone_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    # It counts as one entry in the standalone aggregate.
    set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_STANDALONE agentty_standalone_tests)
endfunction()

function(agentty_finalize_tests)
    get_property(_srcs DIRECTORY PROPERTY AGENTTY_T_CONSOLIDATED_SRCS)
    add_executable(agentty_tests EXCLUDE_FROM_ALL
        ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
        ${_srcs}
        ${AGENTTY_SHARED_OBJECTS}
        $<TARGET_OBJECTS:agentty_acp_obj>)   # acp_integration_test needs AgentServer
    _agentty_test_link_full(agentty_tests)
    # No module scanning: it makes every TU uncacheable by ccache. This binary
    # holds ~90 test TUs, so it is where the hit rate matters most.
    set_target_properties(agentty_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    target_link_libraries(agentty_tests PRIVATE doctest::doctest)
    if(TARGET acp::acp)
        target_link_libraries(agentty_tests PRIVATE acp::acp)
    endif()
    # Test binaries are never shipped, so they don't need LTO — and GCC's
    # -flto=auto re-partitions the whole shared object set's dynamic-init
    # sequence, which surfaced a latent static-init/codegen hazard that CRASHED
    # agentty_tests at startup (doctest_discover_tests runs it POST_BUILD to
    # enumerate cases, so a startup crash broke the whole build — "links fine,
    # dies at discovery"). Turning IPO off for the test binary removes the LTO
    # trigger, keeps LTO where it matters (the shipped `agentty`), and builds
    # the test faster. See cmake/BEST_PRACTICES.md.
    set_target_properties(agentty_tests PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
    doctest_discover_tests(agentty_tests)

    # ── Derived aggregates (computed, never hand-listed) ────────────────────
    get_property(_standalone DIRECTORY PROPERTY AGENTTY_T_STANDALONE)
    get_property(_sanitizer  DIRECTORY PROPERTY AGENTTY_T_SANITIZER)
    get_property(_perf       DIRECTORY PROPERTY AGENTTY_T_PERF)

    add_custom_target(tests DEPENDS agentty_tests ${_standalone})

    set(_gating ${_standalone})
    if(_perf)
        list(REMOVE_ITEM _gating ${_perf})
    endif()
    add_custom_target(tests_gating DEPENDS agentty_tests ${_gating})

    add_custom_target(sanitizer_tests DEPENDS agentty_tests ${_sanitizer})
endfunction()
