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

static const char *tag = "cd-wifi";
static bool wifi_connect = false;
static bool v6_on = false;

#define PORT 0xcdcd
int udp_sock = -1;
struct sockaddr_storage udp_src_addr; // large enough for both ipv4 or ipv6
bool udp_src_addr_valid = false;
static uint8_t rx_buf[4+1500+16] __attribute__((aligned(4))) = {0};
static uint8_t tx_buf[4+1500+16] __attribute__((aligned(4))) = {0};

QueueHandle_t udp_notify_queue = NULL;


static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(tag, "event: sta start");
        csa.wifi_state = 0;

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(tag, "event: sta disconnected");
        csa.wifi_state_.connected = 0;
        csa.wifi_state_.connecting = 0;
        wifi_connect = false;
        udp_src_addr_valid = false;
        v6_on = false;
        memset(csa.local_ip, 0xff, sizeof(csa.local_ip));

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(tag, "event: sta connected");
        csa.wifi_state_.connected = 1;
        csa.wifi_state_.connecting = 0;
        udp_src_addr_valid = false;
        v6_on = false;
        memset(csa.local_ip, 0xff, sizeof(csa.local_ip));

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(tag, "event: got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        memset(csa.local_ip[0], 0, 16);
        memset(csa.local_ip[0] + 10, 0xff, 2);
        memcpy(csa.local_ip[0] + 12, &event->ip_info.ip, 4);

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t* event = (ip_event_got_ip6_t*) event_data;
        ESP_LOGI(tag, "event: got ip6:" IPV6STR, IPV62STR(event->ip6_info.ip));
        for (int i = 1; i < 4; i++) {
            if (get_unaligned16(csa.local_ip[i]) == 0xffff) {
                memcpy(csa.local_ip[i], &event->ip6_info.ip, 16);
                break;
            }
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGI(tag, "event: lost ip");

    } else {
        ESP_LOGI(tag, "event: unkown!");
    }
}


static void fast_scan(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &event_handler, NULL, NULL));

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}


static void udp_server_task(void *arg)
{
    static char addr_str[128];
    static uint8_t src_ip[16];
    struct sockaddr_in6 ser_addr;
    bool skip_err_rpt = false;

    while (true) {
        bzero(&ser_addr.sin6_addr.un, sizeof(ser_addr.sin6_addr.un));
        ser_addr.sin6_family = AF_INET6;
        ser_addr.sin6_port = htons(PORT);

        udp_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
        if (udp_sock < 0) {
            ESP_LOGE(tag, "unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(tag, "socket created");

        int err = bind(udp_sock, (struct sockaddr *)&ser_addr, sizeof(ser_addr));
        if (err < 0) {
            ESP_LOGE(tag, "socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(tag, "socket bound, port %d", PORT);

        struct sockaddr_storage source_addr; // large enough for both ipv4 or ipv6

        while (true) {
            //ESP_LOGI(tag, "waiting for data");
            socklen_t socklen = sizeof(source_addr); // recvfrom may shrink it, reset every time
            int len = recvfrom(udp_sock, rx_buf + 3, sizeof(rx_buf) - 3, 0, (struct sockaddr *)&source_addr, &socklen);
            if (len < 0) {
                ESP_LOGE(tag, "recvfrom failed: errno %d", errno);
                break;
            } else {
                uint16_t src_port = 0xffff;
                if (source_addr.ss_family == PF_INET) {
                    struct sockaddr_in *a = (struct sockaddr_in *)&source_addr;
                    inet_ntoa_r(a->sin_addr, addr_str, sizeof(addr_str) - 1);
                    memset(src_ip, 0, 16);
                    memset(src_ip + 10, 0xff, 2);
                    memcpy(src_ip + 12, &a->sin_addr, 4);
                    src_port = a->sin_port;
                } else if (source_addr.ss_family == PF_INET6) {
                    struct sockaddr_in6 *a = (struct sockaddr_in6 *)&source_addr;
                    inet6_ntoa_r(a->sin6_addr, addr_str, sizeof(addr_str) - 1);
                    memcpy(src_ip, &a->sin6_addr, 16);
                    src_port = a->sin6_port;
                }
                //ESP_LOGI(tag, "received %d bytes from %s :", len, addr_str);

                uint8_t w_hdr;
                uint8_t tgt_mac;
                uint8_t *cdn_buf;
                uint16_t cdn_len;
                uint8_t err_code = 0; // 1: frag err, 2: aes err

                if (len < 2)
                    continue;

                if ((rx_buf[3] & 0x80) == 0) { // no w_hdr
                    w_hdr = 0;
                    cdn_len = len;
                    cdn_buf = rx_buf + 3;

                } else if ((rx_buf[3] & 0x18) == 0) { // w_hdr, no fragment
                    w_hdr = rx_buf[3];
                    cdn_len = len - 1;
                    cdn_buf = rx_buf + 4;
                    if (w_hdr & 0x40) {
                        int plain_len = aes256_cbc_decrypt(cdn_buf, cdn_len, cdn_buf);
                        if (plain_len < 4 || get_unaligned16(cdn_buf) != csa.k_cnt_rx_udp) {
                            ESP_LOGE(tag, "plain_len: %d, rx_cnt: %04x != %04x, %d",
                                    plain_len, get_unaligned16(cdn_buf), csa.k_cnt_rx_udp, skip_err_rpt);
                            err_code = 2; // aes err
                            goto reply_err_code;
                        }
                        csa.k_cnt_rx_udp++;
                        cdn_len = plain_len - 2;
                        cdn_buf = rx_buf + 6; // tgt_mac or cdn_payload
                    }

                } else {
                    continue;
                }

                if ((w_hdr & 0x40) || !(csa.k_en & 2)) {
                    if (get_unaligned16(csa.remote_ip) == 0xffff || csa.remote_port == 0xffff) {
                        memcpy(csa.remote_ip, src_ip, 16);
                        csa.remote_port = src_port;
                        udp_src_addr_valid = true;
                        memcpy(&udp_src_addr, &source_addr, sizeof(source_addr));
                        ESP_LOGI(tag, "update csa remote_ip/port");
                    }
                }

                if ((w_hdr & 0x20) != 0) { // mac_flag
                    tgt_mac = *cdn_buf++;
                    cdn_len--;
                } else {
                    tgt_mac = (cdn_buf[0] & 0b11100000) == 0b01100000 ? csa.p_mac : bus_mac;
                }

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
                        memcpy(&frame->udp_addr, &source_addr, sizeof(source_addr));

                        cd_list_put(&udp_rx_head, frame);
                        if (dispatch_task_handle)
                            xTaskNotifyGive(dispatch_task_handle);
                        portYIELD();
                    } else {
                        ESP_LOGE(tag, "rx no free frame");
                        break;
                    }
                    sub_buf += sub_len;
                }
                continue;

reply_err_code:
                if (skip_err_rpt)
                    continue;
                skip_err_rpt = true;
                cd_frame_t *frame = cd_list_get(&frame_free_head);
                if (frame) {
                    frame->dat[0] = bus_mac;
                    frame->dat[1] = 0;
                    frame->dat[2] = 0;
                    frame->w_hdr = 0x80 | err_code;
                    memcpy(&frame->udp_addr, &source_addr, sizeof(source_addr));

                    BaseType_t ret = xQueueSend(udp_notify_queue, (void *) &frame, (TickType_t) 0);
                    //ESP_LOGI(tag, "reply err code: frame: %p\n", frame);
                    if (ret != pdPASS) {
                        ESP_LOGI(tag, "reply err code: enqueue err\n");
                        cd_list_put(&frame_free_head, frame);
                    }
                }
            }
        }

        if (udp_sock != -1) {
            ESP_LOGE(tag, "shutting down socket and restarting...");
            shutdown(udp_sock, 0);
            close(udp_sock);
        }
    }
    vTaskDelete(NULL);
}


static void udp_notify_task(void *arg)
{
    cd_frame_t *frame;
    list_head_t head_in = {0};
    list_head_t head_left = {0};

    while (true) {
        while (head_left.len) {
            frame = cd_list_get(&head_left);
            cd_list_put(&head_in, frame);
        }
        frame = cd_list_get(&head_in);
        if (!frame)
            xQueueReceive(udp_notify_queue, &frame, portMAX_DELAY);
        //ESP_LOGI(tag, "reply udp cmd (len %d): frame: %p\n", frame->dat[2], frame);
        if (udp_sock < 0) {
            cd_list_put(&frame_free_head, frame);
            continue;
        }

        uint8_t shift = 0;
        uint8_t mac = frame->dat[0];
        if (frame->w_hdr & 0x80) {
            if (frame->w_hdr & 0x40) {
                shift += 2;
                put_unaligned16(csa.k_cnt_tx_udp++, tx_buf + 4);
            }
            if (frame->w_hdr & 0x20) {
                tx_buf[4 + shift] = frame->dat[0];
                shift++;
            }
        }
        tx_buf[3] = frame->w_hdr;
        memcpy(tx_buf + 4 + shift, frame->dat + 3, frame->dat[2]);
        uint16_t len = frame->dat[2] + shift;
        struct sockaddr_storage udp_addr = {0};
        memcpy(&udp_addr, &frame->udp_addr, sizeof(frame->udp_addr));
        cd_list_put(&frame_free_head, frame);

        // if cdnet_len == 253 && has new frame without timeout && w_hdr and src_mac same
        if (len - shift == 253) {
            uint8_t pkt_num = 1;
            while (true) {
                frame = cd_list_get(&head_in);
                if (!frame)
                    xQueueReceive(udp_notify_queue, &frame, 50 / portTICK_PERIOD_MS);
                if (!frame)
                    break;
                if (frame->w_hdr != tx_buf[3] || mac != frame->dat[0]
                        || memcmp(&frame->udp_addr, &udp_addr, sizeof(udp_addr))) {
                    cd_list_put(&head_left, frame);
                    continue;
                }
                uint8_t l = frame->dat[2];
                memcpy(tx_buf + 4 + len, frame->dat + 3, l);
                len += l;
                cd_list_put(&frame_free_head, frame);
                pkt_num++;
                if (l != 253 || pkt_num == 5)
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

        uint8_t *dat = (tx_buf[3] & 0x80) ? tx_buf + 3 : tx_buf + 4;
        if (tx_buf[3] & 0x80)
            len++;

        if (udp_sock >= 0) {
            int err = sendto(udp_sock, dat, len, 0, (struct sockaddr *)&udp_addr, sizeof(udp_addr));
            if (err < 0)
                ESP_LOGE(tag, "udp sendto: errno %d", errno);
        }
    }
}


void wifi_maintain_task(void)
{
    static wifi_ap_record_t ap_info[20];

    if (csa.wifi_state_.disabled)
        return;

    if (csa.scan_start && !csa.wifi_state_.connecting) {
        csa.wifi_state_.scan = 1;
        csa.scan_start = 0;
        memset(csa.scan_rssi, 127, sizeof(csa.scan_rssi));
        uint16_t number = 20;
        uint16_t ap_count = 0;
        memset(ap_info, 0, sizeof(ap_info));
        esp_wifi_scan_start(NULL, true);

        ESP_LOGI(tag, "max ap number ap_info can hold = %u", number);
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
        ESP_LOGI(tag, "total ap scanned = %u, actual ap number ap_info holds = %u", ap_count, number);
        for (int i = 0; i < number; i++) {
            //ESP_LOGI(tag, "RSSI %d\t %s (auth %d ch %d)",
            //        ap_info[i].rssi, ap_info[i].ssid, ap_info[i].authmode, ap_info[i].primary);
            memcpy(csa.scan_ssid[i], ap_info[i].ssid, 32);
            csa.scan_auth[i] = ap_info[i].authmode;
            csa.scan_rssi[i] = ap_info[i].rssi;
        }
        csa.wifi_state_.scan = 0;
    }

    if (csa.wifi_conf == 1 && !wifi_connect) {
        ESP_LOGI(tag, "wifi connecting by wifi_conf");
        wifi_config_t wifi_config = {
            .sta = {
                .scan_method = WIFI_FAST_SCAN,
                .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
                .threshold.rssi = -127,
                .threshold.authmode = WIFI_AUTH_OPEN,
                .threshold.rssi_5g_adjustment = 0,
            },
        };
        memcpy(wifi_config.sta.ssid, csa.wifi_ssid, 32);
        memcpy(wifi_config.sta.password, csa.wifi_pwd, 64);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        wifi_connect = true;
        csa.wifi_state_.connecting = 1;
        esp_wifi_connect();
    }

    if (csa.wifi_conf == 0 && wifi_connect) {
        ESP_LOGI(tag, "wifi disconnect by !wifi_conf");
        wifi_connect = false;
        csa.wifi_state_.connecting = 0;
        esp_wifi_disconnect();
    }

    if (!v6_on && csa.wifi_state_.connected) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            ESP_LOGI(tag, "try v6 on");
            esp_err_t err = esp_netif_create_ip6_linklocal(netif);
            if (err != ESP_OK) {
                ESP_LOGE(tag, "failed to create link-local ipv6 address: %s", esp_err_to_name(err));
            } else {
                v6_on = true;
            }
        }
    }
}


void wifi_main(void)
{
    udp_notify_queue = xQueueCreate(50, sizeof(void *));
    fast_scan();
    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 18, NULL);
    xTaskCreate(udp_notify_task, "udp_notify_task", 4096, NULL, 18, NULL);
    ESP_ERROR_CHECK(esp_coex_preference_set(ESP_COEX_PREFER_WIFI));
}
