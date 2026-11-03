idf_component_register(
    SRCS "app_main.c" "app_state.c" "network.c" "session.c" "v2v_client.c"
         "aec_processor.c" "board_audio.c" "pcm_timeline.c"
         "translation_protocol.c" "media.c" "usb_audio.c" "usb_uac_trace.c" "pcm_ring.c"
         "usb_console.c" "crash_trace.c" "display_transport.c"
         "subtitles.c" "ui_device.c"
         "usb_capture.c" "ui.c" "agora_auth.c" "agora_signaling.c" "webrtc.c"
    INCLUDE_DIRS "."
    REQUIRES esp-mosaico-bsp esp_webrtc esp_peer av_render media_lib_utils
             esp_capture esp_audio_codec esp_codec_dev usb_device_uac
             esp_event esp_wifi esp_netif esp_http_client esp_timer esp_app_format
             nvs_flash espressif__cjson espressif__esp-sr mbedtls lwip console
             espressif__tinyusb)

# Retain the panic frame before UART printing; USB CDC cannot run in panic.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
# Override the pinned BSP's internal-RAM LCD bounce-buffer policy.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_lcd_new_panel_io_spi"
    "-Wl,--undefined=__wrap_esp_lcd_new_panel_io_spi")

# Preserve the UAC component's internal stream reset before our lifecycle hooks.
idf_component_get_property(uac_lib espressif__usb_device_uac COMPONENT_LIB)
target_compile_definitions(${uac_lib} PRIVATE
    tud_mount_cb=uac_device_mount_cb
    tud_umount_cb=uac_device_umount_cb
    tud_suspend_cb=uac_device_suspend_cb
    tud_resume_cb=uac_device_resume_cb
    tud_audio_feedback_params_cb=uac_device_feedback_params_cb)

idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(uac_component_dir espressif__usb_device_uac COMPONENT_DIR)
target_include_directories(${tusb_lib} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${uac_component_dir}/tusb"
    "${uac_component_dir}/tusb_uac")
set(usb_speed_definitions)
if(CONFIG_MOSAICO_USB_HIGH_SPEED)
    list(APPEND usb_speed_definitions CONFIG_TINYUSB_RHPORT_HS=1)
endif()
target_compile_definitions(${tusb_lib} PUBLIC
    CFG_TUSB_CONFIG_FILE="mosaico_tusb_config.h"
    CFG_TUD_CDC=1
    CFG_TUD_CDC_RX_BUFSIZE=256
    CFG_TUD_CDC_TX_BUFSIZE=1024
    CFG_TUD_CDC_EP_BUFSIZE=512
    ${usb_speed_definitions})
target_compile_definitions(${uac_lib} PRIVATE ${usb_speed_definitions})
target_sources(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/usb_descriptors.c")

# Build a checked overlay of the pinned driver; keep managed sources intact.
idf_component_get_property(tusb_component_dir espressif__tinyusb COMPONENT_DIR)
set(tusb_audio_source "${tusb_component_dir}/src/class/audio/audio_device.c")
set(tusb_audio_overlay "${CMAKE_CURRENT_BINARY_DIR}/usb_audio_driver/audio_device.c")
set(tusb_audio_prepare "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare_tinyusb_audio.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${tusb_audio_source}" "${tusb_audio_prepare}")
idf_build_get_property(idf_python PYTHON)
execute_process(COMMAND "${idf_python}" "${tusb_audio_prepare}"
    "${tusb_audio_source}" "${tusb_audio_overlay}"
    RESULT_VARIABLE tusb_audio_result)
if(NOT tusb_audio_result EQUAL 0)
    message(FATAL_ERROR "Unable to prepare the TinyUSB audio driver")
endif()
get_target_property(tusb_sources ${tusb_lib} SOURCES)
list(FILTER tusb_sources EXCLUDE REGEX "(^|/)class/audio/audio_device\\.c$")
set_property(TARGET ${tusb_lib} PROPERTY SOURCES "${tusb_sources}")
target_sources(${tusb_lib} PRIVATE "${tusb_audio_overlay}")
target_include_directories(${tusb_lib} PRIVATE "${tusb_component_dir}/src/class/audio")

