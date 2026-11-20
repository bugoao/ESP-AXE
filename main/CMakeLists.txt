idf_component_register(
SRCS
    "adc.c"
    "i2c_bitaxe.c"
    "main.c"
    "nvs_config.c"
    "display.c"
    "screen.c"
    "socks5_client.c"
    "input.c"
    "system.c"
    "work_queue.c"
    "nvs_device.c"
    "lv_font_portfolio-6x8.c"
    "logo.c"
    "./http_server/http_server.c"
    "./http_server/http_update.c"
    "./http_server/http_json.c"
    "./http_server/theme_api.c"
    "./self_test/self_test.c"
    "./tasks/stratum_task.c"
    "./tasks/create_jobs_task.c"
    "./tasks/asic_task.c"
    "./tasks/asic_result_task.c"
    "./tasks/power_management_task.c"
    "./thermal/EMC2101.c"
    "./thermal/EMC2103.c"
    "./thermal/EMC2302.c"
    "./thermal/TMP1075.c"
    "./thermal/thermal.c"
    "./power/TPS546.c"
    "./power/DS4432U.c"
    "./power/INA260.c"
    "./power/power.c"
    "./power/vcore.c"

INCLUDE_DIRS
    "."
    "tasks"
    "http_server"
    "self_test"
    "../components/asic/include"
    "../components/connect/include"
    "../components/dns_server/include"
    "../components/stratum/include"
    "thermal"
    "power"

PRIV_REQUIRES
    "app_update"
    "driver"
    "esp_adc"
    "esp_app_format"
    "esp_event"
    "esp_http_server"
    "esp_netif"
    "esp_psram"
    "esp_rom"
    "esp_timer"
    "esp_wifi"
    "json"
    "lwip"
    "mbedtls"
    "nvs_flash"
    "spiffs"
    "stratum_v2"
    "tcp_transport"
    "vfs"
    "esp_driver_i2c"

EMBED_FILES "http_server/recovery_page.html"
)

idf_build_set_property(COMPILE_OPTIONS "-DLV_CONF_INCLUDE_SIMPLE=1" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DLV_CONF_PATH= ${CMAKE_SOURCE_DIR}/main/lv_conf.h" APPEND)

set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/http_server/axe-os")

if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    message(STATUS "Running on GitHub Actions. Web ui will be prebuilt.")

    spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist/axe-os FLASH_IN_PROJECT)
else()
    find_program(NPM_EXECUTABLE NAMES npm.cmd npm)
    if(NOT NPM_EXECUTABLE)
        message(FATAL_ERROR "npm is not found! Please install it to proceed.")
    endif()

    file(GLOB_RECURSE WEB_UI_SOURCES CONFIGURE_DEPENDS "${WEB_SRC_DIR}/src/*")
    set(WEB_INSTALL_STAMP "${CMAKE_CURRENT_BINARY_DIR}/web-dependencies.stamp")
    add_custom_command(
        OUTPUT "${WEB_INSTALL_STAMP}"
        BYPRODUCTS "${WEB_SRC_DIR}/node_modules/.package-lock.json"
        COMMAND "${NPM_EXECUTABLE}" ci
        COMMAND "${CMAKE_COMMAND}" -E touch "${WEB_INSTALL_STAMP}"
        DEPENDS "${WEB_SRC_DIR}/package.json" "${WEB_SRC_DIR}/package-lock.json"
        WORKING_DIRECTORY "${WEB_SRC_DIR}"
        USES_TERMINAL
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WEB_SRC_DIR}/dist/axe-os/index.html.gz"
        COMMAND "${NPM_EXECUTABLE}" run build
        DEPENDS ${WEB_UI_SOURCES} "${WEB_INSTALL_STAMP}"
            "${WEB_SRC_DIR}/angular.json" "${WEB_SRC_DIR}/tsconfig.json"
            "${WEB_SRC_DIR}/tsconfig.app.json" "${WEB_SRC_DIR}/only-gzip.js"
        WORKING_DIRECTORY "${WEB_SRC_DIR}"
        USES_TERMINAL
        VERBATIM
    )
    add_custom_target(web_ui_dist DEPENDS "${WEB_SRC_DIR}/dist/axe-os/index.html.gz")

    add_dependencies(${COMPONENT_LIB} web_ui_dist)

    spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist/axe-os FLASH_IN_PROJECT DEPENDS web_ui_dist)
endif()
