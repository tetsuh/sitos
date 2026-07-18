include(FindPackageHandleStandardArgs)

# Standalone zenoh-c releases do not expose portable installed version metadata. The supported
# range is therefore enforced by the dependency-upgrade workflow and remains the consumer's
# responsibility when selecting an externally provisioned root.
if(TARGET zenohc::zenohc)
  set(zenohc_FOUND TRUE)
  return()
endif()

set(_zenohc_root_hint)
if(zenohc_ROOT)
  set(_zenohc_root_hint "${zenohc_ROOT}")
elseif(DEFINED ENV{ZENOHC_ROOT})
  set(_zenohc_root_hint "$ENV{ZENOHC_ROOT}")
endif()

set(_zenohc_find_options)
if(_zenohc_root_hint)
  list(APPEND _zenohc_find_options
    HINTS "${_zenohc_root_hint}"
    NO_DEFAULT_PATH)
endif()

find_path(ZENOHC_INCLUDE_DIR
  NAMES zenoh.h
  ${_zenohc_find_options}
  PATH_SUFFIXES include)

if(WIN32)
  find_file(ZENOHC_IMPLIB
    NAMES zenohc.dll.lib
    ${_zenohc_find_options}
    PATH_SUFFIXES lib)
  find_file(ZENOHC_RUNTIME
    NAMES zenohc.dll
    ${_zenohc_find_options}
    PATH_SUFFIXES bin)
  find_package_handle_standard_args(zenohc
    REQUIRED_VARS ZENOHC_INCLUDE_DIR ZENOHC_IMPLIB ZENOHC_RUNTIME)
else()
  find_library(ZENOHC_LIBRARY
    NAMES zenohc
    ${_zenohc_find_options}
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
unset(_zenohc_find_options)
unset(_zenohc_root_hint)
