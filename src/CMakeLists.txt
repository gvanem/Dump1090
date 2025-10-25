#
# CMake build system contributed by Tyler Kinney.
#
# Ref: https://github.com/gvanem/Dump1090/pull/35
#
cmake_minimum_required(VERSION 3.20)
project(dump1090 LANGUAGES C)

#
# Windows-only check
#
if(NOT WIN32)
    message(FATAL_ERROR "dump1090 is Windows-only")
endif()

#
# Define source files
#
set(SOURCES
    aircraft.c
    airports.c
    cfg_file.c
    convert.c
    cpr.c
    crc.c
    csv.c
    demod-2000.c
    demod-2400.c
    demod-2400AC.c
    demod-8000.c
    dump1090.c
    fifo.c
    geo.c
    infile.c
    interactive.c
    location.c
    misc.c
    net_io.c
    pconsole.c
    smartlist.c
    speech.c
    externals/mongoose.c
    externals/AirSpy/airspy.c
    externals/PDC-Mod/amalgamation.c
    externals/sqlite3.c
    externals/zip.c
    externals/rtl-sdr/librtlsdr.c
    externals/rtl-sdr/trace.c
    externals/rtl-sdr/tuner_e4k.c
    externals/rtl-sdr/tuner_fc001x.c
    externals/rtl-sdr/tuner_fc2580.c
    externals/rtl-sdr/tuner_r82xx.c
    externals/SDRplay/sdrplay.c
)

#
# Include directories
#
include_directories (. ./externals ./externals/PDC-Mod)

#
# Preprocessor define; since we have no custom CMake rule to create
# a 'cflags_$(CC).h' and 'ldflags_$(CC).h' yet.
#
add_definitions (-DDOING_MSBUILD)

#
# Forced include header:
#
if (MSVC)
  message ("Building for MSVC")
  add_compile_options (/FIdump1090_config.h)
else()
  #
  # Assume MinGW / gcc
  #
  message ("Building for MinGW")
  add_compile_options (--include dump1090_config.h)
endif()

#
# Create executable
#
add_executable (dump1090 ${SOURCES})

#
# Link Windows libraries
#
target_link_libraries(dump1090 PRIVATE
    advapi32
    dnsapi
    iphlpapi
    ole32
    user32
    setupapi
    shlwapi
    winmm
    winusb
    ws2_32
)

#
# Compiler options per configuration
#
if (MSVC)
  target_compile_options (dump1090 PRIVATE
                          $<$<CONFIG:Debug>:/MDd /W4 /Zi /Od>
                          $<$<CONFIG:Release>:/MD /W4 /Zi>)

  #
  # Console subsystem
  #
  set_target_properties (dump1090 PROPERTIES
      LINK_FLAGS "/SUBSYSTEM:CONSOLE"
      OUTPUT_NAME "dump1090"
  )

  #
  # Set machine type based on architecture
  #
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set_target_properties(dump1090 PROPERTIES LINK_FLAGS "${CMAKE_LINK_FLAGS} /MACHINE:X64")
  else()
      set_target_properties(dump1090 PROPERTIES LINK_FLAGS "${CMAKE_LINK_FLAGS} /MACHINE:X86")
  endif()

  #
  # Enable PDB generation
  #
  set_target_properties (dump1090 PROPERTIES
      COMPILE_PDB_NAME "dump1090"
      COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  )

else()
  set_target_properties (dump1090 PROPERTIES OUTPUT_NAME "dump1090")
endif()

#
# How the hell is this supposed to work?
#
if (MSVC)
  set (_cflags_h  ${CMAKE_CURRENT_BINARY_DIR}/cflags_cl.h)
  set (_ldflags_h ${CMAKE_CURRENT_BINARY_DIR}/ldflags_cl.h)

  add_custom_command (OUTPUT ${_cflags_h}
                      COMMAND echo const char *cflags = "${CMAKE_C_COMPILER} ${C_FLAGS} ${C_INCLUDES}" > ${_cflags_h}
                      VERBATIM USES_TERMINAL)

  add_custom_command (OUTPUT ${_ldflags_h}
                      COMMAND echo const char *ldflags = "${CMAKE_LINKER} ${CMAKE_EXE_LINKER_FLAGS}" > ${_ldflags_h}
                      VERBATIM USES_TERMINAL)

  add_custom_target (gen_files DEPENDS ${_cflags_h} ${_ldflags_h} VERBATIM)

  add_dependencies (dump1090 gen_files)
  add_definitions (-DDOING_CMAKE_BUILD -I${CMAKE_CURRENT_BINARY_DIR})
endif()

#
# Post-build: copy executable to ../dump1090.exe.
# Fails with "-G Ninja" !?
#
add_custom_command(TARGET dump1090 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:dump1090>
        ${CMAKE_CURRENT_SOURCE_DIR}/../dump1090.exe
    COMMENT "Copying dump1090.exe to ../dump1090.exe"
)
