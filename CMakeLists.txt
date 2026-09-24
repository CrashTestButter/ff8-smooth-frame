cmake_minimum_required(VERSION 3.21)

# ff8interp.dll - FF8 frame interpolation as a standalone DLL next to a stock FFNx (AF3DN.P).
# The interpolation module lives in interp/ (source of truth, see interp/SOURCE.md); src/ holds the
# standalone host adapter, DLL entry, config and log.

project(ff8interp VERSION 0.2.0 LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ff8interp must be built for Win32 (x86): cmake -A Win32")
endif()

set(INTERP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/interp" CACHE PATH "interpolation module sources")
if(NOT EXISTS "${INTERP_DIR}/interp.h")
  message(FATAL_ERROR "interp module not found at ${INTERP_DIR}")
endif()

file(GLOB_RECURSE INTERP_SOURCES CONFIGURE_DEPENDS "${INTERP_DIR}/*.cpp")
list(FILTER INTERP_SOURCES EXCLUDE REGEX ".*/adapter_ffnx\\.cpp$")
file(GLOB_RECURSE INTERP_HEADERS CONFIGURE_DEPENDS "${INTERP_DIR}/*.h")

set(SA_SOURCES
  src/dllmain.cpp
  src/adapter_standalone.cpp
  src/config.cpp
  src/log.cpp
  src/standalone.h
  src/version.rc
)

# Static CRT: the DLL must not depend on a VC++ redistributable inside Proton / the game folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(ff8interp SHARED ${SA_SOURCES} ${INTERP_SOURCES} ${INTERP_HEADERS})
source_group("interp" FILES ${INTERP_SOURCES} ${INTERP_HEADERS})

target_compile_features(ff8interp PRIVATE cxx_std_20)
target_compile_definitions(ff8interp PRIVATE
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
)
target_compile_options(ff8interp PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:/W3;/Zc:__cplusplus;/Zc:preprocessor;/Zc:strictStrings-;/MP;/utf-8>"
)
target_include_directories(ff8interp PRIVATE "${INTERP_DIR}")
target_link_libraries(ff8interp PRIVATE version)
# No ASLR/relocation concerns for our own code; keep a PDB next to the DLL for crash dumps.
target_link_options(ff8interp PRIVATE /PDBALTPATH:ff8interp.pdb)
set_target_properties(ff8interp PROPERTIES
  OUTPUT_NAME ff8interp
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
