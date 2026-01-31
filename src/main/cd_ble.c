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

#define PREFERRED_MTU_VALUE       (247+247+4) // 498
#define LL_PACKET_TIME            2120
#define LL_PACKET_LENGTH          251

static const char *tag = "cd-ble";
static char device_name[20] = {0};
static uint8_t mfg_data[8] = {0xe5, 0x02};
static bool notify_state;
static uint8_t gatts_addr_type;
uint16_t ble_conn_handle;
QueueHandle_t ble_notify_queue = NULL;

#define TX_BUF_SIZE (4 + 3 + 253 * 16 + 16) // 4071
static uint8_t tx_buf[TX_BUF_SIZE] __attribute__((aligned(4))) = {0};


static int gatts_gap_event(struct ble_gap_event *event, void *arg);


void print_addr(const void *addr)
{
    const uint8_t *u8p = addr;
    ESP_LOGI(tag, "%02x:%02x:%02x:%02x:%02x:%02x", u8p[5], u8p[4], u8p[3], u8p[2], u8p[1], u8p[0]);
}

static void bleprph_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    ESP_LOGI(tag, "handle=%d our_ota_addr_type=%d our_ota_addr=", desc->conn_handle, desc->our_ota_addr.type);
    print_addr(desc->our_ota_addr.val);
    ESP_LOGI(tag, " our_id_addr_type=%d our_id_addr=", desc->our_id_addr.type);
    print_addr(desc->our_id_addr.val);
    ESP_LOGI(tag, " peer_ota_addr_type=%d peer_ota_addr=", desc->peer_ota_addr.type);
    print_addr(desc->peer_ota_addr.val);
    ESP_LOGI(tag, " peer_id_addr_type=%d peer_id_addr=", desc->peer_id_addr.type);
    print_addr(desc->peer_id_addr.val);
    ESP_LOGI(tag, " conn_itvl=%d conn_latency=%d supervision_timeout=%d encrypted=%d authenticated=%d bonded=%d",
                desc->conn_itvl, desc->conn_latency, desc->supervision_timeout,
                desc->sec_state.encrypted, desc->sec_state.authenticated, desc->sec_state.bonded);
    csa.ble_itvl_cur = desc->conn_itvl;
}

static void gatts_advertise(void)
{
    static struct ble_gap_adv_params adv_params;
    static struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP; // forthcoming, ble only
    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;
    fields.mfg_data = mfg_data;
    fields.mfg_data_len = sizeof(mfg_data);
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(tag, "error setting advertisement data; rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(gatts_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gatts_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(tag, "error enabling advertisement; rc=%d", rc);
        return;
    }
}

static void notify_task(void *arg)
{
    struct os_mbuf *om;
    cd_frame_t *frame;
    list_head_t head_in = {0};
    list_head_t head_left = {0};
    uint8_t frag_cnt = 0;

    while (true) {
        bool fragment = false;
        while (head_left.len) {
            frame = cd_list_get(&head_left);
            cd_list_put(&head_in, frame);
        }
        frame = cd_list_get(&head_in);
        if (!frame)
            xQueueReceive(ble_notify_queue, &frame, portMAX_DELAY);
        //ESP_LOGI(tag, "reply ble cmd, frame: %p\n", frame);
        if (!notify_state) {
            cd_list_put(&frame_free_head, frame);
            continue;
        }

        uint8_t shift = 0;
        uint8_t mac = frame->dat[0];
        if (frame->w_hdr & 0x80) {
            if (frame->w_hdr & 0x40) {
                shift += 2;
                put_unaligned16(csa.k_cnt_tx_ble++, tx_buf + 4);
            }
            if (frame->w_hdr & 0x20) {
                tx_buf[4 + shift] = frame->dat[0];
                shift++;
            }
        }
        tx_buf[3] = frame->w_hdr;
        memcpy(tx_buf + 4 + shift, frame->dat + 3, frame->dat[2]);
        uint16_t len = frame->dat[2] + shift;
        cd_list_put(&frame_free_head, frame);

        // if cdnet_len == 253 && has new frame without timeout && w_hdr and src_mac same
        if (len - shift == 253) {
            uint8_t pkt_num = 1;
            while (true) {
                frame = cd_list_get(&head_in);
                if (!frame)
                    xQueueReceive(ble_notify_queue, &frame, 50 / portTICK_PERIOD_MS);
                if (!frame)
                    break;
                if (frame->w_hdr != tx_buf[3] || mac != frame->dat[0]) {
                    cd_list_put(&head_left, frame);
                    continue;
                }
                fragment = true;
                uint8_t l = frame->dat[2];
                memcpy(tx_buf + 4 + len, frame->dat + 3, l);
                len += l;
                cd_list_put(&frame_free_head, frame);
                pkt_num++;
                if (l != 253 || pkt_num == 16)
                    break;
            }
        }

        if ((tx_buf[3] & 0xc0) == 0xc0) {
            int e_len = aes256_cbc_encrypt(tx_buf + 4, len, tx_buf + 4);
            if (e_len < 16) {
                ESP_LOGE(tag, "encrypt err");
                continue;
            }
            len = e_len;
        }

        uint8_t *dat = tx_buf + 4;
        bool big_mtu = csa.ble_mtu_cur >= 498;
        if (fragment && !(tx_buf[3] & 0x80) && len > 495)
            tx_buf[3] = 0x80;

        while (true) {
            uint8_t *p = dat;
            uint16_t limit = big_mtu ? 495 : 244;
            if (tx_buf[3] & 0x80)
                limit--;
            uint16_t l = min(limit, len);
            if (!l)
                break;
            dat += l;
            len -= l;
            if (tx_buf[3] & 0x80) {
                p--;
                l++;
                *p = tx_buf[3] & 0b11100000;
                if (p != tx_buf + 3 || len) {
                    if (p == tx_buf + 3)
                        *p |= (frag_cnt++ & 7) | 0x08;
                    else if (len)
                        *p |= (frag_cnt++ & 7) | 0x10;
                    else
                        *p |= (frag_cnt++ & 7) | 0x18;
                }
            }
            do {
                om = ble_hs_mbuf_from_flat(p, l);
                if (om == NULL) {
                    ESP_LOGE(tag, "no mbuf available from pool, retry..");
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                }
            } while (om == NULL);


            int rc = ble_gatts_notify_custom(ble_conn_handle, ble_notify_handle, om);
            if (rc != 0) {
                ESP_LOGE(tag, "error while sending notification; rc = %d", rc);
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
        }
    }
}

static int gatts_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_LINK_ESTAB:
        ESP_LOGI(tag, "connection %s; status = %d ",
                 event->connect.status == 0 ? "established" : "failed", event->connect.status);
        rc = ble_att_set_preferred_mtu(PREFERRED_MTU_VALUE);
        if (rc != 0)
            ESP_LOGE(tag, "failed to set preferred mtu; rc = %d", rc);

        if (event->connect.status != 0) {
            gatts_advertise(); // connection failed; resume advertising
        } else {
            csa.ble_connect = true;
            csa.t_ble_connect = get_systick();
            csa.k_st_ble = !(csa.k_en & 1);
        }

        ble_conn_handle = event->connect.conn_handle;
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(tag, "disconnect; reason = %d", event->disconnect.reason);
        csa.ble_connect = false;
        csa.k_st_ble = false;
        csa.ble_mtu_cur = 0;
        csa.ble_itvl_cur = 0;
        if (!csa.ble_stop)
            gatts_advertise(); // connection terminated; resume advertising
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(tag, "connection updated; status=%d ", event->conn_update.status);
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        assert(rc == 0);
        bleprph_print_conn_desc(&desc);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(tag, "adv complete ");
        gatts_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(tag, "subscribe event; cur_notify=%d; value handle; val_handle = %d",
                 event->subscribe.cur_notify, event->subscribe.attr_handle);
        notify_state = event->subscribe.cur_notify;
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        //ESP_LOGI(tag, "BLE_GAP_EVENT_NOTIFY_TX notify tx status = %d", event->notify_tx.status);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(tag, "mtu update event; conn_handle = %d mtu = %d ", event->mtu.conn_handle, event->mtu.value);
        csa.ble_mtu_cur = event->mtu.value;
        break;
    }
    return 0;
}

static void gatts_on_sync(void)
{
    int rc;
    uint8_t addr_val[6] = {0};

    rc = ble_hs_id_infer_auto(0, &gatts_addr_type);
    assert(rc == 0);
    rc = ble_hs_id_copy_addr(gatts_addr_type, addr_val, NULL);
    assert(rc == 0);
    ESP_LOGI(tag, "device address: ");
    print_addr(addr_val);
    sprintf(device_name, BLE_NAME " %02X%02X", addr_val[1], addr_val[0]);
    for (int i = 0; i < 6; i++)
        mfg_data[i+2] = addr_val[5-i];
    sprintf(cpu_id, "%02x%02x%02x%02x%02x%02x",
            addr_val[5], addr_val[4], addr_val[3], addr_val[2], addr_val[1], addr_val[0]);

    rc = ble_gap_set_prefered_default_le_phy(BLE_HCI_LE_PHY_2M_PREF_MASK, BLE_HCI_LE_PHY_2M_PREF_MASK);
    if (rc == 0)
        ESP_LOGI(tag, "default le phy set successfully");
    else
        ESP_LOGI(tag, "failed to set default le phy");

    gatts_advertise();
}

static void gatts_on_reset(int reason)
{
    ESP_LOGE(tag, "resetting state; reason=%d", reason);
}

void gatts_host_task(void *param)
{
    ESP_LOGI(tag, "ble host task started");
    nimble_port_run(); // this function will return only when nimble_port_stop() is executed
    nimble_port_freertos_deinit();
}

void ble_maintain_task(void)
{
    static bool ble_stop_bk = false;
    static bool update_params = false;
    static uint8_t itvl_min_bk = 6;
    static uint8_t itvl_max_bk = 12;

    csa.ble_itvl_min = max(6, csa.ble_itvl_min); // itvl units 1.25ms
    csa.ble_itvl_max = max(csa.ble_itvl_min, csa.ble_itvl_max);
    struct ble_gap_upd_params conn_params = {
        .itvl_min = csa.ble_itvl_min,
        .itvl_max = csa.ble_itvl_max,
        .min_ce_len = csa.ble_itvl_min * 2, // ce_len units 0.625ms
        .max_ce_len = csa.ble_itvl_max * 2,
        .latency = 0,
        .supervision_timeout = 500,         // units 10ms
    };

    if (ble_stop_bk != csa.ble_stop) {
        ESP_LOGI(tag, "ble_stop val: %d -> %d (connect: %d)", ble_stop_bk, csa.ble_stop, csa.ble_connect);
        if (!csa.ble_connect) {
            if (csa.ble_stop) {
                int rc = ble_gap_adv_stop();
                if (rc == 0)
                    ESP_LOGI(tag, "advertising stopped successfully");
                else
                    ESP_LOGE(tag, "failed to stop advertising: %d", rc);
            } else {
                gatts_advertise();
            }
        }
        ble_stop_bk = csa.ble_stop;
        return;
    }

    if (!csa.ble_connect) {
        update_params = false;
    } else if (!update_params) {
        if (get_systick() - csa.t_ble_connect > 1500) { // delay before updating params for compatibility
            update_params = true;
            ESP_LOGI(tag, "update params ...");
            int rc = ble_hs_hci_util_set_data_len(ble_conn_handle, LL_PACKET_LENGTH, LL_PACKET_TIME);
            if (rc != 0)
                ESP_LOGE(tag, "set packet length failed");
            rc = ble_gap_update_params(ble_conn_handle, &conn_params);
            if (rc != 0)
                ESP_LOGE(tag, "failed to update params, rc = %d !", rc);
            itvl_min_bk = csa.ble_itvl_min;
            itvl_max_bk = csa.ble_itvl_max;
        }
    } else if (itvl_min_bk != conn_params.itvl_min || itvl_max_bk != conn_params.itvl_max) {
        ESP_LOGI(tag, "update params ....");
        int rc = ble_gap_update_params(ble_conn_handle, &conn_params);
        if (rc != 0)
            ESP_LOGE(tag, "failed to update params, rc = %d !", rc);
        itvl_min_bk = csa.ble_itvl_min;
        itvl_max_bk = csa.ble_itvl_max;
    }

    if (!csa.k_st_ble) {
        if (csa.ble_connect && (uint32_t)(get_systick() - csa.t_ble_connect) >= 8000) {
            ESP_LOGI(tag, "key verify timeout\n");
            ble_gap_terminate(ble_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            csa.t_ble_connect = get_systick();
        }
    }
}

void cd_ble_main(void)
{
    ble_notify_queue = xQueueCreate(50, sizeof(void *));
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "failed to init nimble %d ", ret);
        return;
    }

    ble_hs_cfg.sync_cb = gatts_on_sync;
    ble_hs_cfg.reset_cb = gatts_on_reset;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb,
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    xTaskCreate(notify_task, "notify_task", 4096, NULL, 16, NULL);

    int rc = gatt_svr_init();
    assert(rc == 0);
    rc = ble_svc_gap_device_name_set(device_name);
    assert(rc == 0);
    nimble_port_freertos_init(gatts_host_task);
}
