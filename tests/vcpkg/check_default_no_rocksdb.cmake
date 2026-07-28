if(NOT DEFINED VCPKG_INSTALLED_DIR)
  message(FATAL_ERROR "VCPKG_INSTALLED_DIR must name the isolated no-feature install root")
endif()

# A manifest with no dependencies may leave no install root or status database. Both
# states are positive absence evidence, not errors.
if(NOT EXISTS "${VCPKG_INSTALLED_DIR}")
  message(STATUS "VcpkgDefaultPathDoesNotProvisionRocksDB: install root is absent (pass)")
  return()
endif()

file(GLOB_RECURSE _status_files LIST_DIRECTORIES false
  "${VCPKG_INSTALLED_DIR}/vcpkg/status"
  "${VCPKG_INSTALLED_DIR}/*/vcpkg/status")
foreach(_status_file IN LISTS _status_files)
  file(READ "${_status_file}" _status)
  if(_status MATCHES "(^|\\n)Package:[ \\t]*rocksdb([ \\t]*\\n|$)")
    message(FATAL_ERROR "RocksDB status entry found in ${_status_file}")
  endif()
endforeach()

file(GLOB_RECURSE _rocksdb_headers LIST_DIRECTORIES false
  "${VCPKG_INSTALLED_DIR}/*/include/rocksdb/*.h"
  "${VCPKG_INSTALLED_DIR}/*/include/rocksdb/*.hpp")
file(GLOB_RECURSE _rocksdb_libraries LIST_DIRECTORIES false
  "${VCPKG_INSTALLED_DIR}/*/lib/*rocksdb*"
  "${VCPKG_INSTALLED_DIR}/*/debug/lib/*rocksdb*")
file(GLOB_RECURSE _rocksdb_dlls LIST_DIRECTORIES false
  "${VCPKG_INSTALLED_DIR}/*/bin/*rocksdb*.dll"
  "${VCPKG_INSTALLED_DIR}/*/debug/bin/*rocksdb*.dll")
if(_rocksdb_headers OR _rocksdb_libraries OR _rocksdb_dlls)
  message(FATAL_ERROR
    "RocksDB artifacts found in no-feature install root: "
    "headers=${_rocksdb_headers}; libraries=${_rocksdb_libraries}; dlls=${_rocksdb_dlls}")
endif()
message(STATUS "VcpkgDefaultPathDoesNotProvisionRocksDB: no status entry or artifacts (pass)")
