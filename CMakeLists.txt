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

# --- zenoh-c build integration ---
# Only required when building tests or examples that use the transport layer.
option(SITOS_WITH_ZENOH "Download and link prebuilt zenoh-c" OFF)

if(SITOS_WITH_ZENOH)
  include(cmake/zenohc.cmake)
endif()

# Scoped warning flags. Applied per-target (not globally) so vendored deps
# such as GoogleTest (FetchContent) are not pinned to -Werror and break on a
# new compiler warning. Callers of sitos get the flags only via PUBLIC when they
# compile sitos headers; that is fine because sitos headers are warning-clean.
function(sitos_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

# Core library
add_library(sitos STATIC
  src/param_value.cpp
  src/batch.cpp
  src/key.cpp
  src/storage_engine.cpp
  src/in_memory_engine.cpp
)
add_library(sitos::sitos ALIAS sitos)
target_include_directories(sitos PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(sitos PUBLIC cxx_std_20)
sitos_enable_warnings(sitos)

if(SITOS_WITH_ZENOH)
  target_sources(sitos PRIVATE src/transport/zenoh_transport.cpp)
  target_link_libraries(sitos PUBLIC zenohc::zenohc)
endif()

if(SITOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
