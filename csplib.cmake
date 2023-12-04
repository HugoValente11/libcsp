cmake_minimum_required(VERSION 3.20)
project(CSP_LIB C)

# Define the source and header files for the CSP library
set(CSP_SOURCES
    fsw/src/csp_buffer.c
    fsw/src/csp_conn.c
    fsw/src/csp_crc32.c
    fsw/src/csp_debug.c
    fsw/src/csp_if_can.c
    fsw/src/csp_if_ethernet.c
    fsw/src/csp_if_i2c.c
    fsw/src/csp_if_kiss.c
    fsw/src/csp_if_lo.c
    fsw/src/csp_if_zmqhub.c
    fsw/src/csp_io.c
    fsw/src/csp_port.c
    fsw/src/csp_promisc.c
    fsw/src/csp_qfifo.c
    fsw/src/csp_rdp.c
    fsw/src/csp_route.c
    fsw/src/csp_service_handler.c
    fsw/src/csp_services.c
    fsw/src/csp_sfp.c
    fsw/src/csp_threads.c
    # ... (add all other source files of the CSP library)
)

set(CSP_HEADERS
    fsw/public_inc/csp_buffer.h
    fsw/public_inc/csp_conn.h
    fsw/public_inc/csp_crc32.h
    fsw/public_inc/csp_debug.h
    fsw/public_inc/csp_if_can.h
    fsw/public_inc/csp_if_ethernet.h
    fsw/public_inc/csp_if_i2c.h
    fsw/public_inc/csp_if_kiss.h
    fsw/public_inc/csp_if_lo.h
    fsw/public_inc/csp_if_zmqhub.h
    fsw/public_inc/csp_io.h
    fsw/public_inc/csp_port.h
    fsw/public_inc/csp_promisc.h
    fsw/public_inc/csp_qfifo.h
    fsw/public_inc/csp_rdp.h
    fsw/public_inc/csp_route.h
    fsw/public_inc/csp_service_handler.h
    fsw/public_inc/csp_services.h
    fsw/public_inc/csp_sfp.h
    fsw/public_inc/csp_threads.h
    # ... (add all other header files of the CSP library)
)

# Create the CSP library as a cFE app
add_cfe_app(csp_lib ${CSP_SOURCES} ${CSP_HEADERS})

# Include directories for the CSP library
target_include_directories(csp_lib PUBLIC fsw/public_inc)

# Compiler options and definitions
set_target_properties(csp_lib PROPERTIES C_STANDARD 11)
set_target_properties(csp_lib PROPERTIES C_EXTENSIONS ON)
target_compile_options(csp_lib PRIVATE -Wall -Wextra)

# CSP specific configurations
option(CSP_HAVE_STDIO "OS provides C Standard I/O functions" ON)
option(CSP_ENABLE_CSP_PRINT "Enable csp_print() function" ON)
option(CSP_PRINT_STDIO "Use vprintf() for csp_print() function" ON)
option(CSP_USE_RDP "Reliable Datagram Protocol" ON)
option(CSP_USE_HMAC "Hash-based message authentication code" ON)
option(CSP_USE_PROMISC "Promiscious mode" ON)
option(CSP_USE_DEDUP "Packet deduplication" ON)
option(CSP_USE_RTABLE "Use routing table" OFF)
option(CSP_ENABLE_PYTHON3_BINDINGS "Build Python3 binding" OFF)

# Check for system-specific libraries and include files
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BUILD_SHARED_LIBS ON)
    include(CheckIncludeFiles)
    check_include_files(sys/socket.h HAVE_SYS_SOCKET_H)
    check_include_files(arpa/inet.h HAVE_ARPA_INET_H)
    set(CSP_POSIX 1)

    find_package(Threads REQUIRED)
    find_package(PkgConfig)
  
    if(PKG_CONFIG_FOUND)
      pkg_search_module(LIBZMQ libzmq)
      if(${LIBZMQ_FOUND})
        message(STATUS "Found ${LIBZMQ_LINK_LIBRARIES} ${LIBZMQ_VERSION}")
        set(CSP_HAVE_LIBZMQ 1)
      else()
        message(NOTICE "No libzmq found")
      endif()
  
      pkg_search_module(LIBSOCKETCAN libsocketcan)
      if(${LIBSOCKETCAN_FOUND})
        message(STATUS "Found ${LIBSOCKETCAN_LINK_LIBRARIES} ${LIBSOCKETCAN_VERSION}")
        set(CSP_HAVE_LIBSOCKETCAN 1)
      else()
        message(NOTICE "No libsocketcan found")
      endif()
    else()
      message(NOTICE "No pkg-config found")
    endif()
endif()

if(CSP_POSIX)
  set(CSP_C_ARGS -Wshadow -Wcast-align -Wwrite-strings -Wno-unused-parameter)
elseif(CSP_ZEPHYR)
  set(CSP_C_ARGS -Wwrite-strings -Wno-unused-parameter)
endif()

# Add subdirectories if needed (e.g., for additional CSP components)
add_subdirectory(src)
add_subdirectory(examples)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Zephyr")
  install(TARGETS csp LIBRARY COMPONENT runtime)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/;${CMAKE_CURRENT_SOURCE_DIR}/include/;
    TYPE INCLUDE
    FILES_MATCHING PATTERN "*.h*"
  )
endif()

# Unit tests (if applicable)
if (ENABLE_UNIT_TESTS)
    # Add unit test subdirectories here
endif (ENABLE_UNIT_TESTS)

# Installation (if applicable)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Zephyr")
    install(TARGETS csp LIBRARY COMPONENT runtime)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/;${CMAKE_CURRENT_SOURCE_DIR}/include/;
        TYPE INCLUDE
        FILES_MATCHING PATTERN "*.h*"
    )
endif()
