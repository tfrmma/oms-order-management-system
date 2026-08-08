cmake_minimum_required(VERSION 3.22)
project(sor_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# single source of truth for what belongs to each library. the ASan/UBSan
# binary and the test binary both need these same files recompiled with
# their own flags rather than linking sor_lib/oms_lib directly (those are
# built without -fsanitize=..., and at a different -O level than tests want),
# so the file LIST only has to be kept in sync here, once, instead of in
# every target that needs its own copy.
set(SOR_SOURCES sor/routing_engine.cpp)
set(OMS_SOURCES oms/execution_core.cpp)

add_library(hft_compile_flags INTERFACE)
target_compile_options(hft_compile_flags INTERFACE
    $<$<CONFIG:Release>:-O3 -march=native -funroll-loops -fno-semantic-interposition>
    $<$<CONFIG:RelWithDebInfo>:-O2 -march=native -g>
    $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnull-dereference
    -Wno-unused-parameter -Wno-unknown-pragmas
    -fno-exceptions -fno-rtti
)

# ── SOR ───────────────────────────────────────────────────────────────────────
add_library(sor_lib STATIC
    ${SOR_SOURCES}
)
target_include_directories(sor_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sor_lib PUBLIC hft_compile_flags)

# ── OMS ───────────────────────────────────────────────────────────────────────
add_library(oms_lib STATIC
    ${OMS_SOURCES}
)
# two include roots: project root (for "sor/...") and oms/ (for intra-OMS includes)
target_include_directories(oms_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/oms
)
target_link_libraries(oms_lib PUBLIC sor_lib hft_compile_flags)

# ── binaries ──────────────────────────────────────────────────────────────────
add_executable(sor_example main_example.cpp)
target_link_libraries(sor_example PRIVATE sor_lib)

add_executable(oms_example oms_example.cpp)
target_include_directories(oms_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/oms)
target_link_libraries(oms_example PRIVATE oms_lib)
# logger drain thread + dashboard thread need pthreads
find_package(Threads REQUIRED)
target_link_libraries(oms_lib PUBLIC Threads::Threads)

# ── ASan + UBSan ──────────────────────────────────────────────────────────────
# recompiles SOR_SOURCES/OMS_SOURCES with sanitizer flags instead of linking
# sor_lib/oms_lib: those are built without -fsanitize=..., linking against
# them here would leave library code uninstrumented and defeat the point.
add_executable(oms_example_san
    oms_example.cpp
    ${OMS_SOURCES}
    ${SOR_SOURCES}
)
target_include_directories(oms_example_san PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/oms
)
target_compile_options(oms_example_san PRIVATE
    -O1 -g3 -fsanitize=address,undefined
    -fno-omit-frame-pointer -fno-exceptions -fno-rtti
)
target_link_options(oms_example_san PRIVATE -fsanitize=address,undefined)

install(TARGETS sor_example oms_example DESTINATION bin)

# ── Tests (Catch2 amalgamated) ────────────────────────────────────────────────
add_library(catch2_lib STATIC tests/catch_amalgamated.cpp)
target_include_directories(catch2_lib PUBLIC tests/)
target_compile_options(catch2_lib PRIVATE -O1 -fno-exceptions -fno-rtti)

# recompiles SOR_SOURCES/OMS_SOURCES at a fixed -O1 -g instead of linking
# sor_lib/oms_lib: keeps test build time and behavior independent of whatever
# CMAKE_BUILD_TYPE is active (Release's -march=native would tie test runs to
# the local CPU's ISA for no benefit, Debug's -O0 would make them slow).
add_executable(oms_tests
    tests/test_oms.cpp
    tests/test_twap.cpp
    ${SOR_SOURCES}
    ${OMS_SOURCES}
)
target_include_directories(oms_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/oms
    tests/
)
target_compile_options(oms_tests PRIVATE
    -O1 -g -fno-exceptions -fno-rtti
    -Wall -Wextra -Wno-unused-parameter
)
target_link_libraries(oms_tests PRIVATE catch2_lib Threads::Threads)

# same reasoning as oms_example_san: recompiles instead of linking catch2_lib/
# sor_lib/oms_lib, sanitizers need everything built with the flag, mixing in
# an uninstrumented library defeats the point. found a real stack-overflow-
# turned-SIGSEGV bug the very first time this target existed (ExecutionCore
# grew past what a default thread stack tolerates once TimerWheel's kFineSlots
# went from 1024 to 8192), it earns its place in CI.
add_executable(oms_tests_san
    tests/test_oms.cpp
    tests/test_twap.cpp
    tests/catch_amalgamated.cpp
    ${SOR_SOURCES}
    ${OMS_SOURCES}
)
target_include_directories(oms_tests_san PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/oms
    tests/
)
target_compile_options(oms_tests_san PRIVATE
    -O1 -g -fsanitize=address,undefined
    -fno-omit-frame-pointer -fno-exceptions -fno-rtti
    -Wall -Wextra -Wno-unused-parameter
    # false positive, confirmed: GCC's -Wmaybe-uninitialized misfires inside
    # libstdc++'s <regex> internals (regex_automaton.h's std::function move
    # ctor, inlined into Catch2's test-name/tag matcher) specifically under
    # -fsanitize=..., the same libstdc++ headers compile warning-free without
    # the sanitizer flags on oms_tests. not our code, not a real bug.
    -Wno-maybe-uninitialized
)
target_link_options(oms_tests_san PRIVATE -fsanitize=address,undefined)
target_link_libraries(oms_tests_san PRIVATE Threads::Threads)
