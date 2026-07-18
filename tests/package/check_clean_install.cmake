if(NOT DEFINED SITOS_PREFIX)
  message(FATAL_ERROR "SITOS_PREFIX must name an installed sitos prefix")
endif()
if(NOT DEFINED SITOS_INSTALL_LIBDIR)
  set(SITOS_INSTALL_LIBDIR lib)
endif()

set(_forbidden_paths
  "${SITOS_PREFIX}/include/gtest"
  "${SITOS_PREFIX}/include/gmock"
  "${SITOS_PREFIX}/${SITOS_INSTALL_LIBDIR}/cmake/GTest"
  "${SITOS_PREFIX}/${SITOS_INSTALL_LIBDIR}/pkgconfig/gtest.pc"
  "${SITOS_PREFIX}/${SITOS_INSTALL_LIBDIR}/pkgconfig/gmock.pc")
foreach(_path IN LISTS _forbidden_paths)
  if(EXISTS "${_path}")
    message(FATAL_ERROR "Test-only artifact was installed: ${_path}")
  endif()
endforeach()

file(GLOB_RECURSE _test_libraries
  "${SITOS_PREFIX}/${SITOS_INSTALL_LIBDIR}/*gtest*"
  "${SITOS_PREFIX}/${SITOS_INSTALL_LIBDIR}/*gmock*")
if(_test_libraries)
  list(JOIN _test_libraries "\n  " _test_library_list)
  message(FATAL_ERROR "Test-only libraries were installed:\n  ${_test_library_list}")
endif()
