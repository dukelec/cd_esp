set(srcs
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
    "usr/common_services.c"
    "usr/config.c"
)

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "." "usr" "utils")
