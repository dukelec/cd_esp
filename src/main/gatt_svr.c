/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2025, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <d@d-l.io>
 */

#include "main.h"
#include "cd_main.h"

// b334xxxx-56ba-40b1-8ecb-8fe18dfffddd
#define THRPT_UUID_DECLARE(uuid16)                              \
    ((const ble_uuid_t *) (&(ble_uuid128_t) BLE_UUID128_INIT(   \
    0xdd, 0xfd, 0xff, 0x8d, 0xe1, 0x8f, 0xcb, 0x8e,             \
    0xb1, 0x40, 0xba, 0x56, uuid16, uuid16 >> 8, 0x34, 0xb3     \
    )))

#define THRPT_SVC           0x0001
#define THRPT_CHR_WRITE     0x0002
#define THRPT_CHR_NOTIFY    0x0003

#define RX_BUF_SIZE (4 + 3 + 253 * 16 + 16) // 4071
static uint8_t rx_buf[RX_BUF_SIZE] __attribute__((aligned(4))) = {0};
static uint16_t rx_pos = 4;
static uint8_t frag_cnt = 0;

static const char *tag = "cd-ble";
uint16_t ble_notify_handle;

static int gatt_svr_read_write_long_test(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatts_test_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = THRPT_UUID_DECLARE(THRPT_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = THRPT_UUID_DECLARE(THRPT_CHR_WRITE),
                .access_cb = gatt_svr_read_write_long_test,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,

            }, {
                .uuid = THRPT_UUID_DECLARE(THRPT_CHR_NOTIFY),
                .access_cb = gatt_svr_read_write_long_test,
                .val_handle = &ble_notify_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            }, {
                0, // no more
            }
        },
    }, {
        0, // no more
    },
};

static uint16_t extract_uuid16_from_thrpt_uuid128(const ble_uuid_t *uuid)
{
    const uint8_t *u8ptr;
    uint16_t uuid16;

    u8ptr = BLE_UUID128(uuid)->value;
    uuid16 = u8ptr[12];
    uuid16 |= (uint16_t)u8ptr[13] << 8;
    return uuid16;
}

static int gatt_svr_chr_write(uint16_t conn_handle, uint16_t attr_handle,
        struct os_mbuf *om, uint16_t min_len, uint16_t max_len, void *dst, uint16_t *len)
{
    uint16_t om_len;
    int rc;

    om_len = OS_MBUF_PKTLEN(om);
    if (om_len < min_len || om_len > max_len)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    rc = ble_hs_mbuf_to_flat(om, dst, max_len, len);
    if (rc != 0)
        return BLE_ATT_ERR_UNLIKELY;
    return 0;
}

static int gatt_svr_read_write_long_test(uint16_t conn_handle, uint16_t attr_handle,
        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static bool skip_err_rpt = false;
    static bool frag_cnt_err = false;
    uint16_t uuid16 = extract_uuid16_from_thrpt_uuid128(ctxt->chr->uuid);
    assert(uuid16 != 0);

    if (uuid16 == THRPT_CHR_WRITE && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t *cur_buf = rx_buf + rx_pos - 1;
        uint16_t cur_free = RX_BUF_SIZE - (rx_pos - 1);
        uint16_t len = min(cur_free, OS_MBUF_PKTLEN(ctxt->om));
        uint8_t dat_bk = *cur_buf;

        int rc = gatt_svr_chr_write(conn_handle, attr_handle, ctxt->om, 0, cur_free, cur_buf, NULL);
        //ESP_LOGI(tag, "BLE_GATT_ACCESS_OP_WRITE_CHR, len: %d\n", len);

        uint8_t w_hdr;
        uint8_t tgt_mac;
        uint8_t *cdn_buf;
        uint16_t cdn_len;
        uint8_t err_code = 0; // 1: frag err, 2: aes err

        if (len < 2) {
            rx_pos = 4;
            return rc;
        }

        if ((*cur_buf & 0x80) == 0) { // no w_hdr
            if (rx_pos != 4) {
                rx_pos = 4;
                return rc;
            }
            w_hdr = 0;
            cdn_len = len;
            cdn_buf = cur_buf;
            tgt_mac = (cdn_buf[0] & 0b11100000) == 0b01100000 ? csa.p_mac : bus_mac;
            goto cdn_to_frame;

        } else if ((*cur_buf & 0x18) == 0) { // no fragment
            if (rx_pos != 4) {
                rx_pos = 4;
                return rc;
            }
            w_hdr = rx_buf[3];
            cdn_len = len - 1;
            cdn_buf = cur_buf + 1;
            if ((rx_buf[3] & 0x40) == 0)
                goto parse_w_hdr;
            else
                goto decrypt;

        } else if ((*cur_buf & 0x18) == 0x08) { // first fragment
            if (rx_pos != 4) {
                rx_pos = 4;
                frag_cnt_err = true;
                return rc;
            }
            frag_cnt_err = false;
            rx_pos += len - 1;
            frag_cnt = *cur_buf & 7;
            return rc;

        } else if ((*cur_buf & 0x18) == 0x18) { // last fragment
            frag_cnt = (frag_cnt + 1) & 7;
            if (frag_cnt_err || (*cur_buf & 7) != frag_cnt) {
                ESP_LOGE(tag, "ble rx frag_cnt err, cur: %02x != %02x, %d %d",
                        *cur_buf, frag_cnt, frag_cnt_err, skip_err_rpt);
                err_code = 1; // frag err
                goto reply_err_code;
            }

            *cur_buf = dat_bk;
            w_hdr = rx_buf[3];
            cdn_len = rx_pos + (len - 1) - 4;
            cdn_buf = rx_buf + 4;
            if ((rx_buf[3] & 0x40) == 0)
                goto parse_w_hdr;
            else
                goto decrypt;

        } else { // more fragment
            rx_pos += len - 1;
            frag_cnt = (frag_cnt + 1) & 7;
            if (frag_cnt_err || (*cur_buf & 7) != frag_cnt) {
                ESP_LOGE(tag, "ble rx frag_cnt err, cur: %02x != %02x, %d", *cur_buf, frag_cnt, frag_cnt_err);
                frag_cnt_err = true;
                return rc;
            }
            *cur_buf = dat_bk;
            return rc;
        }

decrypt:
        int plain_len = aes256_cbc_decrypt(cdn_buf, cdn_len, cdn_buf);
        if (plain_len < 4 || get_unaligned16(cdn_buf) != csa.k_cnt_rx_ble) {
            ESP_LOGE(tag, "plain_len: %d, rx_cnt: %04x != %04x, %d",
                    plain_len, get_unaligned16(cdn_buf), csa.k_cnt_rx_ble, skip_err_rpt);
            err_code = 2; // aes err
            goto reply_err_code;
        }
        csa.k_cnt_rx_ble++;
        csa.k_st_ble = true;
        cdn_len = plain_len - 2;
        cdn_buf = rx_buf + 6; // tgt_mac or cdn_payload

parse_w_hdr:
        if ((rx_buf[3] & 0x20) != 0) { // mac_flag
            tgt_mac = *cdn_buf++;
            cdn_len--;
        } else {
            tgt_mac = (cdn_buf[0] & 0b11100000) == 0b01100000 ? csa.p_mac : bus_mac;
        }

cdn_to_frame:
        skip_err_rpt = false;
        uint8_t *sub_buf = cdn_buf;
        while (sub_buf < cdn_buf + cdn_len) {
            int sub_len = min(253, cdn_len - (sub_buf - cdn_buf));
            cd_frame_t *frame = cd_list_get(&frame_free_head);
            if (frame) {
                frame->w_hdr = w_hdr & 0b11100000;
                frame->dat[0] = 0;
                frame->dat[1] = tgt_mac;
                frame->dat[2] = sub_len;
                memcpy(frame->dat + 3, sub_buf, sub_len);
                cd_list_put(&ble_rx_head, frame);
                if (dispatch_task_handle)
                    xTaskNotifyGive(dispatch_task_handle);
                portYIELD();
            } else {
                ESP_LOGE(tag, "rx no free frame");
                break;
            }
            sub_buf += sub_len;
        }
        rx_pos = 4;
        return rc;

reply_err_code:
        rx_pos = 4;
        if (skip_err_rpt)
            return rc;
        skip_err_rpt = true;
        cd_frame_t *frame = cd_list_get(&frame_free_head);
        if (frame) {
            frame->dat[0] = bus_mac;
            frame->dat[1] = 0;
            frame->dat[2] = 0;
            frame->w_hdr = 0x80 | err_code;

            BaseType_t ret = xQueueSend(ble_notify_queue, (void *) &frame, (TickType_t) 0);
            //ESP_LOGI(tag, "reply the cmd from ble, frame: %p (send_frame)\n", frame);
            if (ret != pdPASS) {
                ESP_LOGI(tag, "queue send err\n");
                cd_list_put(&frame_free_head, frame);
            }
        }
        return rc;
    }

    assert(0);
    ESP_LOGE(tag, "BLE_ATT_ERR_UNLIKELY\n");
    return BLE_ATT_ERR_UNLIKELY;
}

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(tag, "registered service %s with handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf), ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(tag, "registering characteristic %s with def_handle=%d val_handle=%d\n",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf), ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGD(tag, "registering descriptor %s with handle=%d",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf), ctxt->dsc.handle);
        break;

    default:
        assert(0);
        break;
    }
}

int gatt_svr_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatts_test_svcs);
    if (rc != 0)
        return rc;

    rc = ble_gatts_add_svcs(gatts_test_svcs);
    if (rc != 0)
        return rc;

    return 0;
}
