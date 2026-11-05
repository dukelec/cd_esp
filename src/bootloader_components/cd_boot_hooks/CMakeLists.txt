
idf_component_register(
    SRCS
        "hooks.c"
        "config.c"
        "comm_service.c"
        "cdctl_spi_wr.c"
        "cdnet/utils/modbus_crc.c"
        "cdnet/utils/cd_list.c"
        "cdnet/utils/hex_dump.c"
        "cdnet/dev/cdctl.c"
        "cdnet/dev/cdctl_pll_cal.c"

    INCLUDE_DIRS
        "."
        "cdnet/dev"
        "cdnet/utils"
        "cdnet/arch/esp32_riscv"

    PRIV_REQUIRES
        hal
        soc
        log
        esp_rom
        esp_hw_support
        esp_hal_gpio
        esp_hal_clock
        esp_hal_wdt
)

# "-Wextra" enables -Woverride-init, allow "default + override" style initializers (e.g. usr/config.c)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-override-init)

# ld relaxation may turn a call from the end of iram_seg into iram_loader_seg into c.j, which goes out of range
# once the code ahead shrinks ("relocation truncated to fit: R_RISCV_RVC_JUMP"): keep such callers unrelaxed
idf_component_get_property(bs_lib bootloader_support COMPONENT_LIB)
idf_component_get_property(bs_dir bootloader_support COMPONENT_DIR)
set_source_files_properties(${bs_dir}/src/bootloader_console.c TARGET_DIRECTORY ${bs_lib}
                            PROPERTIES COMPILE_OPTIONS -mno-relax)
