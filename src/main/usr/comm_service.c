/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <d@d-l.io>
 */

#include "cd_main.h"
static const char *tag = "comm-ser";
static const char *sw_version = "1.17";

char cpu_id[25] = { 0 };
static char info_str[100];
static cd_spinlock_t p5_lock = {0};

static QueueHandle_t cmd_rx_queue = NULL;


int sent_cmd(uint8_t dst_mac, uint8_t *d, uint8_t d_len, bool reply, cd_frame_t **rfrm)
{
    cd_frame_t *frm = NULL;
    BaseType_t ret = xQueueReceive(cmd_rx_queue, &frm, 0);
    if (ret == pdTRUE)
        cd_list_put(&frame_free_head, frm);

    for (int i = 0; i < 20; i++) {
        frm = cd_list_get(&frame_free_head);
        if (frm)
            break;
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    if (!frm)
        return -1;

    frm->dat[0] = bus_mac;
    frm->dat[1] = dst_mac;
    frm->dat[2] = d_len;
    memcpy(frm->dat + 3, d, d_len);
    cdctl_put_tx_frame(frm);
    if (!reply)
        return 0;

    ret = xQueueReceive(cmd_rx_queue, &frm, 500 / portTICK_PERIOD_MS);
    if (ret == pdTRUE) {
        if (rfrm)
            *rfrm = frm;
        else
            cd_list_put(&frame_free_head, frm);
        return 0;
    }
    return -1;
}


static void send_frame(cd_frame_t *frame, uint8_t p_len)
{
    frame->dat[2] = p_len + 2;
    swap(frame->dat[0], frame->dat[1]); // swap mac
    swap(frame->dat[3], frame->dat[4]); // swap port
    if (frame->intf == INTF_BLE) {
        BaseType_t ret = xQueueSend(ble_notify_queue, (void *) &frame, (TickType_t) 0);
        //ESP_LOGI(tag, "reply ble cmd: frame: %p\n", frame);
        if (ret != pdPASS) {
            ESP_LOGI(tag, "reply ble cmd: enqueue err\n");
            cd_list_put(&frame_free_head, frame);
        }
    } else if (frame->intf == INTF_TCP) {
        BaseType_t ret = xQueueSend(tcp_notify_queue, (void *) &frame, (TickType_t) 0);
        if (ret != pdPASS) {
            ESP_LOGI(tag, "reply tcp cmd: enqueue err\n");
            cd_list_put(&frame_free_head, frame);
        }
    }
#if !CD_DISABLE_UDP
    else if (frame->intf == INTF_UDP) {
        BaseType_t ret = xQueueSend(udp_notify_queue, (void *) &frame, (TickType_t) 0);
        //ESP_LOGI(tag, "reply udp cmd: frame: %p\n", frame);
        if (ret != pdPASS) {
            ESP_LOGI(tag, "reply udp cmd: enqueue err\n");
            cd_list_put(&frame_free_head, frame);
        }
    }
#endif
    else { // reply rs485 cmd
        cdctl_put_tx_frame(frame);
    }
}

static void init_info_str(void)
{
    // M: model; S: serial string; HW: hardware version; SW: software version
    // sprintf(info_str, "M: cd-esp; S: %s; SW: %s", cpu_id, SW_VER);
    // d_info("info: %s, git: %s\n", info_str, SW_VER_FULL);
    sprintf(info_str, "M: cd-esp; S: %s; SW: %s", cpu_id, sw_version);
    d_info("info: %s, git: %s\n", info_str, sw_version);
}


// device info
static void p1_handler(cd_frame_t *frame)
{
    uint8_t *p_dat = frame->dat + 5;
    uint8_t p_len = frame->dat[2] - 2;

    if (p_len == 0) {
        strcpy((char *)p_dat, info_str);
        send_frame(frame, strlen(info_str));
    } else {
        cd_list_put(&frame_free_head, frame);
    }
}

// flash memory manipulation
static void p8_handler(cd_frame_t *frame)
{
    uint8_t *p_dat = frame->dat + 5;
    uint8_t p_len = frame->dat[2] - 2;
    bool reply = !(*p_dat & 0x80);
    *p_dat &= 0x7f;

    if (*p_dat == 0x2f && p_len == 9) {
        uint32_t addr = get_unaligned32(p_dat + 1);
        uint32_t len = get_unaligned32(p_dat + 5);
        uint8_t ret = flash_erase(addr, len);
        *p_dat = ret ? 1 : 0;
        if (reply)
            send_frame(frame, 1);

    } else if (*p_dat == 0x00 && p_len == 6) {
        uint32_t addr = get_unaligned32(p_dat + 1);
        uint8_t len = min(p_dat[5], CDN_MAX_DAT - 1);
        int ret = esp_flash_read(NULL, p_dat + 1, addr, len);
        *p_dat = ret ? 1 : 0;
        if (reply)
            send_frame(frame, ret ? 1 : len + 1);

    } else if (*p_dat == 0x20 && p_len > 8) {
        uint32_t addr = get_unaligned32(p_dat + 1);
        uint8_t len = p_len - 5;
        uint8_t ret = flash_write(addr, len, p_dat + 5);
        *p_dat = ret ? 1 : 0;
        if (reply)
            send_frame(frame, 1);

    } else if (*p_dat == 0x10 && p_len == 9) {
        uint32_t addr = get_unaligned32(p_dat + 1);
        uint32_t len = get_unaligned32(p_dat + 5);
        uint16_t crc = 0xffff;
        int ret = flash_cal_crc(addr, len, &crc);
        if (ret == 0) {
            *p_dat = 0;
            put_unaligned16(crc, p_dat + 1);
        } else {
            *p_dat = 1;
        }
        if (reply)
            send_frame(frame, ret ? 1 : 3);

    } else {
        cd_list_put(&frame_free_head, frame);
        return;
    }
    if (!reply)
        cd_list_put(&frame_free_head, frame);
}

// csa manipulation
static void p5_handler(cd_frame_t *frame)
{
    uint32_t flags;
    uint8_t *p_dat = frame->dat + 5;
    uint8_t p_len = frame->dat[2] - 2;
    bool reply = !(*p_dat & 0x80);
    *p_dat &= 0x7f;

    if (*p_dat == 0x00 && p_len == 4) {
        uint16_t offset = get_unaligned16(p_dat + 1);
        uint8_t len = min(p_dat[3], CDN_MAX_DAT - 1);
        cd_irq_save(&p5_lock, flags);
        memcpy(p_dat + 1, ((void *) &csa) + offset, len);
        cd_irq_restore(&p5_lock, flags);
        *p_dat = 0;
        if (reply)
            send_frame(frame, len + 1);

    } else if (*p_dat == 0x20 && p_len > 3) {
        uint16_t offset = get_unaligned16(p_dat + 1);
        uint8_t len = p_len - 3;
        uint8_t *src_addr = p_dat + 3;
        uint16_t start = clip(offset, 0, sizeof(csa_t));
        uint16_t end = clip(offset + len, 0, sizeof(csa_t));
        cd_irq_save(&p5_lock, flags);
        memcpy(((void *) &csa) + start, src_addr + (start - offset), end - start);
        cd_irq_restore(&p5_lock, flags);
        *p_dat = 0;
        if (reply)
            send_frame(frame, 1);

    } else if (*p_dat == 0x01 && p_len == 4) {
        uint16_t offset = get_unaligned16(p_dat + 1);
        uint8_t len = min(p_dat[3], CDN_MAX_DAT - 1);
        memcpy(p_dat + 1, ((void *) &csa_dft) + offset, len);
        *p_dat = 0;
        if (reply)
            send_frame(frame, len + 1);

    } else {
        cd_list_put(&frame_free_head, frame);
        return;
    }
    if (!reply)
        cd_list_put(&frame_free_head, frame);
}


static inline void serial_cmd_dispatch(void)
{
    cd_frame_t *frame = cd_list_get(&ble_rx_head);
    if (frame) {
        frame->intf = INTF_BLE;
    } else {
        frame = cd_list_get(&cdctl_rx_head);
        if (frame) {
            BaseType_t ret = pdFAIL;
            frame->intf = INTF_485;
            bool proxy_cmd = (frame->dat[3] & 0b11100000) == 0b01100000; // report / cmd
            bool proxy_ret = (frame->dat[4] & 0b11100000) == 0b01100000; // reply
            if (frame->dat[3] >= 0x40 && frame->dat[4] > 8) // report port > 8
                proxy_cmd = true;
            if (proxy_cmd || proxy_ret) {
                frame->dat[1] = 0;
                if (csa.proxy_sel == INTF_UDP) {
                    frame->w_hdr = (csa.k_en & 2) ? 0xc0 : 0;
                    if (frame->dat[0] != csa.p_mac)
                        frame->w_hdr |= 0xa0;
                    if (tcp_src_addr_valid) {
                        ret = xQueueSend(tcp_notify_queue, (void *) &frame, (TickType_t) 0);
                    }
#if !CD_DISABLE_UDP
                    else if (udp_src_addr_valid) {
                        memcpy(&frame->udp_addr, &udp_src_addr, sizeof(udp_src_addr));
                        //ESP_LOGI(tag, "forward: rs485 -> udp client, frame: %p\n", frame);
                        ret = xQueueSend(udp_notify_queue, (void *) &frame, (TickType_t) 0);
                    }
#endif
                } else { // ble
                    frame->w_hdr = (csa.k_en & 1) ? 0xc0 : 0;
                    if (frame->dat[0] != csa.p_mac)
                        frame->w_hdr |= 0xa0;
                    //ESP_LOGI(tag, "forward: rs485 -> ble central, frame: %p\n", frame);
                    ret = xQueueSend(ble_notify_queue, (void *) &frame, (TickType_t) 0);

                }
                if (ret != pdPASS) {
                    ESP_LOGI(tag, "queue send err (proxy)\n");
                    cd_list_put(&frame_free_head, frame);
                }
                return;
            }

            if (frame->dat[4] >= 0x40) { // local cmd reply
                ESP_LOGI(tag, "cmd reply: frame: %p\n", frame);
                ret = xQueueSend(cmd_rx_queue, (void *) &frame, (TickType_t) 0);
                if (ret != pdPASS) {
                    ESP_LOGI(tag, "queue send err (reply)\n");
                    cd_list_put(&frame_free_head, frame);
                }
                return;
            }
        }
    }

    if (!frame) {
        frame = cd_list_get(&udp_rx_head);
        if (frame)
            frame->intf = INTF_UDP;
    }

    if (!frame) {
        frame = cd_list_get(&tcp_rx_head);
        if (frame)
            frame->intf = INTF_TCP;
    }

    if (frame) {
        uint8_t server_num = frame->dat[4];
        uint8_t *p_dat = frame->dat + 5;

        if (!(frame->w_hdr & 0x40) && ((frame->intf == INTF_BLE && (csa.k_en & 1))
                || ((frame->intf == INTF_UDP || frame->intf == INTF_TCP) && (csa.k_en & 2)))) {
            bool is_err = true;
            uint8_t subs = p_dat[0];

            if (server_num == 1) {
                is_err = false;
            } else if (server_num == 5) {
                uint16_t offset = get_unaligned16(p_dat + 1);
                if (subs == 0x00 && offset >= offsetof(csa_t, proxy_sel))
                    is_err = false;
            }
            if (is_err) {
                ESP_LOGI(tag, "cmd err, !key, intf: %d\n", frame->intf);
                cd_list_put(&frame_free_head, frame);
                if (frame->intf == INTF_BLE)
                    ble_gap_terminate(ble_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return;
            }
        }

        if (frame->intf >= INTF_BLE && frame->dat[1] != bus_mac) { // except 485
            // forward: ble central / udp client -> rs485
            frame->dat[0] = bus_mac;
            frame->dat[3] |= 0b00100000;
            cdctl_put_tx_frame(frame);
        } else {
            //ESP_LOGI(tag, "cmd %d\n", server_num);
            switch (server_num) {
            case 1: p1_handler(frame); break;
            case 5: p5_handler(frame); break;
            case 8: p8_handler(frame); break;
            default:
                ESP_LOGI(tag, "cmd err ser_num: %d\n", server_num);
                cd_list_put(&frame_free_head, frame);
            }
        }
    }
}


void comm_service_init(void)
{
    init_info_str();
    cmd_rx_queue = xQueueCreate(1, sizeof(void *));
}

void comm_service_poll(void)
{
    serial_cmd_dispatch();

    if (csa.save_conf) {
        csa.save_conf = false;
        save_conf();
    }
    if (csa.do_reboot) {
        ESP_LOGI(tag, "do_reboot ...\n");
        esp_restart();
    }
}
