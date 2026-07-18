include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

set(SITOS_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/tests/fixtures/payload_v1")

function(add_sitos_public_header_compile_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE sitos::sitos)
  sitos_enable_warnings(${target})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

add_sitos_public_header_compile_test(
  sitos_public_headers_compile_test consumer/public_headers_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_status_header_compile_test consumer/status_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_result_header_compile_test consumer/result_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_client_config_header_compile_test consumer/client_config_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_param_store_header_compile_test consumer/param_store_header_compile.cpp)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_param_store_header_compile_test)
endif()

add_executable(sitos_bootstrap_test unit/bootstrap_test.cpp)
target_link_libraries(sitos_bootstrap_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_bootstrap_test)
# Use configure-time source discovery to avoid CMake 4.4 JSON discovery
# failures on the Windows runner (#93).
gtest_add_tests(TARGET sitos_bootstrap_test SOURCES unit/bootstrap_test.cpp)

add_executable(sitos_param_value_test unit/param_value_test.cpp)
target_link_libraries(sitos_param_value_test PRIVATE GTest::gtest_main sitos::sitos)
target_compile_definitions(sitos_param_value_test PRIVATE
  SITOS_FIXTURE_DIR="${SITOS_FIXTURE_DIR}")
sitos_enable_warnings(sitos_param_value_test)
gtest_add_tests(TARGET sitos_param_value_test SOURCES unit/param_value_test.cpp)

add_executable(sitos_result_test unit/result_test.cpp)
target_link_libraries(sitos_result_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_result_test)
gtest_add_tests(TARGET sitos_result_test SOURCES unit/result_test.cpp)

add_executable(sitos_client_config_test unit/client_config_test.cpp)
target_link_libraries(sitos_client_config_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_client_config_test)
gtest_add_tests(TARGET sitos_client_config_test SOURCES unit/client_config_test.cpp)

add_executable(sitos_param_store_test unit/param_store_test.cpp)
target_link_libraries(sitos_param_store_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_param_store_test)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_param_store_test)
endif()
gtest_add_tests(TARGET sitos_param_store_test SOURCES unit/param_store_test.cpp)

add_executable(sitos_client_transport_factory_test unit/client_transport_factory_test.cpp)
target_link_libraries(sitos_client_transport_factory_test PRIVATE GTest::gtest_main sitos::sitos)
if(SITOS_WITH_ZENOH)
  target_compile_definitions(sitos_client_transport_factory_test PRIVATE SITOS_WITH_ZENOH=1)
  target_include_directories(sitos_client_transport_factory_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  sitos_copy_zenohc(sitos_client_transport_factory_test)
endif()
sitos_enable_warnings(sitos_client_transport_factory_test)
gtest_add_tests(
  TARGET sitos_client_transport_factory_test
  SOURCES unit/client_transport_factory_test.cpp)

add_executable(sitos_key_test unit/key_test.cpp)
target_link_libraries(sitos_key_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_key_test)
gtest_add_tests(TARGET sitos_key_test SOURCES unit/key_test.cpp)

# sitos_storage_engine_contract_test and sitos_in_memory_engine_test are
# intentionally NOT migrated to gtest_add_tests: their TEST_P cases come from
# INSTANTIATE_STORAGE_ENGINE_CONTRACT_SUITE (storage_engine_contract.hpp),
# so the literal TEST_P(...) text CMake's source parser needs never appears
# in these .cpp files. Switching them would silently register zero tests.
# DISCOVERY_MODE PRE_TEST moves the flaky JSON discovery out of the build
# step (ninja) and into ctest, so a discovery hiccup no longer breaks the
# build itself (#93).
add_executable(sitos_storage_engine_contract_test
  unit/storage_engine_contract_test.cpp)
target_link_libraries(sitos_storage_engine_contract_test
  PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_storage_engine_contract_test)
gtest_discover_tests(sitos_storage_engine_contract_test DISCOVERY_MODE PRE_TEST)

add_executable(sitos_in_memory_engine_test
  unit/in_memory_engine_test.cpp)
target_link_libraries(sitos_in_memory_engine_test
  PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_in_memory_engine_test)
gtest_discover_tests(sitos_in_memory_engine_test DISCOVERY_MODE PRE_TEST)

add_executable(sitos_logging_test
  unit/logging_test.cpp)
target_link_libraries(sitos_logging_test
  PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_logging_test)
gtest_add_tests(TARGET sitos_logging_test SOURCES unit/logging_test.cpp)

add_executable(sitos_transport_get_completion_test
  unit/transport_get_completion_test.cpp)
target_link_libraries(sitos_transport_get_completion_test
  PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_transport_get_completion_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_transport_get_completion_test)
# Use configure-time source discovery to avoid CMake 4.4 JSON discovery failures
# for this zenoh-independent completion test on the Windows runner.
gtest_add_tests(
  TARGET sitos_transport_get_completion_test
  SOURCES unit/transport_get_completion_test.cpp)

add_executable(sitos_storage_node_test
  unit/storage_node_test.cpp)
target_link_libraries(sitos_storage_node_test
  PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_storage_node_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_storage_node_test)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_storage_node_test)
endif()
gtest_add_tests(TARGET sitos_storage_node_test SOURCES unit/storage_node_test.cpp)

# --- Zenoh smoke test (integration) ---
if(SITOS_WITH_ZENOH)
  add_executable(sitos_zenoh_smoke_test
    integration/zenoh_smoke_test.cpp)
  target_link_libraries(sitos_zenoh_smoke_test
    PRIVATE GTest::gtest_main zenohc::zenohc)
  # Do NOT apply sitos_enable_warnings: the zenoh C header may trigger warnings
  # under -Werror that we cannot fix.
  sitos_copy_zenohc(sitos_zenoh_smoke_test)
  gtest_add_tests(
    TARGET sitos_zenoh_smoke_test
    SOURCES integration/zenoh_smoke_test.cpp)

  add_executable(sitos_transport_test
    integration/transport_test.cpp)
  target_link_libraries(sitos_transport_test
    PRIVATE GTest::gtest_main sitos::sitos)
  target_include_directories(sitos_transport_test PRIVATE ${PROJECT_SOURCE_DIR})
  sitos_enable_warnings(sitos_transport_test)
  sitos_copy_zenohc(sitos_transport_test)
  gtest_add_tests(TARGET sitos_transport_test SOURCES integration/transport_test.cpp)

  add_executable(sitos_storage_node_query_test
    integration/storage_node_query_test.cpp)
  target_link_libraries(sitos_storage_node_query_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_storage_node_query_test)
  sitos_copy_zenohc(sitos_storage_node_query_test)
  gtest_add_tests(
    TARGET sitos_storage_node_query_test
    SOURCES integration/storage_node_query_test.cpp)

  add_executable(sitos_storage_node_subscriber_test
    integration/storage_node_subscriber_test.cpp)
  target_link_libraries(sitos_storage_node_subscriber_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_storage_node_subscriber_test)
  sitos_copy_zenohc(sitos_storage_node_subscriber_test)
  gtest_add_tests(
    TARGET sitos_storage_node_subscriber_test
    SOURCES integration/storage_node_subscriber_test.cpp)

  add_executable(sitos_storage_node_lifecycle_test
    integration/storage_node_lifecycle_test.cpp)
  target_link_libraries(sitos_storage_node_lifecycle_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_storage_node_lifecycle_test)
  sitos_copy_zenohc(sitos_storage_node_lifecycle_test)
  gtest_add_tests(
    TARGET sitos_storage_node_lifecycle_test
    SOURCES integration/storage_node_lifecycle_test.cpp)

  add_executable(sitos_storage_node_session_test
    integration/storage_node_session_test.cpp)
  target_link_libraries(sitos_storage_node_session_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_storage_node_session_test)
  sitos_copy_zenohc(sitos_storage_node_session_test)
  gtest_add_tests(
    TARGET sitos_storage_node_session_test
    SOURCES integration/storage_node_session_test.cpp)

  add_executable(sitos_batch_test
    integration/batch_test.cpp)
  target_link_libraries(sitos_batch_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_batch_test)
  sitos_copy_zenohc(sitos_batch_test)
  gtest_add_tests(TARGET sitos_batch_test SOURCES integration/batch_test.cpp)

  add_executable(sitos_param_store_integration_test
    integration/param_store_test.cpp)
  target_link_libraries(sitos_param_store_integration_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_param_store_integration_test)
  sitos_copy_zenohc(sitos_param_store_integration_test)
  gtest_add_tests(
    TARGET sitos_param_store_integration_test
    SOURCES integration/param_store_test.cpp)
endif()
