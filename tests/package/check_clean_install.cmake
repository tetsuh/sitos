if(NOT DEFINED SITOS_PREFIX)
  message(FATAL_ERROR "SITOS_PREFIX must name an installed sitos prefix")
endif()

set(_forbidden_paths
  "${SITOS_PREFIX}/include/gtest"
  "${SITOS_PREFIX}/include/gmock"
  "${SITOS_PREFIX}/lib/cmake/GTest"
  "${SITOS_PREFIX}/lib/pkgconfig/gtest.pc"
  "${SITOS_PREFIX}/lib/pkgconfig/gmock.pc")
foreach(_path IN LISTS _forbidden_paths)
  if(EXISTS "${_path}")
    message(FATAL_ERROR "Test-only artifact was installed: ${_path}")
  endif()
endforeach()

file(GLOB_RECURSE _test_libraries
  "${SITOS_PREFIX}/lib/*gtest*"
  "${SITOS_PREFIX}/lib/*gmock*")
if(_test_libraries)
  list(JOIN _test_libraries "\n  " _test_library_list)
  message(FATAL_ERROR "Test-only libraries were installed:\n  ${_test_library_list}")
endif()
