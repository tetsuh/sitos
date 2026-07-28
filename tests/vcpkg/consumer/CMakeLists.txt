cmake_minimum_required(VERSION 3.20)
project(sitos_vcpkg_rocksdb_probe LANGUAGES CXX)

include(CTest)
enable_testing()

find_package(RocksDB 11.1.2 EXACT CONFIG REQUIRED)
if(NOT RocksDB_VERSION VERSION_EQUAL 11.1.2)
  message(FATAL_ERROR "Resolved RocksDB ${RocksDB_VERSION}, expected exactly 11.1.2")
endif()
if(NOT TARGET RocksDB::rocksdb)
  message(FATAL_ERROR "RocksDB::rocksdb is required by the vcpkg consumer contract")
endif()

add_executable(rocksdb_consumer main.cpp)
target_link_libraries(rocksdb_consumer PRIVATE RocksDB::rocksdb)
target_compile_features(rocksdb_consumer PRIVATE cxx_std_20)

if(DEFINED PROBE_RUNTIME_DIR AND NOT PROBE_RUNTIME_DIR STREQUAL "")
  file(MAKE_DIRECTORY "${PROBE_RUNTIME_DIR}")
  set_target_properties(rocksdb_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROBE_RUNTIME_DIR}")
  add_test(NAME VcpkgRocksDBConsumerLinksAndRuns
    COMMAND "$<TARGET_FILE:rocksdb_consumer>" "${PROBE_RUNTIME_DIR}/rocksdb-db")
  set_tests_properties(VcpkgRocksDBConsumerLinksAndRuns PROPERTIES
    TIMEOUT 60 WORKING_DIRECTORY "${PROBE_RUNTIME_DIR}")
else()
  add_test(NAME VcpkgRocksDBConsumerLinksAndRuns
    COMMAND "$<TARGET_FILE:rocksdb_consumer>" "${CMAKE_CURRENT_BINARY_DIR}/rocksdb-db")
  set_tests_properties(VcpkgRocksDBConsumerLinksAndRuns PROPERTIES TIMEOUT 60)
endif()

# The exact version and imported-target contract are asserted during configure above. Register the
# contract name so the configure gate remains visible in the focused CTest output.
add_test(NAME VcpkgRocksDBFeatureConfigures COMMAND "${CMAKE_COMMAND}" -E true)
