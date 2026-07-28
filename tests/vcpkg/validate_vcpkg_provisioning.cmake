cmake_minimum_required(VERSION 3.20)

foreach(_required_arg IN ITEMS SITOS_SOURCE_DIR VCPKG_ROOT WORK_DIR TRIPLET)
  if(NOT DEFINED ${_required_arg} OR "${${_required_arg}}" STREQUAL "")
    message(FATAL_ERROR "${_required_arg} must be provided")
  endif()
endforeach()
if(NOT DEFINED CMAKE_GENERATOR OR "${CMAKE_GENERATOR}" STREQUAL "")
  set(CMAKE_GENERATOR Ninja)
endif()
set(_generator_args -G "${CMAKE_GENERATOR}")
if(DEFINED CMAKE_MAKE_PROGRAM AND NOT "${CMAKE_MAKE_PROGRAM}" STREQUAL "")
  list(APPEND _generator_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()

function(run_checked)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Command failed with exit code ${_result}: ${ARGV}")
  endif()
endfunction()

set(_manifest "${SITOS_SOURCE_DIR}/vcpkg.json")
if(NOT EXISTS "${_manifest}")
  message(FATAL_ERROR
    "VcpkgRocksDBFeatureConfigures RED: root vcpkg.json is absent; pinned feature provisioning is unavailable")
endif()
file(READ "${_manifest}" _manifest_text)
string(JSON _baseline ERROR_VARIABLE _baseline_error GET "${_manifest_text}" "builtin-baseline")
string(LENGTH "${_baseline}" _baseline_length)
if(_baseline_error OR NOT _baseline_length EQUAL 40 OR NOT _baseline MATCHES "^[0-9a-fA-F]+$")
  message(FATAL_ERROR "vcpkg.json builtin-baseline must be a 40-hex commit")
endif()

execute_process(
  COMMAND git -C "${VCPKG_ROOT}" rev-parse HEAD
  RESULT_VARIABLE _git_result
  OUTPUT_VARIABLE _vcpkg_head
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_VARIABLE _git_error)
if(NOT _git_result EQUAL 0 OR NOT _vcpkg_head STREQUAL _baseline)
  message(FATAL_ERROR
    "vcpkg checkout does not match vcpkg.json builtin-baseline: expected ${_baseline}, got ${_vcpkg_head}; ${_git_error}")
endif()

if(IS_ABSOLUTE "${WORK_DIR}")
  set(_work_dir "${WORK_DIR}")
else()
  set(_work_dir "${CMAKE_CURRENT_BINARY_DIR}/${WORK_DIR}")
endif()
file(REMOVE_RECURSE "${_work_dir}")
file(MAKE_DIRECTORY "${_work_dir}")
set(_feature_build "${_work_dir}/feature-build")
set(_feature_install "${_work_dir}/feature-installed")
set(_feature_stage "${_work_dir}/feature-stage")
set(_default_build "${_work_dir}/default-build")
set(_default_install "${_work_dir}/default-installed")
if(_feature_install STREQUAL _default_install)
  message(FATAL_ERROR "Feature and no-feature install roots must differ")
endif()
file(MAKE_DIRECTORY "${_feature_stage}")

set(_toolchain "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
run_checked(
  "${CMAKE_COMMAND}" -S "${SITOS_SOURCE_DIR}/tests/vcpkg/consumer" -B "${_feature_build}"
  ${_generator_args} -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_TOOLCHAIN_FILE=${_toolchain}"
  "-DVCPKG_MANIFEST_DIR=${SITOS_SOURCE_DIR}"
  -DVCPKG_MANIFEST_FEATURES=rocksdb
  "-DVCPKG_TARGET_TRIPLET=${TRIPLET}"
  "-DVCPKG_INSTALLED_DIR=${_feature_install}"
  "-DPROBE_RUNTIME_DIR=${_feature_stage}")
run_checked("${CMAKE_COMMAND}" --build "${_feature_build}")

set(_sitos_build "${_work_dir}/sitos-rocksdb-build")
set(_sitos_prefix "${_work_dir}/sitos-rocksdb-prefix")
set(_sitos_consumer_build "${_work_dir}/sitos-rocksdb-consumer-build")
run_checked(
  "${CMAKE_COMMAND}" -S "${SITOS_SOURCE_DIR}" -B "${_sitos_build}"
  ${_generator_args} -DCMAKE_BUILD_TYPE=Release
  -DSITOS_BUILD_TESTS=OFF -DSITOS_WITH_ZENOH=OFF -DSITOS_WITH_ROCKSDB=ON
  "-DCMAKE_TOOLCHAIN_FILE=${_toolchain}"
  "-DVCPKG_MANIFEST_DIR=${SITOS_SOURCE_DIR}"
  -DVCPKG_MANIFEST_FEATURES=rocksdb
  "-DVCPKG_TARGET_TRIPLET=${TRIPLET}"
  "-DVCPKG_INSTALLED_DIR=${_feature_install}")
run_checked("${CMAKE_COMMAND}" --build "${_sitos_build}")
run_checked("${CMAKE_COMMAND}" --install "${_sitos_build}" --prefix "${_sitos_prefix}")
run_checked(
  "${CMAKE_COMMAND}" -S "${SITOS_SOURCE_DIR}/tests/package/consumer" -B "${_sitos_consumer_build}"
  ${_generator_args} -DCMAKE_BUILD_TYPE=Release
  -DSITOS_PACKAGE_CONSUMER_WITH_ROCKSDB=ON
  "-DCMAKE_PREFIX_PATH=${_sitos_prefix};${_feature_install}/${TRIPLET}")
run_checked("${CMAKE_COMMAND}" --build "${_sitos_consumer_build}")

if(WIN32)
  # The pinned baseline's zlib 1.3.2 port exports the canonical z.dll name.
  # Never rename it or add a compatibility copy under the pre-1.3.2 name.
  set(_zlib_dll "${_feature_install}/${TRIPLET}/bin/z.dll")
  if(NOT EXISTS "${_zlib_dll}")
    message(FATAL_ERROR "Expected canonical z.dll was not provisioned for x64-windows")
  endif()
  run_checked("${CMAKE_COMMAND}" -E copy_if_different "${_zlib_dll}" "${_feature_stage}/z.dll")
  if(NOT EXISTS "${_feature_stage}/z.dll")
    message(FATAL_ERROR "Failed to stage canonical z.dll")
  endif()
  if(EXISTS "${_feature_stage}/zlib1.dll")
    message(FATAL_ERROR "Unexpected zlib1.dll compatibility artifact was staged")
  endif()

  execute_process(
    COMMAND dumpbin /DEPENDENTS "${_feature_stage}/rocksdb_consumer.exe"
    RESULT_VARIABLE _dumpbin_result
    OUTPUT_VARIABLE _dumpbin_output
    ERROR_VARIABLE _dumpbin_error)
  if(NOT _dumpbin_result EQUAL 0)
    message(FATAL_ERROR "Unable to inspect Windows probe dependencies: ${_dumpbin_error}")
  endif()
  string(TOLOWER "${_dumpbin_output}" _dumpbin_lower)
  if(NOT _dumpbin_lower MATCHES "(^|[^a-z0-9_])z\\.dll([^a-z0-9_]|$)")
    message(FATAL_ERROR "Windows probe dependency inspection did not report canonical z.dll")
  endif()
  if(NOT _dumpbin_lower MATCHES "vcruntime140(_1)?\\.dll" OR
     NOT _dumpbin_lower MATCHES "msvcp140(_atomic_wait)?\\.dll")
    message(FATAL_ERROR "Windows probe does not report the expected dynamic MSVC CRT dependencies")
  endif()
  if(_dumpbin_lower MATCHES "rocksdb-shared\\.dll")
    message(FATAL_ERROR "Canonical RocksDB::rocksdb probe depends on rocksdb-shared.dll")
  endif()
endif()

# Execute only from the clean stage directory. In particular, do not add a vcpkg
# or build-tree directory to PATH to make runtime discovery succeed accidentally.
set(_clean_path "${_feature_stage}")
run_checked(
  "${CMAKE_COMMAND}" -E env "PATH=${_clean_path}"
  "${CMAKE_CTEST_COMMAND}" --test-dir "${_feature_build}" --output-on-failure
  -R "VcpkgRocksDBFeatureConfigures|VcpkgRocksDBConsumerLinksAndRuns")

run_checked(
  "${CMAKE_COMMAND}" -S "${SITOS_SOURCE_DIR}" -B "${_default_build}"
  ${_generator_args} -DCMAKE_BUILD_TYPE=Release
  -DSITOS_BUILD_TESTS=OFF -DSITOS_WITH_ROCKSDB=OFF -DSITOS_WITH_ZENOH=OFF
  "-DCMAKE_TOOLCHAIN_FILE=${_toolchain}"
  "-DVCPKG_MANIFEST_DIR=${SITOS_SOURCE_DIR}"
  "-DVCPKG_TARGET_TRIPLET=${TRIPLET}"
  "-DVCPKG_INSTALLED_DIR=${_default_install}")
run_checked("${CMAKE_COMMAND}" --build "${_default_build}")
run_checked(
  "${CMAKE_COMMAND}" "-DVCPKG_INSTALLED_DIR=${_default_install}"
  -P "${SITOS_SOURCE_DIR}/tests/vcpkg/check_default_no_rocksdb.cmake")
message(STATUS "VcpkgDefaultPathDoesNotProvisionRocksDB: isolated no-feature build passed")
