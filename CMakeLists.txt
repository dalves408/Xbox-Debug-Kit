cmake_minimum_required(VERSION 3.16)
project(xrdbg C)
# Builds only the demo/self-test. A game does not use this file: it includes cmake/xrdbg.cmake instead (see README.md).
include(cmake/xrdbg.cmake)
add_executable(xrdbg_demo WIN32 demo/xrdbg_demo.c)
xrdbg_enable(xrdbg_demo)
set_property(TARGET xrdbg_demo PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
