cmake_minimum_required(VERSION 3.20)
project(fluent_audio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Same numbers everywhere (design D8): no FMA contraction, no fast-math, SSE2 not x87.
set(FA_FLAGS -ffp-contract=off -fno-fast-math -Wall -Wextra -Wno-unused-parameter)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  list(APPEND FA_FLAGS -msse2 -mfpmath=sse)
endif()

# Nodes: one file each, picked up by the glob (drop a file in src/nodes/, re-run the build).
file(GLOB FA_NODE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/nodes/*.cpp)
file(GLOB FA_MACROS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/macros/*.yaml)
set(FA_GEN ${CMAKE_BINARY_DIR}/gen/builtin_macros.cpp)
add_custom_command(
  OUTPUT ${FA_GEN}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_macros.py ${FA_GEN} ${FA_MACROS}
  DEPENDS ${FA_MACROS} ${CMAKE_SOURCE_DIR}/tools/embed_macros.py
  COMMENT "Embedding macros/*.yaml")

set(FA_CORE_SOURCES
  src/core/node.cpp src/core/engine.cpp src/core/yaml_parser.cpp src/core/diagnostics.cpp
  src/sound/compiler.cpp src/capi.cpp ${FA_NODE_SOURCES} ${FA_GEN})

# An OBJECT library, so every node's self-registration object is always linked (no archive dropping).
add_library(fa_core OBJECT ${FA_CORE_SOURCES})
target_include_directories(fa_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(fa_core PRIVATE ${FA_FLAGS})
set_target_properties(fa_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(fluent_audio SHARED $<TARGET_OBJECTS:fa_core>)
target_include_directories(fluent_audio PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(fa_render tools/fa_render.cpp)
target_link_libraries(fa_render PRIVATE fa_core)
target_include_directories(fa_render PRIVATE tools)
target_compile_options(fa_render PRIVATE ${FA_FLAGS})

option(FA_TESTS "Build the tests" ON)
if(FA_TESTS)
  enable_testing()
  add_executable(fa_tests tests/test_main.cpp tests/test_core.cpp tests/test_nodes.cpp tests/test_golden.cpp)
  target_link_libraries(fa_tests PRIVATE fa_core)
  target_include_directories(fa_tests PRIVATE tools tests)
  target_compile_options(fa_tests PRIVATE ${FA_FLAGS})
  target_compile_definitions(fa_tests PRIVATE FA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME core COMMAND fa_tests core)
  add_test(NAME nodes COMMAND fa_tests nodes)
  add_test(NAME golden COMMAND fa_tests golden)
  # native ↔ wasm: needs node and wasm/dist/fluent_audio.wasm (wasm/build.sh); skipped (77) otherwise
  add_test(NAME wasm_same_sound
           COMMAND ${CMAKE_SOURCE_DIR}/tests/wasm_same_sound.sh $<TARGET_FILE:fa_render> ${CMAKE_SOURCE_DIR})
  set_tests_properties(wasm_same_sound PROPERTIES SKIP_RETURN_CODE 77)
endif()
