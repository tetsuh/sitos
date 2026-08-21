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
add_sitos_public_header_compile_test(
  sitos_param_cache_header_compile_test consumer/param_cache_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_param_concepts_header_compile_test consumer/param_concepts_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_list_sink_header_compile_test consumer/list_sink_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_param_subscription_header_compile_test consumer/param_subscription_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_session_view_header_compile_test consumer/session_view_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_rocksdb_engine_header_compile_test consumer/rocksdb_engine_header_compile.cpp)
add_sitos_public_header_compile_test(
  sitos_storage_node_header_compile_test consumer/storage_node_header_compile.cpp)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_param_store_header_compile_test)
  sitos_copy_zenohc(sitos_param_cache_header_compile_test)
  sitos_copy_zenohc(sitos_param_concepts_header_compile_test)
  sitos_copy_zenohc(sitos_list_sink_header_compile_test)
  sitos_copy_zenohc(sitos_param_subscription_header_compile_test)
  sitos_copy_zenohc(sitos_session_view_header_compile_test)
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

add_executable(sitos_ack_codec_test unit/ack_codec_test.cpp)
target_link_libraries(sitos_ack_codec_test PRIVATE GTest::gtest_main sitos::sitos)
target_compile_definitions(sitos_ack_codec_test PRIVATE
  SITOS_ACK_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/ack_v1")
sitos_enable_warnings(sitos_ack_codec_test)
gtest_add_tests(TARGET sitos_ack_codec_test SOURCES unit/ack_codec_test.cpp)

add_executable(sitos_result_test unit/result_test.cpp)
target_link_libraries(sitos_result_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_result_test)
gtest_add_tests(TARGET sitos_result_test SOURCES unit/result_test.cpp)

add_executable(sitos_python_status_translation_test unit/python_status_translation_test.cpp)
target_link_libraries(sitos_python_status_translation_test PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_python_status_translation_test PRIVATE ${PROJECT_SOURCE_DIR})
sitos_enable_warnings(sitos_python_status_translation_test)
gtest_add_tests(
  TARGET sitos_python_status_translation_test
  SOURCES unit/python_status_translation_test.cpp)

add_executable(sitos_client_config_test unit/client_config_test.cpp)
target_link_libraries(sitos_client_config_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_client_config_test)
gtest_add_tests(TARGET sitos_client_config_test SOURCES unit/client_config_test.cpp)

add_executable(sitos_param_cache_attach_test unit/param_cache_attach_test.cpp)
target_link_libraries(sitos_param_cache_attach_test PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_param_cache_attach_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_param_cache_attach_test)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_param_cache_attach_test)
endif()
gtest_add_tests(TARGET sitos_param_cache_attach_test SOURCES unit/param_cache_attach_test.cpp)

add_executable(sitos_param_cache_read_test unit/param_cache_read_test.cpp)
target_link_libraries(sitos_param_cache_read_test PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_param_cache_read_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_param_cache_read_test)
gtest_add_tests(TARGET sitos_param_cache_read_test SOURCES unit/param_cache_read_test.cpp)

add_executable(sitos_param_store_test unit/param_store_test.cpp)
target_link_libraries(sitos_param_store_test PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_param_store_test)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_param_store_test)
endif()
gtest_add_tests(TARGET sitos_param_store_test SOURCES unit/param_store_test.cpp)

add_executable(sitos_param_store_subscribe_test unit/param_store_subscribe_test.cpp)
target_link_libraries(sitos_param_store_subscribe_test PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_param_store_subscribe_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_param_store_subscribe_test)
gtest_add_tests(
  TARGET sitos_param_store_subscribe_test
  SOURCES unit/param_store_subscribe_test.cpp)

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
if(NOT SITOS_WITH_ROCKSDB)
  add_executable(sitos_rocksdb_engine_off_test unit/rocksdb_engine_off_test.cpp)
  target_link_libraries(sitos_rocksdb_engine_off_test PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_rocksdb_engine_off_test)
  gtest_add_tests(TARGET sitos_rocksdb_engine_off_test SOURCES unit/rocksdb_engine_off_test.cpp)
endif()

add_executable(sitos_storage_engine_contract_test
  unit/storage_engine_contract_test.cpp)
target_link_libraries(sitos_storage_engine_contract_test
  PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_storage_engine_contract_test)
gtest_discover_tests(sitos_storage_engine_contract_test DISCOVERY_MODE PRE_TEST)

if(SITOS_WITH_ROCKSDB)
  # The contract suite links the production archive so it exercises the exact
  # RocksDB implementation that is installed for consumers.
  add_executable(sitos_rocksdb_engine_test
    unit/rocksdb_engine_test.cpp)
  target_link_libraries(sitos_rocksdb_engine_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_rocksdb_engine_test)
  gtest_discover_tests(sitos_rocksdb_engine_test DISCOVERY_MODE PRE_TEST)

  # Native failure and lifecycle instrumentation stay in a separate binary.
  # It supplies the seam translation unit directly and never links sitos::sitos,
  # avoiding duplicate RocksDBEngine definitions in one executable.
  add_library(sitos_rocksdb_test_engine OBJECT ../src/rocksdb_engine.cpp)
  target_compile_definitions(sitos_rocksdb_test_engine PRIVATE
    SITOS_WITH_ROCKSDB=1 SITOS_ROCKSDB_TEST_SEAM=1)
  target_include_directories(sitos_rocksdb_test_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(sitos_rocksdb_test_engine PRIVATE RocksDB::rocksdb)
  sitos_enable_warnings(sitos_rocksdb_test_engine)

  add_executable(sitos_rocksdb_test_access_test
    unit/rocksdb_engine_test_access_test.cpp
    ../src/status.cpp
    ../src/storage_engine.cpp)
  target_sources(sitos_rocksdb_test_access_test PRIVATE
    $<TARGET_OBJECTS:sitos_rocksdb_test_engine>)
  target_link_libraries(sitos_rocksdb_test_access_test
    PRIVATE GTest::gtest_main RocksDB::rocksdb)
  target_include_directories(sitos_rocksdb_test_access_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
  sitos_enable_warnings(sitos_rocksdb_test_access_test)
  gtest_discover_tests(sitos_rocksdb_test_access_test DISCOVERY_MODE PRE_TEST)

  add_executable(sitos_rocksdb_buffer_lifecycle_test
    integration/rocksdb_buffer_lifecycle_test.cpp)
  target_link_libraries(sitos_rocksdb_buffer_lifecycle_test
    PRIVATE GTest::gtest_main sitos::sitos)
  target_include_directories(sitos_rocksdb_buffer_lifecycle_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  sitos_enable_warnings(sitos_rocksdb_buffer_lifecycle_test)
  gtest_add_tests(
    TARGET sitos_rocksdb_buffer_lifecycle_test
    SOURCES integration/rocksdb_buffer_lifecycle_test.cpp)
  set_tests_properties(
    RocksDBBufferLifecycleTest.CloseReleasesEngineBeforeReturn
    RocksDBBufferLifecycleTest.SameSidRecreationUsesFreshEngine
    PROPERTIES TIMEOUT 60)
endif()

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

add_executable(sitos_ack_registry_test unit/ack_registry_test.cpp)
target_link_libraries(sitos_ack_registry_test PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_ack_registry_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_ack_registry_test)
gtest_add_tests(TARGET sitos_ack_registry_test SOURCES unit/ack_registry_test.cpp)

add_executable(sitos_storage_node_ack_test unit/storage_node_ack_test.cpp)
target_link_libraries(sitos_storage_node_ack_test PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_storage_node_ack_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_storage_node_ack_test)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_storage_node_ack_test)
endif()
gtest_add_tests(TARGET sitos_storage_node_ack_test SOURCES unit/storage_node_ack_test.cpp)

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

add_executable(sitos_storage_node_buffer_api_test
  unit/storage_node_buffer_api_test.cpp)
target_link_libraries(sitos_storage_node_buffer_api_test
  PRIVATE GTest::gtest_main sitos::sitos)
sitos_enable_warnings(sitos_storage_node_buffer_api_test)
gtest_add_tests(TARGET sitos_storage_node_buffer_api_test SOURCES unit/storage_node_buffer_api_test.cpp)

add_executable(sitos_storage_node_buffer_routing_test
  unit/storage_node_buffer_routing_test.cpp)
target_link_libraries(sitos_storage_node_buffer_routing_test
  PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_storage_node_buffer_routing_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_storage_node_buffer_routing_test)
gtest_add_tests(TARGET sitos_storage_node_buffer_routing_test SOURCES unit/storage_node_buffer_routing_test.cpp)

add_executable(sitos_storage_node_buffer_lifecycle_test
  unit/storage_node_buffer_lifecycle_test.cpp)
target_link_libraries(sitos_storage_node_buffer_lifecycle_test
  PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_storage_node_buffer_lifecycle_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_storage_node_buffer_lifecycle_test)
gtest_add_tests(TARGET sitos_storage_node_buffer_lifecycle_test SOURCES unit/storage_node_buffer_lifecycle_test.cpp)

add_executable(sitos_buffer_late_join_test
  integration/buffer_late_join_test.cpp)
target_link_libraries(sitos_buffer_late_join_test
  PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_buffer_late_join_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_buffer_late_join_test)
gtest_add_tests(
  TARGET sitos_buffer_late_join_test
  SOURCES integration/buffer_late_join_test.cpp)
set_tests_properties(
  BufferLateJoinTest.OrdersMaterializedBufferedAndLiveSamples
  BufferLateJoinTest.BufferedIdenticalReplyPromotesToMaterializedCategory
  BufferLateJoinTest.BufferedConflictingReplyFailsBeforeObserverDelivery
  BufferLateJoinTest.ObservationBoundarySerializesInitialBatchAndLiveSample
  BufferLateJoinTest.DurableLateJoinDoesNotLoseDistinctKeys
  BufferLateJoinTest.FailureInvokesNoObserverAndCleansUp
  PROPERTIES TIMEOUT 60)

add_executable(sitos_session_view_test
  unit/session_view_test.cpp)
target_link_libraries(sitos_session_view_test
  PRIVATE GTest::gtest_main sitos::sitos)
target_include_directories(sitos_session_view_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
sitos_enable_warnings(sitos_session_view_test)
if(SITOS_WITH_ZENOH)
  sitos_copy_zenohc(sitos_session_view_test)
endif()
gtest_add_tests(TARGET sitos_session_view_test SOURCES unit/session_view_test.cpp)

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

  add_executable(sitos_raw_zenoh_fixture
    interop/raw_zenoh_fixture.cpp)
  target_link_libraries(sitos_raw_zenoh_fixture PRIVATE sitos::sitos)
  sitos_enable_warnings(sitos_raw_zenoh_fixture)
  sitos_copy_zenohc(sitos_raw_zenoh_fixture)

  add_executable(sitos_raw_zenoh_buffer_fixture
    interop/raw_zenoh_buffer_fixture.cpp)
  target_link_libraries(sitos_raw_zenoh_buffer_fixture PRIVATE sitos::sitos)
  if(SITOS_WITH_ROCKSDB)
    target_compile_definitions(sitos_raw_zenoh_buffer_fixture PRIVATE SITOS_WITH_ROCKSDB=1)
  endif()
  sitos_enable_warnings(sitos_raw_zenoh_buffer_fixture)
  sitos_copy_zenohc(sitos_raw_zenoh_buffer_fixture)

  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME RawZenohClientCanPutAndGet
    COMMAND "${Python3_EXECUTABLE}" -m pytest -q
            "${CMAKE_CURRENT_SOURCE_DIR}/interop/test_raw_zenoh_single.py"
            -k test_raw_zenoh_client_can_put_and_get)
  add_test(
    NAME RawZenohInteropFixtureBoundaries
    COMMAND "${Python3_EXECUTABLE}" -m pytest -q
            "${CMAKE_CURRENT_SOURCE_DIR}/interop/test_raw_zenoh_single.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/interop/test_raw_zenoh_wheel_boundary.py"
            -k "not test_raw_zenoh_client_can_put_and_get")
  add_test(
    NAME RawZenohClientCanUseMixedSessionBuffers
    COMMAND "${Python3_EXECUTABLE}" -m pytest -q
            "${CMAKE_CURRENT_SOURCE_DIR}/interop/test_raw_zenoh_buffer.py"
            -k test_raw_zenoh_client_can_use_mixed_session_buffers)
  add_test(
    NAME RawZenohDurableLateJoinPreservesDistinctKeys
    COMMAND "${Python3_EXECUTABLE}" -m pytest -q
            "${CMAKE_CURRENT_SOURCE_DIR}/interop/test_raw_zenoh_buffer.py"
            -k test_raw_zenoh_durable_late_join_preserves_distinct_keys)
  add_test(
    NAME RawZenohBufferInteropFixtureBoundaries
    COMMAND "${Python3_EXECUTABLE}" -m pytest -q
            "${CMAKE_CURRENT_SOURCE_DIR}/interop/test_raw_zenoh_buffer.py"
            -k test_raw_zenoh_buffer_interop_fixture_boundaries)
  set_tests_properties(
    RawZenohClientCanPutAndGet RawZenohInteropFixtureBoundaries
    RawZenohClientCanUseMixedSessionBuffers RawZenohDurableLateJoinPreservesDistinctKeys
    RawZenohBufferInteropFixtureBoundaries
    PROPERTIES
      ENVIRONMENT "SITOS_RAW_ZENOH_FIXTURE=$<TARGET_FILE:sitos_raw_zenoh_fixture>"
      RUN_SERIAL TRUE
      TIMEOUT 60)
  set_tests_properties(
    RawZenohClientCanUseMixedSessionBuffers RawZenohDurableLateJoinPreservesDistinctKeys
    RawZenohBufferInteropFixtureBoundaries
    PROPERTIES
      ENVIRONMENT "SITOS_RAW_ZENOH_FIXTURE=$<TARGET_FILE:sitos_raw_zenoh_buffer_fixture>"
      RUN_SERIAL TRUE
      TIMEOUT 60)

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

  add_executable(sitos_param_store_subscribe_integration_test
    integration/param_store_subscribe_test.cpp)
  target_link_libraries(sitos_param_store_subscribe_integration_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_param_store_subscribe_integration_test)
  sitos_copy_zenohc(sitos_param_store_subscribe_integration_test)
  gtest_add_tests(
    TARGET sitos_param_store_subscribe_integration_test
    SOURCES integration/param_store_subscribe_test.cpp)

  add_executable(sitos_session_view_integration_test
    integration/session_view_test.cpp)
  target_link_libraries(sitos_session_view_integration_test
    PRIVATE GTest::gtest_main sitos::sitos)
  sitos_enable_warnings(sitos_session_view_integration_test)
  sitos_copy_zenohc(sitos_session_view_integration_test)
  gtest_add_tests(
    TARGET sitos_session_view_integration_test
    SOURCES integration/session_view_test.cpp)

  if(SITOS_BUILD_PYTHON)
    add_executable(sitos_python_param_cache_fixture
      python/param_cache_fixture.cpp)
    target_link_libraries(sitos_python_param_cache_fixture PRIVATE sitos::sitos)
    sitos_enable_warnings(sitos_python_param_cache_fixture)
    sitos_copy_zenohc(sitos_python_param_cache_fixture)

    add_executable(sitos_python_param_store_fixture
      python/param_store_fixture.cpp)
    target_link_libraries(sitos_python_param_store_fixture PRIVATE sitos::sitos)
    sitos_enable_warnings(sitos_python_param_store_fixture)
    sitos_copy_zenohc(sitos_python_param_store_fixture)
  endif()

  add_executable(sitos_param_cache_integration_test
    integration/param_cache_attach_test.cpp)
  target_link_libraries(sitos_param_cache_integration_test
    PRIVATE GTest::gtest_main sitos::sitos)
  target_include_directories(sitos_param_cache_integration_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  sitos_enable_warnings(sitos_param_cache_integration_test)
  sitos_copy_zenohc(sitos_param_cache_integration_test)
  gtest_add_tests(
    TARGET sitos_param_cache_integration_test
    SOURCES integration/param_cache_attach_test.cpp)
endif()
