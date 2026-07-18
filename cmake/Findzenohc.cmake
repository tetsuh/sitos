include(FindPackageHandleStandardArgs)

if(TARGET zenohc::zenohc)
  set(zenohc_FOUND TRUE)
  return()
endif()

set(_zenohc_root_hints)
if(zenohc_ROOT)
  list(APPEND _zenohc_root_hints "${zenohc_ROOT}")
endif()
if(DEFINED ENV{ZENOHC_ROOT})
  list(APPEND _zenohc_root_hints "$ENV{ZENOHC_ROOT}")
endif()

find_path(ZENOHC_INCLUDE_DIR
  NAMES zenoh.h
  HINTS ${_zenohc_root_hints}
  PATH_SUFFIXES include)

if(WIN32)
  find_file(ZENOHC_IMPLIB
    NAMES zenohc.dll.lib
    HINTS ${_zenohc_root_hints}
    PATH_SUFFIXES lib)
  find_file(ZENOHC_RUNTIME
    NAMES zenohc.dll
    HINTS ${_zenohc_root_hints}
    PATH_SUFFIXES bin)
  find_package_handle_standard_args(zenohc
    REQUIRED_VARS ZENOHC_INCLUDE_DIR ZENOHC_IMPLIB ZENOHC_RUNTIME)
else()
  find_library(ZENOHC_LIBRARY
    NAMES zenohc
    HINTS ${_zenohc_root_hints}
    PATH_SUFFIXES lib)
  find_package_handle_standard_args(zenohc
    REQUIRED_VARS ZENOHC_INCLUDE_DIR ZENOHC_LIBRARY)
endif()

if(zenohc_FOUND AND NOT TARGET zenohc::zenohc)
  add_library(zenohc::zenohc SHARED IMPORTED)
  set_target_properties(zenohc::zenohc PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ZENOHC_INCLUDE_DIR}")
  if(WIN32)
    set_target_properties(zenohc::zenohc PROPERTIES
      IMPORTED_IMPLIB "${ZENOHC_IMPLIB}"
      IMPORTED_LOCATION "${ZENOHC_RUNTIME}")
    set_property(TARGET zenohc::zenohc APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES ws2_32 iphlpapi userenv bcrypt)
  else()
    set_target_properties(zenohc::zenohc PROPERTIES
      IMPORTED_LOCATION "${ZENOHC_LIBRARY}")
  endif()
endif()

mark_as_advanced(
  ZENOHC_INCLUDE_DIR
  ZENOHC_IMPLIB
  ZENOHC_LIBRARY
  ZENOHC_RUNTIME)
unset(_zenohc_root_hints)
