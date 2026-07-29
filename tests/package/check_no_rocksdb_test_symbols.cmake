if(NOT DEFINED SITOS_PREFIX)
  message(FATAL_ERROR "SITOS_PREFIX must name an installed sitos prefix")
endif()
if(NOT DEFINED SITOS_INSTALL_LIBDIR)
  set(SITOS_INSTALL_LIBDIR lib)
endif()

if(WIN32)
  set(_archive "${SITOS_PREFIX}/${SITOS_INSTALL_LIBDIR}/sitos.lib")
  find_program(_symbol_tool NAMES dumpbin)
  if(NOT _symbol_tool)
    message(FATAL_ERROR "dumpbin is required for installed archive symbol validation")
  endif()
  execute_process(
    COMMAND "${_symbol_tool}" /SYMBOLS "${_archive}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _symbols
    ERROR_VARIABLE _error)
else()
  file(GLOB _archives "${SITOS_PREFIX}/${SITOS_INSTALL_LIBDIR}/libsitos.a")
  list(LENGTH _archives _archive_count)
  if(NOT _archive_count EQUAL 1)
    message(FATAL_ERROR "Expected one installed sitos archive, found ${_archive_count}")
  endif()
  list(GET _archives 0 _archive)
  find_program(_symbol_tool NAMES llvm-nm nm)
  if(NOT _symbol_tool)
    message(FATAL_ERROR "nm is required for installed archive symbol validation")
  endif()
  execute_process(
    COMMAND "${_symbol_tool}" -g "${_archive}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _symbols
    ERROR_VARIABLE _error)
endif()

if(NOT _result EQUAL 0)
  message(FATAL_ERROR "Unable to inspect installed archive ${_archive}: ${_error}")
endif()
if(_symbols MATCHES "rocksdb_test|SetOpenFailureForTest|SetFailures|GetSnapshotStats|GetEventLog")
  message(FATAL_ERROR "Installed production archive exposes RocksDB test-control symbols")
endif()
message(STATUS "Installed production archive contains no RocksDB test-control symbols")
