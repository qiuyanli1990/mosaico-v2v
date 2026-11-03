cmake_minimum_required(VERSION 3.20)
project(mosaico_ui_test C)
set(CMAKE_C_STANDARD 11)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/managed_components/lvgl__lvgl" lvgl)
add_executable(ui_test test_ui.c "${ROOT}/main/translation_protocol.c"
    "${ROOT}/managed_components/espressif__cjson/cJSON/cJSON.c")
target_include_directories(ui_test PRIVATE mocks "${ROOT}/main"
    "${ROOT}/managed_components/espressif__cjson/cJSON")
target_compile_options(ui_test PRIVATE -Wall -Wextra -Werror -Wno-deprecated-declarations
    -fsanitize=address,undefined -g)
target_link_options(ui_test PRIVATE -fsanitize=address,undefined)
target_link_libraries(ui_test PRIVATE lvgl)
