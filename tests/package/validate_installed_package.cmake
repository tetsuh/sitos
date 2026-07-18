if(NOT DEFINED SITOS_SOURCE_DIR)
  message(FATAL_ERROR "SITOS_SOURCE_DIR must name the sitos source tree")
endif()
if(NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "WORK_DIR must name the package validation work directory")
endif()
if(NOT DEFINED CMAKE_GENERATOR)
  set(CMAKE_GENERATOR Ninja)
endif()

function(run_checked)
  execute_process(
    COMMAND ${ARGV}
    RESULT_VARIABLE _result)
  if(_result)
    message(FATAL_ERROR "Command failed with exit code ${_result}: ${ARGV}")
  endif()
endfunction()

function(validate_clean_install prefix)
  run_checked(
    "${CMAKE_COMMAND}"
    "-DSITOS_PREFIX=${prefix}"
    -P "${SITOS_SOURCE_DIR}/tests/package/check_clean_install.cmake")
endfunction()

function(validate_relocation prefix original_prefix build_dir)
  run_checked(
    "${CMAKE_COMMAND}"
    "-DSITOS_PREFIX=${prefix}"
    "-DSITOS_SOURCE_DIR=${SITOS_SOURCE_DIR}"
    "-DSITOS_BUILD_DIR=${build_dir}"
    "-DORIGINAL_PREFIX=${original_prefix}"
    -P "${SITOS_SOURCE_DIR}/tests/package/check_relocatable.cmake")
endfunction()

function(validate_consumer prefix build_dir)
  set(_extra_args ${ARGN})
  run_checked(
    "${CMAKE_COMMAND}"
    "-DSITOS_PREFIX=${prefix}"
    "-DSITOS_SOURCE_DIR=${SITOS_SOURCE_DIR}"
    "-DCONSUMER_BUILD_DIR=${build_dir}"
    "-DCMAKE_GENERATOR=${CMAKE_GENERATOR}"
    ${_extra_args}
    -P "${SITOS_SOURCE_DIR}/tests/package/run_installed_consumer.cmake")
endfunction()

if(EXISTS "${WORK_DIR}")
  run_checked("${CMAKE_COMMAND}" -E rm -rf "${WORK_DIR}")
endif()

set(_test_build "${WORK_DIR}/test-install-build")
set(_test_prefix "${WORK_DIR}/test-install-prefix")
run_checked(
  "${CMAKE_COMMAND}" -S "${SITOS_SOURCE_DIR}" -B "${_test_build}"
  -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE=Release
  -DSITOS_BUILD_TESTS=ON -DSITOS_WITH_ZENOH=OFF)
run_checked("${CMAKE_COMMAND}" --build "${_test_build}")
run_checked("${CMAKE_COMMAND}" --install "${_test_build}" --prefix "${_test_prefix}")
validate_clean_install("${_test_prefix}")

set(_off_build "${WORK_DIR}/off-build")
set(_off_original "${WORK_DIR}/off-original")
set(_off_moved "${WORK_DIR}/off-moved")
set(_off_consumer "${WORK_DIR}/off-consumer")
run_checked(
  "${CMAKE_COMMAND}" -S "${SITOS_SOURCE_DIR}" -B "${_off_build}"
  -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE=Release
  -DSITOS_BUILD_TESTS=OFF -DSITOS_WITH_ZENOH=OFF)
run_checked("${CMAKE_COMMAND}" --build "${_off_build}")
run_checked("${CMAKE_COMMAND}" --install "${_off_build}" --prefix "${_off_original}")
validate_clean_install("${_off_original}")
run_checked("${CMAKE_COMMAND}" -E copy_directory "${_off_original}" "${_off_moved}")
validate_relocation("${_off_moved}" "${_off_original}" "${_off_build}")
validate_consumer("${_off_moved}" "${_off_consumer}")

set(_on_build "${WORK_DIR}/on-build")
set(_on_original "${WORK_DIR}/on-original")
set(_on_moved "${WORK_DIR}/on-moved")
set(_on_consumer "${WORK_DIR}/on-consumer")
set(_zenoh_root "${_on_build}/_deps/zenohc-src")
run_checked(
  "${CMAKE_COMMAND}" -S "${SITOS_SOURCE_DIR}" -B "${_on_build}"
  -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE=Release
  -DSITOS_BUILD_TESTS=OFF -DSITOS_WITH_ZENOH=ON)
run_checked("${CMAKE_COMMAND}" --build "${_on_build}")
run_checked("${CMAKE_COMMAND}" --install "${_on_build}" --prefix "${_on_original}")
validate_clean_install("${_on_original}")
run_checked("${CMAKE_COMMAND}" -E copy_directory "${_on_original}" "${_on_moved}")
validate_relocation("${_on_moved}" "${_on_original}" "${_on_build}")
validate_consumer("${_on_moved}" "${_on_consumer}-cmake-root"
  "-DZENOHC_ROOT=${_zenoh_root}")
validate_consumer("${_on_moved}" "${_on_consumer}-environment-root"
  "-DZENOHC_ROOT_ENV=${_zenoh_root}")
