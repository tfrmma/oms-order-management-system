cmake_minimum_required(VERSION 3.22)
project(sor_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
    sor/routing_engine.cpp
)
target_include_directories(sor_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sor_lib PUBLIC hft_compile_flags)

# ── OMS ───────────────────────────────────────────────────────────────────────
add_library(oms_lib STATIC
    oms/execution_core.cpp
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
add_executable(oms_example_san
    oms_example.cpp
    oms/execution_core.cpp
    sor/routing_engine.cpp
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

add_executable(oms_tests
    tests/test_oms.cpp
    sor/routing_engine.cpp
    oms/execution_core.cpp
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
