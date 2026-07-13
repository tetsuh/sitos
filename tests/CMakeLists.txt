include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

set(SITOS_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/tests/fixtures/payload_v1")

add_executable(sitos_bootstrap_test unit/bootstrap_test.cpp)
target_link_libraries(sitos_bootstrap_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_bootstrap_test)
gtest_discover_tests(sitos_bootstrap_test)

add_executable(sitos_param_value_test unit/param_value_test.cpp)
target_link_libraries(sitos_param_value_test PRIVATE GTest::gtest_main sitos::sitos)
target_compile_definitions(sitos_param_value_test PRIVATE
  SITOS_FIXTURE_DIR="${SITOS_FIXTURE_DIR}")
sitos_enable_warnings(sitos_param_value_test)
gtest_discover_tests(sitos_param_value_test)

add_executable(sitos_key_test unit/key_test.cpp)
target_link_libraries(sitos_key_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_key_test)
gtest_discover_tests(sitos_key_test)

add_executable(sitos_storage_engine_contract_test
  unit/storage_engine_contract_test.cpp)
target_link_libraries(sitos_storage_engine_contract_test
  PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_storage_engine_contract_test)
gtest_discover_tests(sitos_storage_engine_contract_test)

add_executable(sitos_in_memory_engine_test
  unit/in_memory_engine_test.cpp)
target_link_libraries(sitos_in_memory_engine_test
  PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_in_memory_engine_test)
gtest_discover_tests(sitos_in_memory_engine_test)

add_executable(sitos_logging_test
  unit/logging_test.cpp)
target_link_libraries(sitos_logging_test
  PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_logging_test)
gtest_discover_tests(sitos_logging_test)

add_executable(sitos_storage_node_test
  unit/storage_node_test.cpp)
target_link_libraries(sitos_storage_node_test
  PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_storage_node_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_storage_node_test)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_storage_node_test)
endif()
gtest_discover_tests(sitos_storage_node_test)

# --- Zenoh smoke test (integration) ---
if(SITOS_WITH_ZENOH)
  add_executable(sitos_zenoh_smoke_test
    integration/zenoh_smoke_test.cpp)
  target_link_libraries(sitos_zenoh_smoke_test
    PRIVATE GTest::gtest_main zenohc::zenohc)
  # Do NOT apply sitos_enable_warnings: the zenoh C header may trigger warnings
  # under -Werror that we cannot fix.
  sitos_copy_zenohc(sitos_zenoh_smoke_test)
  gtest_discover_tests(sitos_zenoh_smoke_test)

  add_executable(sitos_transport_test
    integration/transport_test.cpp)
  target_link_libraries(sitos_transport_test
    PRIVATE GTest::gtest_main sitos::sitos)
  target_include_directories(sitos_transport_test PRIVATE ${PROJECT_SOURCE_DIR})
  sitos_enable_warnings(sitos_transport_test)
  sitos_copy_zenohc(sitos_transport_test)
  gtest_discover_tests(sitos_transport_test)

  add_executable(sitos_storage_node_query_test
    integration/storage_node_query_test.cpp)
  target_link_libraries(sitos_storage_node_query_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_storage_node_query_test)
  sitos_copy_zenohc(sitos_storage_node_query_test)
  gtest_discover_tests(sitos_storage_node_query_test)

  add_executable(sitos_storage_node_subscriber_test
    integration/storage_node_subscriber_test.cpp)
  target_link_libraries(sitos_storage_node_subscriber_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_storage_node_subscriber_test)
  sitos_copy_zenohc(sitos_storage_node_subscriber_test)
  gtest_discover_tests(sitos_storage_node_subscriber_test)

  add_executable(sitos_storage_node_lifecycle_test
    integration/storage_node_lifecycle_test.cpp)
  target_link_libraries(sitos_storage_node_lifecycle_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_storage_node_lifecycle_test)
  sitos_copy_zenohc(sitos_storage_node_lifecycle_test)
  gtest_discover_tests(sitos_storage_node_lifecycle_test)
endif()
