# Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
# Distributed under the MIT license that can be found in the LICENSE file.

cmake_minimum_required(VERSION 3.8)
include(ProcessorCount)

project(VectorSynth)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_CXX_FLAGS                "-Wall -Werror -march=native -fPIC ${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG          "-g -Og ${CMAKE_CXX_FLAGS_DEBUG}")
set(CMAKE_CXX_FLAGS_RELEASE        "-O3 -DNDEBUG ${CMAKE_CXX_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELEASE} -g ${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include_directories(${PROJECT_SOURCE_DIR})

find_package(Alive2)
include_directories(${ALIVE2_SOURCE_DIR})


find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

if (NOT LLVM_ENABLE_RTTI)
  message(FATAL_ERROR "LLVM must be built with '-DLLVM_ENABLE_RTTI=ON'")
endif()

if (NOT LLVM_ENABLE_EH)
  message(FATAL_ERROR "LLVM must be built with '-DLLVM_ENABLE_EH=ON'")
endif()

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(SYNTHESIZER_SRC
  "lib/constantsynth.cpp"
  "lib/ir.cpp"
  "lib/simd.cpp"
  "lib/synthesizer.cpp"
  "alive_util/known_fns.cpp"
  "alive_util/llvm2alive.cpp"
  "alive_util/utils.cpp"
)

add_llvm_executable(alive-cs
  "tools/alive-cs.cpp"
)

add_library(synth STATIC ${SYNTHESIZER_SRC})
set(VECTORSYNTH_LIBS_LLVM synth ${ALIVE_LIBS})

find_package(Z3 4.8.5 REQUIRED)

llvm_map_components_to_libnames(llvm_libs support core irreader analysis passes transformutils)
target_link_libraries(alive-cs PRIVATE ${VECTORSYNTH_LIBS_LLVM} ${llvm_libs})
target_link_libraries(alive-cs PRIVATE ${Z3_LIBRARIES})

add_subdirectory(pass)
