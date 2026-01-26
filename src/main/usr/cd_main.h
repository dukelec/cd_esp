/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <d@d-l.io>
 */

#ifndef __CD_MAIN_H__
#define __CD_MAIN_H__

#include "cd_utils.h"
#include "cd_list.h"
#include "cdctl_it.h"
#include "cdbus_uart.h"
#include "modbus_crc.h"

#define APP_CONF_ADDR       0x003ff000 // last page
#define APP_CONF_VER        0x0201

#define FRAME_MAX           40

typedef enum {
    INTF_485 = 0,
    INTF_BLE,
    INTF_UDP
} intf_t;


typedef struct {
    uint16_t        offset;
    uint16_t        size;
} regr_t; // reg range


typedef struct {
    uint16_t        magic_code;     // 0xcdcd
    uint16_t        conf_ver;
    uint8_t         conf_from;      // 0: default, 1: all from flash, 2: partly from flash
    uint8_t         do_reboot;
    bool            _reserved0;
    bool            save_conf;

    uint8_t         dbg_en;
    cdctl_cfg_t     bus_cfg;
    #define         _end_common _reserved1

    uint8_t         _reserved1[92];
    uint8_t         p_mac;          // predefined remote rs485 address

    uint8_t         _reserved2[12];
    uint8_t         k_en;           // bit0: ble, bit1: udp
    uint8_t         k_pwd[24];

    uint8_t         ble_itvl_min;   // units 1.25ms
    uint8_t         ble_itvl_max;
    uint8_t         wifi_ssid[32];
    uint8_t         wifi_pwd[64];
    uint8_t         wifi_conf;      // 0: disconnect, 1: station

    // end of flash
    // below read only without key auth
    #define         _end_save proxy_sel
    uint8_t         proxy_sel;      // 0: empty, 1: ble, 2: udp
    bool            ble_stop;

    uint8_t         _reserved4[13];
    bool            k_st_ble;       // for ble only
    uint32_t        k_random;
    uint16_t        k_cnt_rx_ble;
    uint16_t        k_cnt_tx_ble;
    uint16_t        k_cnt_rx_udp;
    uint16_t        k_cnt_tx_udp;

    uint8_t         _reserved5[10];
    uint16_t        ble_mtu_cur;    // characteristic r/w max size: ble_mtu_cur - 3 (att header)
    uint8_t         ble_itvl_cur;
    bool            ble_connect;
    uint32_t        t_ble_connect;

    uint8_t         _reserved6[12];
    union {
        uint8_t wifi_state;
        struct {
            uint8_t scan        : 1; // [0]
            uint8_t connected   : 1; // [1]
            uint8_t             : 2; // [3:2]
            uint8_t connecting  : 1; // [4]
            uint8_t             : 2; // [6:5]
            uint8_t disabled    : 1; // [7]
        } wifi_state_;
    };
    uint8_t         remote_ip[16];
    uint16_t        remote_port;
    uint8_t         local_ip[4][16];
    uint8_t         scan_start;
    uint8_t         scan_auth[20];
    int8_t          scan_rssi[20];
    uint8_t         scan_ssid[20][32];

} csa_t; // config status area

extern csa_t csa;
extern const csa_t csa_dft;
extern char cpu_id[25];
extern uint8_t bus_mac;


int flash_erase(uint32_t addr, uint32_t len);
int flash_write(uint32_t addr, uint32_t len, const uint8_t *buf);
int flash_cal_crc(uint32_t src_addr, uint32_t len, uint16_t *crc);

extern list_head_t frame_free_head;
extern list_head_t ble_rx_head;
extern list_head_t udp_rx_head;

int sent_cmd(uint8_t dst_mac, uint8_t *d, uint8_t d_len, bool reply, cd_frame_t **rfrm);
void common_service_init(void);
void common_service_routine(void);

void cd_main_early(void);
void cd_main_late(void);
void load_conf(void);
int save_conf(void);
void csa_list_show(void);

#endif
