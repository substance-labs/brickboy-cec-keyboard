cmake_minimum_required(VERSION 3.16)

project(brickboy-cec-keyboard VERSION 0.1.0 LANGUAGES CXX)

option(BRICKBOY_CEC_WITH_LIBCEC "Build with libCEC adapter support" ON)
option(BRICKBOY_CEC_STATIC_LIBCEC "Prefer static libCEC linking" ON)

set(BRICKBOY_CEC_PREFIX "" CACHE PATH "Prefix containing the custom RPi-enabled libCEC build")
set(BRICKBOY_RPI_LIB_DIR "" CACHE PATH "Directory containing Raspberry Pi firmware libraries")

add_executable(brickboy-cec-keyboard src/main.cpp)
target_compile_features(brickboy-cec-keyboard PRIVATE cxx_std_17)
target_compile_options(brickboy-cec-keyboard PRIVATE -Wall -Wextra -Wpedantic)

if(BRICKBOY_CEC_WITH_LIBCEC)
  target_compile_definitions(brickboy-cec-keyboard PRIVATE BRICKBOY_CEC_WITH_LIBCEC=1)

  if(BRICKBOY_CEC_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${BRICKBOY_CEC_PREFIX}")
  endif()

  find_path(LIBCEC_INCLUDE_DIR
    NAMES cec.h
    PATHS "${BRICKBOY_CEC_PREFIX}/include"
    PATH_SUFFIXES libcec
  )

  if(BRICKBOY_CEC_STATIC_LIBCEC)
    find_library(LIBCEC_LIBRARY
      NAMES libcec.a cec
      PATHS "${BRICKBOY_CEC_PREFIX}/lib"
    )
    find_library(P8_PLATFORM_LIBRARY
      NAMES libp8-platform.a p8-platform
      PATHS "${BRICKBOY_CEC_PREFIX}/lib"
    )
  else()
    find_library(LIBCEC_LIBRARY
      NAMES cec
      PATHS "${BRICKBOY_CEC_PREFIX}/lib"
    )
  endif()

  if(NOT LIBCEC_INCLUDE_DIR OR NOT LIBCEC_LIBRARY)
    message(FATAL_ERROR "libCEC headers/library not found. Set BRICKBOY_CEC_PREFIX.")
  endif()

  target_include_directories(brickboy-cec-keyboard PRIVATE "${LIBCEC_INCLUDE_DIR}")
  target_link_libraries(brickboy-cec-keyboard PRIVATE "${LIBCEC_LIBRARY}")

  if(P8_PLATFORM_LIBRARY)
    target_link_libraries(brickboy-cec-keyboard PRIVATE "${P8_PLATFORM_LIBRARY}")
  elseif(BRICKBOY_CEC_STATIC_LIBCEC)
    message(FATAL_ERROR "Static libCEC requires libp8-platform. Set BRICKBOY_CEC_PREFIX.")
  endif()

  if(BRICKBOY_RPI_LIB_DIR)
    find_library(RPI_BCM_HOST_LIBRARY NAMES bcm_host PATHS "${BRICKBOY_RPI_LIB_DIR}" NO_DEFAULT_PATH)
    find_library(RPI_VCHIQ_ARM_LIBRARY NAMES vchiq_arm PATHS "${BRICKBOY_RPI_LIB_DIR}" NO_DEFAULT_PATH)
    find_library(RPI_VCOS_LIBRARY NAMES vcos PATHS "${BRICKBOY_RPI_LIB_DIR}" NO_DEFAULT_PATH)
    if(NOT RPI_BCM_HOST_LIBRARY OR NOT RPI_VCHIQ_ARM_LIBRARY OR NOT RPI_VCOS_LIBRARY)
      message(FATAL_ERROR "Raspberry Pi firmware libraries not found in BRICKBOY_RPI_LIB_DIR.")
    endif()
  else()
    message(FATAL_ERROR "Set BRICKBOY_RPI_LIB_DIR for the Raspberry Pi CEC backend.")
  endif()

  find_package(Threads REQUIRED)
  target_link_libraries(brickboy-cec-keyboard PRIVATE Threads::Threads dl)

  find_library(UDEV_LIBRARY udev)
  if(NOT UDEV_LIBRARY)
    message(FATAL_ERROR "libudev development library not found.")
  endif()
  target_link_libraries(brickboy-cec-keyboard PRIVATE "${UDEV_LIBRARY}")

  find_library(X11_LIBRARY X11)
  find_library(XRANDR_LIBRARY Xrandr)
  if(NOT X11_LIBRARY OR NOT XRANDR_LIBRARY)
    message(FATAL_ERROR "X11 and Xrandr development libraries are required by this libCEC build.")
  endif()
  target_link_libraries(brickboy-cec-keyboard PRIVATE "${X11_LIBRARY}" "${XRANDR_LIBRARY}")

  target_link_libraries(brickboy-cec-keyboard PRIVATE
    "${RPI_BCM_HOST_LIBRARY}"
    "${RPI_VCHIQ_ARM_LIBRARY}"
    "${RPI_VCOS_LIBRARY}"
  )
endif()

install(TARGETS brickboy-cec-keyboard RUNTIME DESTINATION bin)
