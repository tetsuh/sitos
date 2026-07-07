cmake_minimum_required(VERSION 3.20)
project(sitos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Options
option(SITOS_WITH_ROCKSDB "Build with RocksDB storage engine support" OFF)
option(SITOS_BUILD_PYTHON "Build Python bindings" OFF)
option(SITOS_BUILD_TESTS "Build tests" ON)
option(SITOS_BUILD_EXAMPLES "Build examples" OFF)

# Warnings as errors
if(MSVC)
  add_compile_options(/W4 /WX /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Core library
add_library(sitos STATIC
  src/param_value.cpp
  src/batch.cpp
)
add_library(sitos::sitos ALIAS sitos)
target_include_directories(sitos PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(sitos PUBLIC cxx_std_20)

if(SITOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
