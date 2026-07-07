include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(sitos_bootstrap_test unit/bootstrap_test.cpp)
target_link_libraries(sitos_bootstrap_test GTest::gtest_main sitos::sitos)

add_executable(sitos_key_test unit/key_test.cpp)
target_link_libraries(sitos_key_test GTest::gtest_main sitos::sitos)

include(GoogleTest)
gtest_discover_tests(sitos_bootstrap_test)
gtest_discover_tests(sitos_key_test)
