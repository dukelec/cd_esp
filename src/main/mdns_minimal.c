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

#define TAG "mdns"

#define MDNS_PORT       5353
#define SERVICE_PORT    0xcdcd
#define HOSTNAME        "cd-esp"
#define INSTANCE_NAME   "CD-ESP"
#define SERVICE_TYPE    "_cd-esp._udp.local"
#define SERVICE_ENUM    "_services._dns-sd._udp.local"


static int dns_write_name(uint8_t *buf, int off, const char *name)
{
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int len = dot ? (dot - p) : strlen(p);
        buf[off++] = len;
        memcpy(&buf[off], p, len);
        off += len;
        if (!dot)
            break;
        p = dot + 1;
    }
    buf[off++] = 0;
    return off;
}

// parse the first question of a received packet; reads qname and returns qtype,
// or -1 if the packet is too short / malformed. every index is bounded by len.
static int dns_parse_query(const uint8_t *buf, int len, char *out, int outlen)
{
    if (len < 12)
        return -1;

    int pos = 0;
    int off = 12; // skip dns header
    while (off < len && buf[off]) {
        int l = buf[off++];
        if (off + l > len || pos + l + 1 >= outlen)
            return -1;
        memcpy(out + pos, &buf[off], l);
        pos += l;
        out[pos++] = '.';
        off += l;
    }
    if (off >= len) // no root label within the packet
        return -1;
    if (pos)
        out[pos - 1] = 0;
    else
        out[0] = 0;
    off++; // skip the zero-length root label

    if (off + 2 > len) // need 2 bytes of qtype
        return -1;
    return (buf[off] << 8) | buf[off + 1];
}


static void mdns_send_response(int sock, int is_ipv6, const char *qname, uint16_t qtype)
{
    uint8_t pkt[512];
    memset(pkt, 0, sizeof(pkt));

    // dns header
    pkt[2] = 0x84; // response + authoritative

    int off = 12;
    int ancount = 4; // srv + txt + a + aaaa; ptr added below when matched

    // ptr _services._dns-sd._udp.local
    if (qtype == 12 && strcmp(qname, SERVICE_ENUM) == 0) {
        ancount++;
        off = dns_write_name(pkt, off, SERVICE_ENUM);
        pkt[off++] = 0x00; pkt[off++] = 0x0c; // ptr
        pkt[off++] = 0x00; pkt[off++] = 0x01; // IN
        pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x3c; // ttl
        int rdlen = off; off += 2;
        int start = off;
        off = dns_write_name(pkt, off, SERVICE_TYPE);
        pkt[rdlen]     = (off - start) >> 8;
        pkt[rdlen + 1] = (off - start) & 0xff;
    }

    // ptr _cd-esp._udp.local
    if (qtype == 12 && strcmp(qname, SERVICE_TYPE) == 0) {
        ancount++;
        off = dns_write_name(pkt, off, SERVICE_TYPE);
        pkt[off++] = 0x00; pkt[off++] = 0x0c; // ptr
        pkt[off++] = 0x00; pkt[off++] = 0x01;
        pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x3c;
        int rdlen = off; off += 2;
        int start = off;
        off = dns_write_name(pkt, off, INSTANCE_NAME "." SERVICE_TYPE);
        pkt[rdlen]     = (off - start) >> 8;
        pkt[rdlen + 1] = (off - start) & 0xff;
    }

    // srv
    off = dns_write_name(pkt, off, INSTANCE_NAME "." SERVICE_TYPE);
    pkt[off++] = 0x00; pkt[off++] = 0x21; // srv
    pkt[off++] = 0x00; pkt[off++] = 0x01; // in
    pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x3c; // ttl
    int rdlen = off; off += 2;
    int start = off;
    pkt[off++] = 0x00; pkt[off++] = 0x00; // priority
    pkt[off++] = 0x00; pkt[off++] = 0x00; // weight
    pkt[off++] = SERVICE_PORT >> 8;
    pkt[off++] = SERVICE_PORT & 0xff;
    off = dns_write_name(pkt, off, HOSTNAME ".local");
    pkt[rdlen]     = (off - start) >> 8;
    pkt[rdlen + 1] = (off - start) & 0xff;

    // txt (empty)
    off = dns_write_name(pkt, off, INSTANCE_NAME "." SERVICE_TYPE);
    pkt[off++] = 0x00; pkt[off++] = 0x10; // txt
    pkt[off++] = 0x00; pkt[off++] = 0x01;
    pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x3c; // ttl
    pkt[off++] = 0x00; pkt[off++] = 0x01; // rdlength
    pkt[off++] = 0x00; // empty

    // a
    off = dns_write_name(pkt, off, HOSTNAME ".local");
    pkt[off++] = 0x00; pkt[off++] = 0x01; // a
    pkt[off++] = 0x00; pkt[off++] = 0x01; // in
    pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x3c; // ttl
    pkt[off++] = 0x00; pkt[off++] = 0x04; // rdlen
    memcpy(&pkt[off], csa.local_ip[0] + 12, 4);
    off += 4;

    // aaaa
    off = dns_write_name(pkt, off, HOSTNAME ".local");
    pkt[off++] = 0x00; pkt[off++] = 0x1c; // aaaa
    pkt[off++] = 0x00; pkt[off++] = 0x01; // in
    pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x00; pkt[off++] = 0x3c; // ttl
    pkt[off++] = 0x00; pkt[off++] = 0x10; // rdlen = 16
    memcpy(&pkt[off], csa.local_ip[1], 16);
    off += 16;

    pkt[7] = ancount; // actual answer count (4 or 5)

    // multicast send
    if (is_ipv6) {
        struct sockaddr_in6 to6 = {
            .sin6_family = AF_INET6,
            .sin6_port   = htons(MDNS_PORT),
        };
        inet_pton(AF_INET6, "ff02::fb", &to6.sin6_addr);
        sendto(sock, pkt, off, 0, (struct sockaddr *)&to6, sizeof(to6));
    } else {
        struct sockaddr_in to4 = {
            .sin_family = AF_INET,
            .sin_port   = htons(MDNS_PORT),
            .sin_addr.s_addr = inet_addr("224.0.0.251"),
        };
        sendto(sock, pkt, off, 0, (struct sockaddr *)&to4, sizeof(to4));
    }
}


static void mdns_server_task(void *arg)
{
    static uint8_t rx_buf[512];

    while (true) {
        int sock6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        int sock4 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (sock6 < 0 || sock4 < 0) {
            ESP_LOGE(TAG, "socket create failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int on = 1;
        setsockopt(sock6, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(sock4, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_in6 addr6 = {
            .sin6_family = AF_INET6,
            .sin6_port   = htons(MDNS_PORT),
            .sin6_addr   = IN6ADDR_ANY_INIT,
        };
        bind(sock6, (struct sockaddr *)&addr6, sizeof(addr6));
        struct ipv6_mreq mreq6;
        inet_pton(AF_INET6, "ff02::fb", &mreq6.ipv6mr_multiaddr);
        mreq6.ipv6mr_interface = 0;
        setsockopt(sock6, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6));

        struct sockaddr_in addr4 = {
            .sin_family = AF_INET,
            .sin_port   = htons(MDNS_PORT),
            .sin_addr.s_addr = INADDR_ANY,
        };
        bind(sock4, (struct sockaddr *)&addr4, sizeof(addr4));
        struct ip_mreq mreq4 = {0};
        mreq4.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
        mreq4.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(sock4, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq4, sizeof(mreq4));

        ESP_LOGI(TAG, "mdns sockets bound");

        while (true) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock4, &readfds);
            FD_SET(sock6, &readfds);
            int maxfd = sock6 > sock4 ? sock6 : sock4;

            struct timeval tv = {0, 20000}; // 20ms
            int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
            if (ret <= 0)
                continue;

            if (FD_ISSET(sock4, &readfds)) {
                struct sockaddr_in src4;
                socklen_t slen = sizeof(src4);
                int len = recvfrom(sock4, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&src4, &slen);
                //ESP_LOGI(TAG, "mdns v4 ...");
                if (len <= 0)
                    break;

                char qname[128];
                int qtype = dns_parse_query(rx_buf, len, qname, sizeof(qname));
                if (qtype < 0)
                    continue;

                mdns_send_response(sock4, 0, qname, qtype);
            }

            if (FD_ISSET(sock6, &readfds)) {
                struct sockaddr_in6 src6;
                socklen_t slen = sizeof(src6);
                int len = recvfrom(sock6, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&src6, &slen);
                //ESP_LOGI(TAG, "mdns v6 ...");
                if (len <= 0)
                    break;

                char qname[128];
                int qtype = dns_parse_query(rx_buf, len, qname, sizeof(qname));
                if (qtype < 0)
                    continue;

                mdns_send_response(sock6, 1, qname, qtype);
            }
        }

        close(sock4);
        close(sock6);
        ESP_LOGI(TAG, "sockets closed, restarting...");
    }

    vTaskDelete(NULL);
}


void mdns_init(void)
{
    xTaskCreate(mdns_server_task, "mdns_server", 4096, NULL, 18, NULL);
    ESP_LOGI(TAG, "mdns_init done");
}

