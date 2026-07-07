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
