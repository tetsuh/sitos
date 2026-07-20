# cmake/zenohc.cmake — Download prebuilt zenoh-c standalone package and create
# an imported target.
#
# Pinned zenoh-c version. Update this when upgrading the locked default.
# Declared as a CACHE variable so the dependency-upgrade CI can override it
# with -DZENOHC_VERSION=<version> to test other releases.
set(ZENOHC_VERSION "1.9.0" CACHE STRING "Pinned zenoh-c standalone version")
set(SITOS_ZENOHC_ROOT "" CACHE PATH
    "Pre-staged zenoh-c standalone tree; empty downloads the pinned archive")

# Detect platform and set the archive name.
if(WIN32)
  set(ZENOHC_PLATFORM "x86_64-pc-windows-msvc")
elseif(APPLE)
  set(ZENOHC_PLATFORM "x86_64-apple-darwin")
else()
  set(ZENOHC_PLATFORM "x86_64-unknown-linux-gnu")
endif()
set(ZENOHC_ARCHIVE "zenoh-c-${ZENOHC_VERSION}-${ZENOHC_PLATFORM}-standalone.zip")
set(ZENOHC_URL "https://github.com/eclipse-zenoh/zenoh-c/releases/download/${ZENOHC_VERSION}/${ZENOHC_ARCHIVE}")

if(SITOS_ZENOHC_ROOT)
  # Wheel builders and externally provisioned consumers pass an explicit,
  # already validated standalone tree. Do not download in this mode.
  get_filename_component(ZENOHC_ROOT "${SITOS_ZENOHC_ROOT}" ABSOLUTE)
else()
  include(FetchContent)
  # zenoh-c standalone has no CMakeLists.txt; MakeAvailable populates the archive
  # and, on encountering no CMakeLists.txt at the source directory, does not call
  # add_subdirectory() — so no CMakeLists.txt is required.
  FetchContent_Declare(
    zenohc
    URL ${ZENOHC_URL}
    SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/zenohc-src
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(zenohc)
  set(ZENOHC_ROOT "${zenohc_SOURCE_DIR}")
endif()

set(ZENOHC_INCLUDE_DIR "${ZENOHC_ROOT}/include")
set(ZENOHC_LIB_DIR "${ZENOHC_ROOT}/lib")
if(EXISTS "${ZENOHC_ROOT}/LICENSE")
  set(SITOS_ZENOHC_LICENSE "${ZENOHC_ROOT}/LICENSE")
else()
  set(SITOS_ZENOHC_LICENSE "${CMAKE_SOURCE_DIR}/third_party/zenoh-c/LICENSE")
endif()
if(WIN32)
  set(SITOS_ZENOHC_RUNTIME "${ZENOHC_ROOT}/bin/zenohc.dll")
  set(SITOS_ZENOHC_IMPORT_LIBRARY "${ZENOHC_LIB_DIR}/zenohc.dll.lib")
  set(SITOS_ZENOHC_REQUIRED_FILES
      "${ZENOHC_INCLUDE_DIR}/zenoh.h"
      "${SITOS_ZENOHC_RUNTIME}"
      "${SITOS_ZENOHC_IMPORT_LIBRARY}")
else()
  set(SITOS_ZENOHC_RUNTIME "${ZENOHC_LIB_DIR}/libzenohc.so")
  set(SITOS_ZENOHC_REQUIRED_FILES
      "${ZENOHC_INCLUDE_DIR}/zenoh.h"
      "${SITOS_ZENOHC_RUNTIME}")
endif()
foreach(required_file IN LISTS SITOS_ZENOHC_REQUIRED_FILES)
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR
      "zenoh-c standalone tree is incomplete; missing ${required_file}. "
      "Set SITOS_ZENOHC_ROOT to a valid staged tree.")
  endif()
endforeach()

# Create imported library target.
if(WIN32)
  add_library(zenohc::zenohc SHARED IMPORTED)
  set_target_properties(zenohc::zenohc PROPERTIES
    IMPORTED_LOCATION "${SITOS_ZENOHC_RUNTIME}"
    IMPORTED_IMPLIB   "${SITOS_ZENOHC_IMPORT_LIBRARY}"
  )
  target_link_libraries(zenohc::zenohc INTERFACE ws2_32 iphlpapi userenv bcrypt)
else()
  add_library(zenohc::zenohc SHARED IMPORTED)
  set_target_properties(zenohc::zenohc PROPERTIES
    IMPORTED_LOCATION "${ZENOHC_LIB_DIR}/libzenohc.so"
  )
endif()

target_include_directories(zenohc::zenohc INTERFACE "${ZENOHC_INCLUDE_DIR}")

# Helper: copy the zenohc shared library alongside a test executable so it can
# be found at runtime on Windows. On Linux, LD_LIBRARY_PATH is handled via the
# ctest environment.
function(sitos_copy_zenohc target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SITOS_ZENOHC_RUNTIME}"
        "$<TARGET_FILE_DIR:${target}>"
    )
  endif()
endfunction()
