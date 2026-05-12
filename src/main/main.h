/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2025, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <d@d-l.io>
 */

#ifndef __MAIN_H__
#define __MAIN_H__

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "hal/cpu_hal.h"
#include "nvs_flash.h"
#include "esp_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_coexist.h"
#include "freertos/FreeRTOSConfig.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "../src/ble_hs_hci_priv.h"

#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#include "psa/crypto.h"

#ifndef BLE_NAME
#define BLE_NAME        "CD-ESP"
#endif

#define LED_W_PIN       9
#define LED_G_PIN       8
#define BUTTON_PIN      20
#define BUZZER_PIN      5
#define MCO_PIN         3

#define CDCTL_MISO_PIN  2 // Q
#define CDCTL_MOSI_PIN  7 // D
#define CDCTL_SCK_PIN   6
#define CDCTL_CS_PIN    10
#define CDCTL_INT_PIN   4


extern TaskHandle_t dispatch_task_handle;
extern QueueHandle_t udp_notify_queue;
extern QueueHandle_t ble_notify_queue;
extern uint16_t ble_conn_handle;
extern uint16_t ble_notify_handle;

extern struct sockaddr_storage udp_src_addr;
extern bool udp_src_addr_valid;

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);
int gatt_svr_init(void);

void ble_maintain_task(void);
void wifi_maintain_task(void);
void cd_main_maintain_task(void);
void cd_ble_main(void);
void wifi_main(void);
void mdns_init(void);

int aes256_cbc_encrypt(uint8_t *input, size_t in_len, uint8_t *output);
int aes256_cbc_decrypt(const uint8_t *input, size_t in_len, uint8_t *output);

#endif
