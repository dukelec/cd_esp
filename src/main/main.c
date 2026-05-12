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

static psa_key_id_t aes_key_id = 0;


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
    psa_status_t status;
    uint8_t iv[16] = {0};
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t out_len = 0;
    size_t padded_len = pkcs7_pad(input, in_len); // input should large enough

    status = psa_cipher_encrypt_setup(&op, aes_key_id, PSA_ALG_CBC_NO_PADDING);
    if (status != PSA_SUCCESS)
        return -1;

    status = psa_cipher_set_iv(&op, iv, sizeof(iv));
    if (status != PSA_SUCCESS)
        goto fail;

    size_t part_len = 0;
    status = psa_cipher_update(&op, input, padded_len, output, padded_len, &part_len);
    if (status != PSA_SUCCESS)
        goto fail;
    out_len = part_len;

    status = psa_cipher_finish(&op, output + part_len, padded_len - part_len, &part_len);
    if (status != PSA_SUCCESS)
        goto fail;
    out_len += part_len;

    psa_cipher_abort(&op);
    return out_len;

fail:
    psa_cipher_abort(&op);
    return -1;
}

int aes256_cbc_decrypt(const uint8_t *input, size_t in_len, uint8_t *output)
{
    psa_status_t status;
    uint8_t iv[16] = {0};
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t out_len = 0;
    size_t part_len = 0;

    status = psa_cipher_decrypt_setup(&op, aes_key_id, PSA_ALG_CBC_NO_PADDING);
    if (status != PSA_SUCCESS)
        return -1;

    status = psa_cipher_set_iv(&op, iv, sizeof(iv));
    if (status != PSA_SUCCESS)
        goto fail;

    status = psa_cipher_update(&op, input, in_len, output, in_len, &part_len);
    if (status != PSA_SUCCESS)
        goto fail;
    out_len = part_len;

    status = psa_cipher_finish(&op, output + part_len, in_len - part_len, &part_len);
    if (status != PSA_SUCCESS)
        goto fail;
    out_len += part_len;

    psa_cipher_abort(&op);
    return pkcs7_unpad(output, out_len);

fail:
    psa_cipher_abort(&op);
    return -1;
}

static int aes256_cbc_init(const uint8_t *key)
{
    psa_status_t status;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

    status = psa_crypto_init();
    if (status != PSA_SUCCESS)
        return -1;

    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);

    status = psa_import_key(&attr, key, 32, &aes_key_id);
    psa_reset_key_attributes(&attr);

    return (status == PSA_SUCCESS) ? 0 : -1;
}


static int sha256_sum(const uint8_t *data, size_t data_len, uint8_t *out_hash)
{
    size_t out_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, data, data_len, out_hash, 32, &out_len);
    return (status == PSA_SUCCESS) ? 0 : -1;
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
