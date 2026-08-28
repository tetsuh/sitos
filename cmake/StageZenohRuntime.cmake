# Stage one zenoh-c runtime per binary directory before dependent executables
# link. This avoids concurrent POST_BUILD copies racing with vcpkg's applocal
# processing when many Windows targets share one output directory.
function(sitos_copy_zenohc target)
  if(NOT WIN32)
    return()
  endif()
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Unknown target passed to sitos_copy_zenohc: ${target}")
  endif()
  if(NOT DEFINED SITOS_ZENOHC_RUNTIME OR SITOS_ZENOHC_RUNTIME STREQUAL "")
    message(FATAL_ERROR "SITOS_ZENOHC_RUNTIME must name the zenoh-c runtime")
  endif()

  get_property(_stage_target DIRECTORY PROPERTY SITOS_ZENOHC_STAGE_TARGET)
  if(NOT _stage_target)
    string(SHA256 _directory_hash "${CMAKE_CURRENT_BINARY_DIR}")
    string(SUBSTRING "${_directory_hash}" 0 12 _directory_id)
    set(_stage_target "sitos_stage_zenohc_${_directory_id}")
    if(CMAKE_CONFIGURATION_TYPES)
      set(_runtime_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
    else()
      set(_runtime_directory "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    add_custom_target(${_stage_target}
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_runtime_directory}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SITOS_ZENOHC_RUNTIME}"
        "${_runtime_directory}/zenohc.dll"
      COMMENT "Stage shared zenoh-c runtime"
      VERBATIM)
    set_property(DIRECTORY PROPERTY SITOS_ZENOHC_STAGE_TARGET ${_stage_target})
  endif()
  add_dependencies(${target} ${_stage_target})
endfunction()
