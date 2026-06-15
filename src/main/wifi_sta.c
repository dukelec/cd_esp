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
#include <netinet/tcp.h>
#include <stdlib.h>
#include "freertos/semphr.h"

static const char *tag = "cd-wifi";
static bool wifi_connect = false;
static bool v6_on = false;

#define PORT 0xcdcd
#define TCP_BUF_SIZE (4 + 3 + 253 * 16 + 16)
#if !CD_DISABLE_UDP
int udp_sock = -1;
struct sockaddr_storage udp_src_addr; // large enough for both ipv4 or ipv6
bool udp_src_addr_valid = false;
static uint8_t rx_buf[4+1500+16] __attribute__((aligned(4))) = {0};
static uint8_t tx_buf[4+1500+16] __attribute__((aligned(4))) = {0};

QueueHandle_t udp_notify_queue = NULL;
#endif
QueueHandle_t tcp_notify_queue = NULL;

static int tcp_listen_sock = -1;
static int tcp_client_sock = -1;
bool tcp_src_addr_valid = false;
static uint8_t tcp_src_ip[16] = {0};
static uint16_t tcp_src_port = 0xffff;
static SemaphoreHandle_t tcp_sock_mutex = NULL;
static TaskHandle_t tcp_rx_task_handle = NULL;
static uint8_t tcp_rx_buf[TCP_BUF_SIZE] __attribute__((aligned(4))) = {0};
static uint8_t tcp_tx_buf[TCP_BUF_SIZE] __attribute__((aligned(4))) = {0};

static void tcp_close_client(void)
{
    if (tcp_sock_mutex)
        xSemaphoreTake(tcp_sock_mutex, portMAX_DELAY);

    int sock = tcp_client_sock;
    tcp_client_sock = -1;
    tcp_src_addr_valid = false;

    if (tcp_sock_mutex)
        xSemaphoreGive(tcp_sock_mutex);

    if (sock >= 0) {
        csa.ble_stop = false;
        shutdown(sock, 0);
        close(sock);
    }
}

static int recv_all(int sock, uint8_t *buf, int len)
{
    int off = 0;
    while (off < len) {
        int r = recv(sock, (char *)buf + off, len - off, 0);
        if (r <= 0)
            return -1;
        off += r;
    }
    return 0;
}


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
#if !CD_DISABLE_UDP
        udp_src_addr_valid = false;
#endif
        tcp_close_client();
        v6_on = false;
        memset(csa.local_ip, 0xff, sizeof(csa.local_ip));

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(tag, "event: sta connected");
        csa.wifi_state_.connected = 1;
        csa.wifi_state_.connecting = 0;
#if !CD_DISABLE_UDP
        udp_src_addr_valid = false;
#endif
        tcp_close_client();
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


#if !CD_DISABLE_UDP
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
        socklen_t socklen = sizeof(source_addr);

        while (true) {
            //ESP_LOGI(tag, "waiting for data");
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
#endif

static void tcp_server_task(void *arg)
{
    static char addr_str[128];
    struct sockaddr_in6 ser_addr;

    while (true) {
        bzero(&ser_addr.sin6_addr.un, sizeof(ser_addr.sin6_addr.un));
        ser_addr.sin6_family = AF_INET6;
        ser_addr.sin6_port = htons(PORT);

        tcp_listen_sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_IPV6);
        if (tcp_listen_sock < 0) {
            ESP_LOGE(tag, "unable to create tcp socket: errno %d", errno);
            break;
        }

        int on = 1;
        setsockopt(tcp_listen_sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        int err = bind(tcp_listen_sock, (struct sockaddr *)&ser_addr, sizeof(ser_addr));
        if (err < 0) {
            ESP_LOGE(tag, "tcp socket unable to bind: errno %d", errno);
        }

        err = listen(tcp_listen_sock, 1);
        if (err < 0) {
            ESP_LOGE(tag, "tcp listen failed: errno %d", errno);
            break;
        }
        ESP_LOGI(tag, "tcp listen, port %d", PORT);

        while (true) {
            struct sockaddr_storage source_addr = {0};
            socklen_t socklen = sizeof(source_addr);
            int new_sock = accept(tcp_listen_sock, (struct sockaddr *)&source_addr, &socklen);
            if (new_sock < 0) {
                ESP_LOGE(tag, "tcp accept failed: errno %d", errno);
                break;
            }

            int nodelay = 1;
            setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

            uint8_t src_ip[16] = {0};
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

            if (tcp_sock_mutex)
                xSemaphoreTake(tcp_sock_mutex, portMAX_DELAY);
            bool busy = (tcp_client_sock >= 0);
            if (!busy) {
                tcp_client_sock = new_sock;
                tcp_src_addr_valid = true;
                memcpy(tcp_src_ip, src_ip, 16);
                tcp_src_port = src_port;
                csa.ble_stop = true;
            }
            if (tcp_sock_mutex)
                xSemaphoreGive(tcp_sock_mutex);

            if (busy) {
                ESP_LOGW(tag, "tcp busy, reject client: %s", addr_str);
                uint8_t dat = 0x80 | 3;
                uint16_t out_len = 1;
                uint16_t out_net_len = htons(out_len);
                send(new_sock, (void *)&out_net_len, 2, 0);
                send(new_sock, (void *)&dat, 1, 0);
                shutdown(new_sock, 0);
                close(new_sock);
                continue;
            }

            ESP_LOGI(tag, "tcp client connected: %s", addr_str);
            if (tcp_rx_task_handle)
                xTaskNotifyGive(tcp_rx_task_handle);
        }

        if (tcp_listen_sock != -1) {
            ESP_LOGE(tag, "tcp shutdown and restart...");
            shutdown(tcp_listen_sock, 0);
            close(tcp_listen_sock);
            tcp_listen_sock = -1;
        }
    }
    vTaskDelete(NULL);
}

static void tcp_rx_task(void *arg)
{
    bool skip_err_rpt = false;

    while (true) {
        if (tcp_sock_mutex)
            xSemaphoreTake(tcp_sock_mutex, portMAX_DELAY);
        int sock = tcp_client_sock;
        uint8_t src_ip[16];
        memcpy(src_ip, tcp_src_ip, 16);
        uint16_t src_port = tcp_src_port;
        if (tcp_sock_mutex)
            xSemaphoreGive(tcp_sock_mutex);

        if (sock < 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        while (true) {
            uint16_t net_len = 0;
            if (recv_all(sock, (uint8_t *)&net_len, 2) != 0)
                break;

            uint16_t len = ntohs(net_len);
            if (len == 0)
                continue;

            if (len <= TCP_BUF_SIZE - 3) {
                if (recv_all(sock, tcp_rx_buf + 3, len) != 0)
                    break;

                uint8_t w_hdr;
                uint8_t tgt_mac;
                uint8_t *cdn_buf;
                uint16_t cdn_len;
                uint8_t err_code = 0;

                if ((tcp_rx_buf[3] & 0x80) == 0) {
                    w_hdr = 0;
                    cdn_len = len;
                    cdn_buf = tcp_rx_buf + 3;
                } else if ((tcp_rx_buf[3] & 0x18) == 0) {
                    w_hdr = tcp_rx_buf[3];
                    cdn_len = len - 1;
                    cdn_buf = tcp_rx_buf + 4;
                    if (w_hdr & 0x40) {
                        int plain_len = aes256_cbc_decrypt(cdn_buf, cdn_len, cdn_buf);
                        if (plain_len < 4 || get_unaligned16(cdn_buf) != csa.k_cnt_rx_udp) {
                            ESP_LOGE(tag, "tcp plain_len: %d, rx_cnt: %04x != %04x, %d",
                                    plain_len, get_unaligned16(cdn_buf), csa.k_cnt_rx_udp, skip_err_rpt);
                            err_code = 2;
                            goto tcp_reply_err_code_small;
                        }
                        csa.k_cnt_rx_udp++;
                        cdn_len = plain_len - 2;
                        cdn_buf = tcp_rx_buf + 6;
                    }
                } else {
                    continue;
                }

                if ((w_hdr & 0x40) || !(csa.k_en & 2)) {
                    if (get_unaligned16(csa.remote_ip) == 0xffff || csa.remote_port == 0xffff) {
                        memcpy(csa.remote_ip, src_ip, 16);
                        csa.remote_port = src_port;
                        ESP_LOGI(tag, "update csa remote_ip/port (tcp)");
                    }
                }

                if ((w_hdr & 0x20) != 0) {
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
                        cd_list_put(&tcp_rx_head, frame);
                        xTaskNotifyGive(dispatch_task_handle);
                        portYIELD();
                    } else {
                        ESP_LOGE(tag, "tcp rx no free frame");
                        break;
                    }
                    sub_buf += sub_len;
                }
                continue;

tcp_reply_err_code_small:
                if (skip_err_rpt)
                    continue;
                skip_err_rpt = true;
                uint8_t dat = 0x80 | err_code;
                uint16_t out_len = 1;
                uint16_t out_net_len = htons(out_len);
                send(sock, (void *)&out_net_len, 2, 0);
                send(sock, (void *)&dat, 1, 0);
                continue;
            }

            uint16_t remain = len;
            uint8_t b0 = 0;
            if (recv_all(sock, &b0, 1) != 0)
                break;
            remain--;

            uint8_t w_hdr = 0;
            uint8_t tgt_mac = 0;
            uint8_t err_code = 0;

            if ((b0 & 0x80) == 0) {
                w_hdr = 0;
                uint8_t cdn_first = b0;

                if ((w_hdr & 0x40) || !(csa.k_en & 2)) {
                    if (get_unaligned16(csa.remote_ip) == 0xffff || csa.remote_port == 0xffff) {
                        memcpy(csa.remote_ip, src_ip, 16);
                        csa.remote_port = src_port;
                        ESP_LOGI(tag, "update csa remote_ip/port (tcp)");
                    }
                }

                tgt_mac = (cdn_first & 0b11100000) == 0b01100000 ? csa.p_mac : bus_mac;

                uint8_t buf_253[253];
                uint16_t buf_len = 0;
                buf_253[buf_len++] = cdn_first;

                bool drop = false;
                uint8_t tmp[512];
                while (remain) {
                    int want = min((int)sizeof(tmp), (int)remain);
                    int r = recv(sock, (char *)tmp, want, 0);
                    if (r <= 0)
                        goto tcp_stream_close;
                    remain -= r;

                    int off = 0;
                    while (off < r) {
                        int n = min((int)(253 - buf_len), r - off);
                        memcpy(buf_253 + buf_len, tmp + off, n);
                        buf_len += n;
                        off += n;

                        if (buf_len == 253) {
                            if (!drop) {
                                cd_frame_t *frame = cd_list_get(&frame_free_head);
                                if (frame) {
                                    frame->w_hdr = w_hdr & 0b11100000;
                                    frame->dat[0] = 0;
                                    frame->dat[1] = tgt_mac;
                                    frame->dat[2] = buf_len;
                                    memcpy(frame->dat + 3, buf_253, buf_len);
                                    cd_list_put(&tcp_rx_head, frame);
                                    xTaskNotifyGive(dispatch_task_handle);
                                    portYIELD();
                                } else {
                                    ESP_LOGE(tag, "tcp rx no free frame");
                                    drop = true;
                                }
                            }
                            buf_len = 0;
                        }
                    }
                }

                if (buf_len && !drop) {
                    cd_frame_t *frame = cd_list_get(&frame_free_head);
                    if (frame) {
                        frame->w_hdr = w_hdr & 0b11100000;
                        frame->dat[0] = 0;
                        frame->dat[1] = tgt_mac;
                        frame->dat[2] = buf_len;
                        memcpy(frame->dat + 3, buf_253, buf_len);
                        cd_list_put(&tcp_rx_head, frame);
                        xTaskNotifyGive(dispatch_task_handle);
                        portYIELD();
                    } else {
                        ESP_LOGE(tag, "tcp rx no free frame");
                    }
                }
                continue;
            }

            if ((b0 & 0x18) != 0) {
                err_code = 1;
                uint8_t tmp[512];
                while (remain) {
                    int want = min((int)sizeof(tmp), (int)remain);
                    int r = recv(sock, (char *)tmp, want, 0);
                    if (r <= 0)
                        goto tcp_stream_close;
                    remain -= r;
                }
                goto tcp_reply_err_code_stream;
            }

            w_hdr = b0;

            if (w_hdr & 0x40) {
                if (remain > 16384) {
                    err_code = 4;
                    uint8_t tmp[512];
                    while (remain) {
                        int want = min((int)sizeof(tmp), (int)remain);
                        int r = recv(sock, (char *)tmp, want, 0);
                        if (r <= 0)
                            goto tcp_stream_close;
                        remain -= r;
                    }
                    goto tcp_reply_err_code_stream;
                }

                uint8_t *dyn = malloc(len);
                if (!dyn) {
                    err_code = 4;
                    uint8_t tmp[512];
                    while (remain) {
                        int want = min((int)sizeof(tmp), (int)remain);
                        int r = recv(sock, (char *)tmp, want, 0);
                        if (r <= 0)
                            goto tcp_stream_close;
                        remain -= r;
                    }
                    goto tcp_reply_err_code_stream;
                }

                dyn[0] = w_hdr;
                if (recv_all(sock, dyn + 1, len - 1) != 0) {
                    free(dyn);
                    goto tcp_stream_close;
                }
                remain = 0;

                uint8_t *cdn_buf = dyn + 1;
                uint16_t cdn_len = len - 1;
                int plain_len = aes256_cbc_decrypt(cdn_buf, cdn_len, cdn_buf);
                if (plain_len < 4 || get_unaligned16(cdn_buf) != csa.k_cnt_rx_udp) {
                    ESP_LOGE(tag, "tcp plain_len: %d, rx_cnt: %04x != %04x, %d",
                            plain_len, get_unaligned16(cdn_buf), csa.k_cnt_rx_udp, skip_err_rpt);
                    err_code = 2;
                    free(dyn);
                    goto tcp_reply_err_code_stream;
                }
                csa.k_cnt_rx_udp++;
                cdn_len = plain_len - 2;
                cdn_buf += 2;

                if ((w_hdr & 0x40) || !(csa.k_en & 2)) {
                    if (get_unaligned16(csa.remote_ip) == 0xffff || csa.remote_port == 0xffff) {
                        memcpy(csa.remote_ip, src_ip, 16);
                        csa.remote_port = src_port;
                        ESP_LOGI(tag, "update csa remote_ip/port (tcp)");
                    }
                }

                if ((w_hdr & 0x20) != 0) {
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
                        cd_list_put(&tcp_rx_head, frame);
                        xTaskNotifyGive(dispatch_task_handle);
                        portYIELD();
                    } else {
                        ESP_LOGE(tag, "tcp rx no free frame");
                        break;
                    }
                    sub_buf += sub_len;
                }
                free(dyn);
                continue;
            }

            if ((w_hdr & 0x40) || !(csa.k_en & 2)) {
                if (get_unaligned16(csa.remote_ip) == 0xffff || csa.remote_port == 0xffff) {
                    memcpy(csa.remote_ip, src_ip, 16);
                    csa.remote_port = src_port;
                    ESP_LOGI(tag, "update csa remote_ip/port (tcp)");
                }
            }

            uint8_t buf_253[253];
            uint16_t buf_len = 0;
            bool drop = false;

            if ((w_hdr & 0x20) != 0) {
                if (recv_all(sock, &tgt_mac, 1) != 0)
                    goto tcp_stream_close;
                if (remain)
                    remain--;
            } else {
                uint8_t cdn_first = 0;
                if (recv_all(sock, &cdn_first, 1) != 0)
                    goto tcp_stream_close;
                if (remain)
                    remain--;
                tgt_mac = (cdn_first & 0b11100000) == 0b01100000 ? csa.p_mac : bus_mac;
                buf_253[buf_len++] = cdn_first;
            }

            uint8_t tmp[512];
            while (remain) {
                int want = min((int)sizeof(tmp), (int)remain);
                int r = recv(sock, (char *)tmp, want, 0);
                if (r <= 0)
                    goto tcp_stream_close;
                remain -= r;

                int off = 0;
                while (off < r) {
                    int n = min((int)(253 - buf_len), r - off);
                    memcpy(buf_253 + buf_len, tmp + off, n);
                    buf_len += n;
                    off += n;

                    if (buf_len == 253) {
                        if (!drop) {
                            cd_frame_t *frame = cd_list_get(&frame_free_head);
                            if (frame) {
                                frame->w_hdr = w_hdr & 0b11100000;
                                frame->dat[0] = 0;
                                frame->dat[1] = tgt_mac;
                                frame->dat[2] = buf_len;
                                memcpy(frame->dat + 3, buf_253, buf_len);
                                cd_list_put(&tcp_rx_head, frame);
                                xTaskNotifyGive(dispatch_task_handle);
                                portYIELD();
                            } else {
                                ESP_LOGE(tag, "tcp rx no free frame");
                                drop = true;
                            }
                        }
                        buf_len = 0;
                    }
                }
            }

            if (buf_len && !drop) {
                cd_frame_t *frame = cd_list_get(&frame_free_head);
                if (frame) {
                    frame->w_hdr = w_hdr & 0b11100000;
                    frame->dat[0] = 0;
                    frame->dat[1] = tgt_mac;
                    frame->dat[2] = buf_len;
                    memcpy(frame->dat + 3, buf_253, buf_len);
                    cd_list_put(&tcp_rx_head, frame);
                    xTaskNotifyGive(dispatch_task_handle);
                    portYIELD();
                } else {
                    ESP_LOGE(tag, "tcp rx no free frame");
                }
            }
            continue;

tcp_reply_err_code_stream:
            if (skip_err_rpt)
                continue;
            skip_err_rpt = true;
            uint8_t dat = 0x80 | err_code;
            uint16_t out_len = 1;
            uint16_t out_net_len = htons(out_len);
            send(sock, (void *)&out_net_len, 2, 0);
            send(sock, (void *)&dat, 1, 0);
        }

tcp_stream_close:
        ESP_LOGI(tag, "tcp client disconnected");
        tcp_close_client();
    }
}


#if !CD_DISABLE_UDP
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
                if (frame->w_hdr != tx_buf[3] || mac != frame->dat[0]) {
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
#endif

static void tcp_notify_task(void *arg)
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
            xQueueReceive(tcp_notify_queue, &frame, portMAX_DELAY);

        if (tcp_sock_mutex)
            xSemaphoreTake(tcp_sock_mutex, portMAX_DELAY);
        int sock = tcp_client_sock;
        if (tcp_sock_mutex)
            xSemaphoreGive(tcp_sock_mutex);

        if (sock < 0) {
            cd_list_put(&frame_free_head, frame);
            continue;
        }

        uint8_t shift = 0;
        uint8_t mac = frame->dat[0];
        if (frame->w_hdr & 0x80) {
            if (frame->w_hdr & 0x40) {
                shift += 2;
                put_unaligned16(csa.k_cnt_tx_udp++, tcp_tx_buf + 4);
            }
            if (frame->w_hdr & 0x20) {
                tcp_tx_buf[4 + shift] = frame->dat[0];
                shift++;
            }
        }
        tcp_tx_buf[3] = frame->w_hdr;
        memcpy(tcp_tx_buf + 4 + shift, frame->dat + 3, frame->dat[2]);
        uint16_t len = frame->dat[2] + shift;
        cd_list_put(&frame_free_head, frame);

        if (len - shift == 253) {
            uint8_t pkt_num = 1;
            while (true) {
                frame = cd_list_get(&head_in);
                if (!frame)
                    xQueueReceive(tcp_notify_queue, &frame, 50 / portTICK_PERIOD_MS);
                if (!frame)
                    break;
                if (frame->w_hdr != tcp_tx_buf[3] || mac != frame->dat[0]) {
                    cd_list_put(&head_left, frame);
                    continue;
                }
                uint8_t l = frame->dat[2];
                memcpy(tcp_tx_buf + 4 + len, frame->dat + 3, l);
                len += l;
                cd_list_put(&frame_free_head, frame);
                pkt_num++;
                if (l != 253 || pkt_num == 5)
                    break;
            }
        }

        if ((tcp_tx_buf[3] & 0xc0) == 0xc0) {
            int e_len = aes256_cbc_encrypt(tcp_tx_buf + 4, len, tcp_tx_buf + 4);
            if (e_len < 16) {
                ESP_LOGE(tag, "tcp encrypt err");
                continue;
            }
            len = e_len;
        }

        uint8_t *dat = (tcp_tx_buf[3] & 0x80) ? tcp_tx_buf + 3 : tcp_tx_buf + 4;
        if (tcp_tx_buf[3] & 0x80)
            len++;

        uint16_t net_len = htons(len);
        if (send(sock, (void *)&net_len, 2, 0) < 0)
            tcp_close_client();
        else if (send(sock, dat, len, 0) < 0)
            tcp_close_client();
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
#if !CD_DISABLE_UDP
    udp_notify_queue = xQueueCreate(50, sizeof(void *));
#endif
    tcp_notify_queue = xQueueCreate(50, sizeof(void *));
    tcp_sock_mutex = xSemaphoreCreateMutex();
    assert(tcp_sock_mutex);
    fast_scan();
#if !CD_DISABLE_UDP
    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 18, NULL);
    xTaskCreate(udp_notify_task, "udp_notify_task", 4096, NULL, 18, NULL);
#endif
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 18, NULL);
    xTaskCreate(tcp_rx_task, "tcp_rx", 4096, NULL, 18, &tcp_rx_task_handle);
    xTaskCreate(tcp_notify_task, "tcp_notify_task", 4096, NULL, 18, NULL);
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
}
