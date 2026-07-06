
idf_component_register(
    SRCS
        "main.c"
        "cd_ble.c"
        "gatt_svr.c"
        "wifi_sta.c"
        "mdns_minimal.c"
        "cdnet/arch/esp32_riscv/arch_wrapper.c"
        "cdnet/utils/cd_list.c"
        "cdnet/utils/modbus_crc.c"
        "cdnet/dev/cdctl_pll_cal.c"
        "cdnet/dev/cdctl_it.c"
        "usr/cd_main.c"
        "usr/comm_service.c"
        "usr/config.c"

    INCLUDE_DIRS
        "."
        "usr"
        "cdnet/arch/esp32_riscv"
        "cdnet/utils"
        "cdnet/dev"

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
