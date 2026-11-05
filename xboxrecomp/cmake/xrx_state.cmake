# Save states for a game built on the xboxrecomp toolkit. Include after xrdbg.cmake and after the toolkit's
# add_subdirectory, then:
#
#   xrdbg_enable(mygame SAVE_STATES)
#   xrx_state_enable(mygame)
#
# Debug builds only. Needs XBOXRECOMP_DIR, MSVC x64, and the game's src/ on the include path for recomp/gen/recomp_types.h.
# Generates private instrumented copies of six toolkit sources in the build directory; the toolkit checkout is not edited
# and a normal build is not affected. Hooks are exact matches, so a toolkit they do not fit stops configuration.
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "xboxrecomp save states need native continuations: MSVC x64 only")
endif()
enable_language(ASM_MASM)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(_XRX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(_xrx_replace_source target original generated runtime_dir)
    get_target_property(_sources ${target} SOURCES)
    set(_replacement)
    set(_found FALSE)
    foreach(_entry IN LISTS _sources)
        if(_entry MATCHES "(^|/)${original}\\.c$")
            set(_found TRUE)
            list(APPEND _replacement "${runtime_dir}/${generated}.c")
        else()
            list(APPEND _replacement "${_entry}")
        endif()
    endforeach()
    if(NOT _found)
        message(FATAL_ERROR "Cannot find runtime source ${original}.c in ${target}")
    endif()
    set_property(TARGET ${target} PROPERTY SOURCES "${_replacement}")
    target_include_directories(${target} PRIVATE "${_XRX_ROOT}/include")
endfunction()

function(xrx_state_enable target)
    if(NOT XBOXRECOMP_DIR)
        message(FATAL_ERROR "xrx_state_enable needs XBOXRECOMP_DIR")
    endif()
    set(_runtime "${CMAKE_CURRENT_BINARY_DIR}/state_runtime")
    file(GLOB _appends "${_XRX_ROOT}/append/*.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_XRX_ROOT}/generate_runtime.py" ${_appends}
        "${XBOXRECOMP_DIR}/src/kernel/kernel_bridge.c" "${XBOXRECOMP_DIR}/src/kernel/kernel_sync.c"
        "${XBOXRECOMP_DIR}/src/kernel/kernel_rtl.c" "${XBOXRECOMP_DIR}/src/kernel/xbox_memory_layout.c"
        "${XBOXRECOMP_DIR}/src/apu/apu_core.c" "${XBOXRECOMP_DIR}/src/usb/ohci.c")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${_XRX_ROOT}/generate_runtime.py" "${XBOXRECOMP_DIR}" "${_runtime}"
        RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "xboxrecomp save-state hooks do not match this toolkit revision (see xboxrecomp/README.md)")
    endif()
    _xrx_replace_source(xbox_kernel kernel_bridge kernel_bridge_state "${_runtime}")
    _xrx_replace_source(xbox_kernel kernel_sync kernel_sync_state "${_runtime}")
    _xrx_replace_source(xbox_kernel kernel_rtl kernel_rtl_state "${_runtime}")
    _xrx_replace_source(xbox_kernel xbox_memory_layout xbox_memory_layout_state "${_runtime}")
    _xrx_replace_source(xbox_apu apu_core apu_core_state "${_runtime}")
    _xrx_replace_source(xbox_usb ohci ohci_state "${_runtime}")
    target_sources(${target} PRIVATE "${_XRX_ROOT}/src/xrx_state.c" "${_XRX_ROOT}/src/xrx_state_runtime.c")
    target_include_directories(${target} PRIVATE "${_XRX_ROOT}/include")
    xrdbg_native_enable(${target})
endfunction()
