add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE sitos::sitos)
sitos_enable_warnings(quickstart)
sitos_copy_zenohc(quickstart)

add_executable(sitobolon sitobolon.cpp)
target_link_libraries(sitobolon PRIVATE sitos::sitos)
if(SITOS_WITH_ROCKSDB)
  target_compile_definitions(sitobolon PRIVATE SITOS_WITH_ROCKSDB=1)
endif()
sitos_enable_warnings(sitobolon)
sitos_copy_zenohc(sitobolon)

if(SITOS_BUILD_TESTS)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  set(_cpp_example_driver "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/examples/test_cpp_examples.py")
  set(_cpp_example_driver_rocksdb_arg)
  if(SITOS_WITH_ROCKSDB)
    set(_cpp_example_driver_rocksdb_arg --rocksdb-built)
  endif()

  add_test(NAME CppQuickstartRuns COMMAND "$<TARGET_FILE:quickstart>")
  add_test(NAME SitobolonHelpDocumentsOptions
    COMMAND "${Python3_EXECUTABLE}" "${_cpp_example_driver}" help
            --executable "$<TARGET_FILE:sitobolon>")
  add_test(NAME SitobolonRejectsInvalidArguments
    COMMAND "${Python3_EXECUTABLE}" "${_cpp_example_driver}" invalid
            --executable "$<TARGET_FILE:sitobolon>" ${_cpp_example_driver_rocksdb_arg})
  add_test(NAME SitobolonStartsAndStopsCleanly
    COMMAND "${Python3_EXECUTABLE}" "${_cpp_example_driver}" lifecycle
            --executable "$<TARGET_FILE:sitobolon>")
  set_tests_properties(
    CppQuickstartRuns SitobolonHelpDocumentsOptions SitobolonRejectsInvalidArguments
    SitobolonStartsAndStopsCleanly
    PROPERTIES TIMEOUT 60)
  set_tests_properties(
    CppQuickstartRuns SitobolonStartsAndStopsCleanly
    PROPERTIES RUN_SERIAL TRUE)

  if(SITOS_WITH_ROCKSDB)
    add_test(NAME SitobolonRocksDbReleasesPath
      COMMAND "${Python3_EXECUTABLE}" "${_cpp_example_driver}" rocksdb
              --executable "$<TARGET_FILE:sitobolon>")
    set_tests_properties(SitobolonRocksDbReleasesPath PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  endif()
endif()
