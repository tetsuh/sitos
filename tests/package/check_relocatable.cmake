foreach(_required_variable SITOS_PREFIX SITOS_SOURCE_DIR SITOS_BUILD_DIR ORIGINAL_PREFIX)
  if(NOT DEFINED ${_required_variable})
    message(FATAL_ERROR "${_required_variable} must be defined")
  endif()
endforeach()

file(GLOB_RECURSE _package_files
  "${SITOS_PREFIX}/lib/cmake/sitos/*.cmake")
set(_forbidden_strings
  "${SITOS_SOURCE_DIR}"
  "${SITOS_BUILD_DIR}"
  "${ORIGINAL_PREFIX}")
foreach(_package_file IN LISTS _package_files)
  file(READ "${_package_file}" _package_contents)
  foreach(_forbidden_string IN LISTS _forbidden_strings)
    string(FIND "${_package_contents}" "${_forbidden_string}" _match)
    if(NOT _match EQUAL -1)
      message(FATAL_ERROR
        "Non-relocatable path '${_forbidden_string}' found in ${_package_file}")
    endif()
  endforeach()
endforeach()
