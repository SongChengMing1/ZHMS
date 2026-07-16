cmake_minimum_required(VERSION 3.20.0)

if((NOT DEFINED DTC_OVERLAY_FILE OR DTC_OVERLAY_FILE STREQUAL "")
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32s3_devkitc.overlay")
  set(DTC_OVERLAY_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32s3_devkitc.overlay"
      CACHE STRING "Application devicetree overlay" FORCE)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_health_monitoring_system)

add_subdirectory(drivers)

file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS src/*.c)
target_include_directories(app PRIVATE
  src
  src/ui
  src/services
)
target_sources(app PRIVATE ${app_sources})
