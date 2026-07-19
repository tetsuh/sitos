cmake_minimum_required(VERSION 3.20)
project(sitos VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Options
option(SITOS_WITH_ROCKSDB "Build with RocksDB storage engine support" OFF)
option(SITOS_BUILD_PYTHON "Build Python bindings" OFF)
option(SITOS_BUILD_TESTS "Build tests" ON)
option(SITOS_BUILD_EXAMPLES "Build examples" OFF)
option(SITOS_BUILD_BENCHMARKS "Build optional Google Benchmark targets" OFF)
option(SITOS_ENABLE_TSAN "Enable ThreadSanitizer for supported compilers" OFF)
option(SITOS_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(SITOS_ENABLE_TSAN AND SITOS_ENABLE_ASAN_UBSAN)
  message(FATAL_ERROR "SITOS_ENABLE_TSAN and SITOS_ENABLE_ASAN_UBSAN are mutually exclusive")
endif()
if(SITOS_ENABLE_TSAN OR SITOS_ENABLE_ASAN_UBSAN)
  if(MSVC)
    message(FATAL_ERROR "Sanitizers are currently supported only with GCC/Clang")
  endif()
  add_compile_options(-fno-omit-frame-pointer)
  if(SITOS_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
  else()
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# --- zenoh-c build integration ---
# Only required when building tests or examples that use the transport layer.
option(SITOS_WITH_ZENOH "Download and link prebuilt zenoh-c" OFF)

if(SITOS_WITH_ZENOH)
  include(cmake/zenohc.cmake)
endif()

if(SITOS_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googlebenchmark)
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
  src/client_config.cpp
  src/param_store.cpp
  src/param_cache.cpp
  src/status.cpp
  src/param_value.cpp
  src/batch.cpp
  src/key.cpp
  src/storage_engine.cpp
  src/in_memory_engine.cpp
  src/logging.cpp
  src/storage_node.cpp
  src/transport/get_completion.cpp
  src/transport/declaration_handle_lifecycle.cpp
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
  target_link_libraries(sitos PRIVATE zenohc::zenohc)
else()
  target_sources(sitos PRIVATE src/transport_stub.cpp)
endif()

if(SITOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SITOS_BUILD_BENCHMARKS)
  add_subdirectory(tests/bench)
endif()

install(TARGETS sitos
  EXPORT sitosTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(SITOS_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/sitos)
set(SITOS_PACKAGE_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/sitosConfig.cmake")
set(SITOS_PACKAGE_VERSION_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/sitosConfigVersion.cmake")
configure_package_config_file(
  cmake/sitosConfig.cmake.in
  "${SITOS_PACKAGE_CONFIG}"
  INSTALL_DESTINATION "${SITOS_CMAKE_INSTALL_DIR}")
write_basic_package_version_file(
  "${SITOS_PACKAGE_VERSION_CONFIG}"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)

install(EXPORT sitosTargets
  FILE sitosTargets.cmake
  NAMESPACE sitos::
  DESTINATION ${SITOS_CMAKE_INSTALL_DIR})
install(FILES
  "${SITOS_PACKAGE_CONFIG}"
  "${SITOS_PACKAGE_VERSION_CONFIG}"
  cmake/Findzenohc.cmake
  DESTINATION ${SITOS_CMAKE_INSTALL_DIR})
