/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <d@d-l.io>
 */

#ifndef __CD_FRAME_H__
#define __CD_FRAME_H__

#include "cd_list.h"

// 256 bytes are enough for the CDCTL controller (without CRC)
// 258 bytes are enough for the UART controller (with CRC)
// allow smaller sizes to save memory
#ifndef CD_FRAME_SIZE
#define CD_FRAME_SIZE   256
#endif

typedef struct {
    list_node_t node;
    uint8_t     intf; // 0: 485, 1: ble, 2: wifi, 3: usb
    // payload (dat + 3): 32bit alignment for esp32 dma
    uint8_t     dat[CD_FRAME_SIZE];

    // bit7: 1: bit6: aes, bit5: mac, bit[4:3]: fragment, bit[2:0]: cnt/err
    // bit7: 0: no w_hdr (wireless header)
    uint8_t     w_hdr;
    struct sockaddr_storage udp_addr;
} cd_frame_t;

#endif
