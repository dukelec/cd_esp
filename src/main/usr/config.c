/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <d@d-l.io>
 */

#include "cd_main.h"

#define ONCE_PAGE_SIZE  4096


const csa_t csa_dft = {
        .magic_code = 0xcdcd,
        .conf_ver = APP_CONF_VER,
        .bus_cfg = CDCTL_CFG_DFT(0xfe),
        .p_mac = 0x10,
        .k_en = 2,
        .k_pwd = "123456",
        .ble_itvl_min = 6,
        .ble_itvl_max = 12,
        .proxy_sel = 1
};

csa_t csa;
uint8_t bus_mac; // bus_cfg.mac backup


void load_conf(void)
{
    csa_t csa_tmp = { 0 };
    int ret = esp_flash_read(NULL, &csa_tmp, APP_CONF_ADDR, sizeof(csa_t));
    printf("load_conf: magic code = %04x, ret: %d\n", csa_tmp.magic_code, ret);
    csa = csa_dft;

    if (csa_tmp.magic_code == 0xcdcd && csa_tmp.conf_ver == APP_CONF_VER) {
        memcpy(&csa, &csa_tmp, offsetof(csa_t, _end_save));
        csa.conf_from = 1;
    } else if (csa_tmp.magic_code == 0xcdcd && (csa_tmp.conf_ver >> 8) == (APP_CONF_VER >> 8)) {
        memcpy(&csa, &csa_tmp, offsetof(csa_t, _end_common));
        csa.conf_from = 2;
        csa.conf_ver = APP_CONF_VER;
    }
    printf("conf_from: %d\n", csa.conf_from);
    memset(&csa.do_reboot, 0, 3);
    memset(csa.remote_ip, 0xff, sizeof(csa.remote_ip));
    csa.remote_port = 0xffff;
    memset(csa.local_ip, 0xff, sizeof(csa.local_ip));
    memset(csa.scan_rssi, 127, sizeof(csa.scan_rssi));
    csa.wifi_state = 0x80;
    bus_mac = csa.bus_cfg.mac;
}

int save_conf(void)
{
    int ret = flash_erase(APP_CONF_ADDR, 2048);
    if (ret)
        d_info("conf: failed to erase flash\n");
    ret = flash_write(APP_CONF_ADDR, offsetof(csa_t, _end_save), (uint8_t *)&csa);

    if (!ret) {
        d_info("conf: save to flash successed, size: %d\n", offsetof(csa_t, _end_save));
        return 0;
    } else {
        d_error("conf: save to flash error\n");
        return 1;
    }
}

int flash_erase(uint32_t addr, uint32_t len)
{
    uint32_t _addr = addr & ~4095;
    uint32_t _len = len + (addr - _addr);
    _len = (_len + 4095) & ~4095;
    int ret = esp_flash_erase_region(NULL, _addr, _len);
    d_debug("flash erase: %08lx +%08lx (%08lx +%08lx), ret: %d\n", addr, len, _addr, _len, ret);
    return ret ? -1 : 0;
}

int flash_write(uint32_t addr, uint32_t len, const uint8_t *buf)
{
    int ret = esp_flash_write(NULL, buf, addr, len);
    d_verbose("flash write: %08lx %ld, ret: %d\n", addr, len, ret);
    return ret ? -1 : 0;
}

int flash_cal_crc(uint32_t src_addr, uint32_t len, uint16_t *crc)
{
    uint8_t buffer[256];
    uint16_t c = 0xffff;
    uint32_t p = src_addr;
    while (true) {
        uint32_t left_len = len - (p - src_addr);
        uint32_t sub_len = min(left_len, 256);
        if (sub_len == 0)
            break;
        uint32_t copy_len = (sub_len + 3) & ~3;
        int ret = esp_flash_read(NULL, buffer, p, copy_len);
        if (ret != 0)
            return ret;
        c = crc16_sub(buffer, sub_len, c);
        p += sub_len;
    }
    *crc = c;
    return 0;
}


#define t_name(expr)  \
        (_Generic((expr), \
                int8_t: "b", uint8_t: "B", \
                int16_t: "h", uint16_t: "H", \
                int32_t: "i", uint32_t: "I", \
                int: "i", \
                bool: "b", \
                float: "f", \
                char *: "[c]", \
                int8_t *: "[b]", \
                uint8_t *: "[B]", \
                regr_t: "H,H", \
                regr_t *: "{H,H}", \
                default: "-"))


#define CSA_SHOW(_p, _x, _desc) \
        d_debug("  [ 0x%04x, %d, \"%s\", " #_p ", \"" #_x "\", \"%s\" ],\n", \
                offsetof(csa_t, _x), sizeof(csa._x), t_name(csa._x), _desc);

#define CSA_SHOW_SUB(_p, _x, _y_t, _y, _desc) \
        d_debug("  [ 0x%04x, %d, \"%s\", " #_p ", \"" #_x "_" #_y "\", \"%s\" ],\n", \
                offsetof(csa_t, _x) + offsetof(_y_t, _y), sizeof(csa._x._y), t_name(csa._x._y), _desc);

void csa_list_show(void)
{
    d_debug("csa_list_show:\n");
    d_debug("\n");

    CSA_SHOW(1, magic_code, "Magic code: 0xcdcd");
    CSA_SHOW(1, conf_ver, "Config version");
    CSA_SHOW(1, conf_from, "0: default config, 1: all from flash, 2: partly from flash");
    CSA_SHOW(0, do_reboot, "Reboot: 1 stay in bootloader, 2 boot app");
    CSA_SHOW(0, save_conf, "Write 1 to save current config to flash");
    d_debug("\n");

    CSA_SHOW(0, dbg_en, "1: Report debug message to host, 0: do not report");
    d_debug("\n");

    CSA_SHOW_SUB(1, bus_cfg, cdctl_cfg_t, mac, "RS-485 port id, range: 0~254");
    CSA_SHOW_SUB(0, bus_cfg, cdctl_cfg_t, baud_l, "RS-485 baud rate for first byte");
    CSA_SHOW_SUB(0, bus_cfg, cdctl_cfg_t, baud_h, "RS-485 baud rate for follow bytes");
    CSA_SHOW_SUB(1, bus_cfg, cdctl_cfg_t, filter_m, "Multicast address");
    CSA_SHOW_SUB(0, bus_cfg, cdctl_cfg_t, mode, "0: Traditional, 1: Arbitration, 2: Break Sync");
    CSA_SHOW_SUB(0, bus_cfg, cdctl_cfg_t, tx_permit_len, "Allow send wait time");
    CSA_SHOW_SUB(0, bus_cfg, cdctl_cfg_t, max_idle_len, "Max idle wait time for BS mode");
    CSA_SHOW_SUB(0, bus_cfg, cdctl_cfg_t, tx_pre_len, "Active TX_EN before TX");
    d_debug("\n");

    CSA_SHOW(1, p_mac, "Predefined remote rs485 address");
    CSA_SHOW(0, k_en, "Key enable (bit0: ble, bit1: udp)");
    CSA_SHOW(0, k_pwd, "Key string");
    d_debug("\n");

    CSA_SHOW(0, ble_itvl_min, "");
    CSA_SHOW(0, ble_itvl_max, "");
    CSA_SHOW(0, wifi_ssid, "");
    CSA_SHOW(0, wifi_pwd, "");
    CSA_SHOW(0, wifi_conf, "0: turn off, 1: station mode");
    d_debug("\n");

    CSA_SHOW(0, proxy_sel, "1: ble, 2: wifi");
    CSA_SHOW(0, ble_stop, "1: stop advertisement");
    CSA_SHOW(0, k_st_ble, "Key verify status");
    CSA_SHOW(1, k_random, "");
    CSA_SHOW(1, k_cnt_rx_ble, "");
    CSA_SHOW(1, k_cnt_tx_ble, "");
    CSA_SHOW(1, k_cnt_rx_udp, "");
    CSA_SHOW(1, k_cnt_tx_udp, "");
    d_debug("\n");

    CSA_SHOW(0, ble_mtu_cur, "");
    CSA_SHOW(0, ble_itvl_cur, "");
    d_debug("\n");

    CSA_SHOW(1, wifi_state, "{disabled, 2'd0, connecting, 2'd0, connected, scan}");
    CSA_SHOW(2, remote_ip, "Client ip, starts with ffff: empty");
    CSA_SHOW(0, remote_port, "Client udp port, 0xffff: empty");
    for (int i = 0; i < 4; i++)
        d_debug("  [ 0x%04x, 16, \"[B]\", 2, \"local_ip[%d]\", \"\" ],", offsetof(csa_t, local_ip) + 16*i, i);
    CSA_SHOW(0, scan_start, "Write 1 to start wifi scan");
    CSA_SHOW(0, scan_auth, "0: open, 1: wep, 2: wpa_psk, 3: wpa2_psk, 4: wpa_wpa2_psk ...");
    CSA_SHOW(0, scan_rssi, "");
    for (int i = 0; i < 20; i++)
        d_debug("  [ 0x%04x, 32, \"[c]\", 0, \"scan_ssid[%d]\", \"\" ],", offsetof(csa_t, scan_ssid) + 32*i, i);
    d_debug("\n");
}
