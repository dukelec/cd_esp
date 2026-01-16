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

static const char *tag = "cd-main";

static mbedtls_aes_context aes_enc_ctx;
static mbedtls_aes_context aes_dec_ctx;


static inline size_t pkcs7_pad(uint8_t *buf, size_t len)
{
    uint8_t pad_len = 16 - (len % 16);
    for (uint8_t i = 0; i < pad_len; i++)
        buf[len + i] = pad_len;
    return len + pad_len;
}

static inline size_t pkcs7_unpad(const uint8_t *buf, size_t len)
{
    uint8_t pad_len = buf[len - 1];
    return len - pad_len;
}

int aes256_cbc_encrypt(uint8_t *input, size_t in_len, uint8_t *output)
{
    uint8_t iv[16] = {0};
    size_t padded_len = pkcs7_pad(input, in_len); // input should large enough
    if (mbedtls_aes_crypt_cbc(&aes_enc_ctx, MBEDTLS_AES_ENCRYPT, padded_len, iv, input, output))
        return -1;
    return padded_len;
}

int aes256_cbc_decrypt(const uint8_t *input, size_t in_len, uint8_t *output)
{
    uint8_t iv[16] = {0};
    if (mbedtls_aes_crypt_cbc(&aes_dec_ctx, MBEDTLS_AES_DECRYPT, in_len, iv, input, output))
        return -1;
    return pkcs7_unpad(output, in_len);
}

static int aes256_cbc_init(const uint8_t *key)
{
    uint8_t key_len = 32;
    mbedtls_aes_init(&aes_enc_ctx);
    mbedtls_aes_init(&aes_dec_ctx);
    if (mbedtls_aes_setkey_enc(&aes_enc_ctx, key, key_len * 8))
        return -1;
    if (mbedtls_aes_setkey_dec(&aes_dec_ctx, key, key_len * 8))
        return -1;
    return 0;
}


static int sha256_sum(const uint8_t *data, size_t data_len, uint8_t *out_hash)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    if (mbedtls_sha256_starts(&ctx, 0)) // 0: sha256, 1: sha224
        goto exit_err;
    if (mbedtls_sha256_update(&ctx, data, data_len))
        goto exit_err;
    if (mbedtls_sha256_finish(&ctx, out_hash))
        goto exit_err;

    mbedtls_sha256_free(&ctx);
    return 0;

exit_err:
    mbedtls_sha256_free(&ctx);
    return -1;
}


static void common_task(void *arg)
{
    while (true) {
        vTaskDelay(200 / portTICK_PERIOD_MS);
        ble_maintain_task();
        wifi_maintain_task();
        cd_main_maintain_task();
    }
}

void app_main(void)
{
    uint8_t aes_key[32];
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init(); // phy calibration data
    }
    ESP_ERROR_CHECK(ret);

    cd_main_early();

    csa.k_random = esp_random();
    char key_str[12+24+1] = {0};
    sprintf(key_str, "cd_%08lx_", csa.k_random);
    memcpy(key_str + 12, csa.k_pwd, 24);

    if (sha256_sum((uint8_t *)key_str, strlen(key_str), aes_key))
        ESP_LOGE(tag, "sha cal err");
    if (aes256_cbc_init(aes_key))
        ESP_LOGE(tag, "aes init err");

    cd_ble_main();
    wifi_main();
    cd_main_late();
    mdns_init();

    xTaskCreate(common_task, "common_task", 4096, NULL, 5, NULL);
    ESP_LOGI(tag, "sha str: %s, len: %d", key_str, strlen(key_str));
    ESP_LOGI(tag, "key: %02x %02x ... %02x", aes_key[0], aes_key[1], aes_key[31]);
}
