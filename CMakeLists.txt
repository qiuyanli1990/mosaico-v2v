cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "v9")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-mosaico-bsp/components/esp-mosaico-bsp"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-webrtc-solution/components/esp_peer"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-webrtc-solution/components/esp_webrtc"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-webrtc-solution/components/av_render"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-webrtc-solution/components/media_lib_utils"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-webrtc-solution/components/webrtc_utils")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(mosaico_translator)

