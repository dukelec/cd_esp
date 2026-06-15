
idf_component_register(
    SRCS
        "main.c"
        "cd_ble.c"
        "gatt_svr.c"
        "wifi_sta.c"
        "mdns_minimal.c"
        "utils/cd_list.c"
        "utils/cdctl_pll_cal.c"
        "utils/cdctl_it.c"
        "utils/modbus_crc.c"
        "usr/cd_main.c"
        "usr/cdctl_spi_wr.c"
        "usr/comm_service.c"
        "usr/config.c"

    INCLUDE_DIRS
        "."
        "usr"
        "utils"

    REQUIRES
        bt
        mbedtls
        esp_event
        nvs_flash
        spi_flash
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_coex
)

if(DEFINED CD_DISABLE_UDP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CD_DISABLE_UDP=${CD_DISABLE_UDP})
endif()
