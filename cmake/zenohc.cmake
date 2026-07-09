# cmake/zenohc.cmake — Download prebuilt zenoh-c standalone package and create
# an imported target.
#
# Pinned zenoh-c version. Update this when upgrading the locked default.
set(ZENOHC_VERSION "1.9.0")

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

include(FetchContent)
# FetchContent_Populate is used because zenoh-c standalone does not provide a
# CMakeLists.txt; we only need the extracted archive.
cmake_policy(SET CMP0169 OLD)
FetchContent_Declare(
  zenohc
  URL ${ZENOHC_URL}
  SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/zenohc-src
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(zenohc)
if(NOT zenohc_POPULATED)
  FetchContent_Populate(zenohc)
endif()

# Paths resolved after Populate (zenohc_SOURCE_DIR is set by Populate).
set(ZENOHC_INCLUDE_DIR "${zenohc_SOURCE_DIR}/include")
set(ZENOHC_LIB_DIR "${zenohc_SOURCE_DIR}/lib")

# Create imported library target.
if(WIN32)
  add_library(zenohc::zenohc SHARED IMPORTED)
  set_target_properties(zenohc::zenohc PROPERTIES
    IMPORTED_LOCATION "${zenohc_SOURCE_DIR}/bin/zenohc.dll"
    IMPORTED_IMPLIB   "${ZENOHC_LIB_DIR}/zenohc.dll.lib"
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
        "${zenohc_SOURCE_DIR}/bin/zenohc.dll"
        "$<TARGET_FILE_DIR:${target}>"
    )
  endif()
endfunction()
