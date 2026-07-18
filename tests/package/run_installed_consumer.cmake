if(NOT DEFINED SITOS_PREFIX)
  message(FATAL_ERROR "SITOS_PREFIX must name an installed sitos prefix")
endif()
if(NOT DEFINED SITOS_SOURCE_DIR)
  message(FATAL_ERROR "SITOS_SOURCE_DIR must name the sitos source tree")
endif()
if(NOT DEFINED CONSUMER_BUILD_DIR)
  message(FATAL_ERROR "CONSUMER_BUILD_DIR must name the consumer build directory")
endif()

if(NOT DEFINED CMAKE_GENERATOR)
  set(CMAKE_GENERATOR Ninja)
endif()

set(_consumer_source_dir "${SITOS_SOURCE_DIR}/tests/package/consumer")
set(_configure_command
  "${CMAKE_COMMAND}"
  -S "${_consumer_source_dir}"
  -B "${CONSUMER_BUILD_DIR}"
  -G "${CMAKE_GENERATOR}"
  -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_PREFIX_PATH=${SITOS_PREFIX}")
if(DEFINED ZENOHC_ROOT)
  list(APPEND _configure_command "-Dzenohc_ROOT=${ZENOHC_ROOT}")
endif()

execute_process(
  COMMAND ${_configure_command}
  RESULT_VARIABLE _configure_result)
if(_configure_result)
  message(FATAL_ERROR "Installed consumer configuration failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD_DIR}"
  RESULT_VARIABLE _build_result)
if(_build_result)
  message(FATAL_ERROR "Installed consumer build failed")
endif()
