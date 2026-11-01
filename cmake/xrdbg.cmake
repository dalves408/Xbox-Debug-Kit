# xrdbg CMake helper. Include this file from the game's CMakeLists.txt, then:
#
#   option(MYGAME_DEBUGKIT "Debug build with the xrdbg self-monitoring kit" OFF)
#   if(MYGAME_DEBUGKIT)
#       include(path/to/xrdbg/cmake/xrdbg.cmake)
#       xrdbg_enable(mygame_exe)
#   endif()
#
# It adds src/xrdbg.c to the target, defines XRDBG (turns the XRDBG_* macros in xrdbg.h on), puts include/ on the include
# path, links what the kit needs, and makes the build produce a PDB (dumps can only be read with the exact exe + PDB pair:
# keep both for every build that goes to a player).
#
# Optional: a buffered log for games that print a lot from hot threads (see CUSTOMIZING.md, "A game that logs a lot"):
#   xrdbg_enable(mygame_exe STDERR_BUFFER 65536)
get_filename_component(_XRDBG_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(xrdbg_enable target)
    cmake_parse_arguments(_XRDBG "" "STDERR_BUFFER" "" ${ARGN})
    target_sources(${target} PRIVATE "${_XRDBG_ROOT}/src/xrdbg.c")
    target_include_directories(${target} PRIVATE "${_XRDBG_ROOT}/include")
    target_compile_definitions(${target} PRIVATE XRDBG=1)
    if(_XRDBG_STDERR_BUFFER)
        target_compile_definitions(${target} PRIVATE XRDBG_STDERR_BUFFER=${_XRDBG_STDERR_BUFFER})
    endif()
    target_link_libraries(${target} PRIVATE dbghelp psapi shell32 user32 gdi32 advapi32)
    if(MSVC)
        target_compile_options(${target} PRIVATE /Zi)
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()
endfunction()
