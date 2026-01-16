/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <d@d-l.io>
 */

#include "cd_main.h"
static const char *tag = "cd-main";

static gpio_t led_w_pin = LED_W_PIN;
static gpio_t led_g_pin = LED_G_PIN;
static gpio_t button_pin = BUTTON_PIN;
static gpio_t buzzer_pin = BUZZER_PIN;
static gpio_t mco_pin = MCO_PIN;

static cd_frame_t frame_alloc[FRAME_MAX];
list_head_t frame_free_head = {0};

list_head_t ble_rx_head = {0};
list_head_t udp_rx_head = {0};

TaskHandle_t dispatch_task_handle = NULL;
static TaskHandle_t button_task_handle = NULL;


static void IRAM_ATTR gpio_isr_cd_int_n(void *arg)
{
    cdctl_int_isr();
}

static void IRAM_ATTR gpio_isr_button(void *arg)
{
    BaseType_t task_woken = pdFALSE;
    gpio_isr_handler_remove(button_pin);
    vTaskNotifyGiveFromISR(button_task_handle, &task_woken);
    portYIELD_FROM_ISR(task_woken);
}


void configure_clock_output()
{
    // mco, clk for cdctl
    ledc_timer_config_t ledc_timer_clk = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .freq_hz = 8000000,
        .clk_cfg = LEDC_USE_APB_CLK
    };
    ledc_timer_config(&ledc_timer_clk);

    ledc_channel_config_t ledc_channel_clk = {
        .gpio_num = mco_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel_clk);

    // buzzer
    ledc_timer_config_t ledc_timer_buzzer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 2000,
        .clk_cfg = LEDC_USE_APB_CLK
    };
    ledc_timer_config(&ledc_timer_buzzer);

    ledc_channel_config_t ledc_channel_buzzer = {
        .gpio_num = buzzer_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0, // 0% duty
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel_buzzer);

    // led
    ledc_timer_config_t ledc_timer_led = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_2,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 100000,
        .clk_cfg = LEDC_USE_APB_CLK
    };
    ledc_timer_config(&ledc_timer_led);

    ledc_channel_config_t led_channels[] = {
        { .gpio_num = led_w_pin, .channel = LEDC_CHANNEL_2 },
        { .gpio_num = led_g_pin, .channel = LEDC_CHANNEL_3 },
    };

    for (int i = 0; i < 2; i++) {
        led_channels[i].speed_mode = LEDC_LOW_SPEED_MODE;
        led_channels[i].timer_sel = LEDC_TIMER_2;
        led_channels[i].duty = 10; // 10/256 duty
        led_channels[i].hpoint = 0;
        led_channels[i].flags.output_invert = 1;
        ledc_channel_config(&led_channels[i]);
    }
}

static void _buzzer_set(int freq_hz)
{
    if (freq_hz) {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, freq_hz);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 128);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 2000);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    }
}

static void buzzer_set(int freq, int on_ms, int off_ms, int times)
{
    for (int i = 0; i < times; i++) {
        _buzzer_set(freq);
        vTaskDelay(on_ms / portTICK_PERIOD_MS);
        _buzzer_set(0);
        vTaskDelay(off_ms / portTICK_PERIOD_MS);
    }
}

static void led_set_w(uint8_t duty_w)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, duty_w);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

static void led_set_g(uint8_t duty_g)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, duty_g);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
}


static void dispatch_task(void *arg)
{
    while (true) {
        if (!cdctl_rx_head.first && !ble_rx_head.first && !udp_rx_head.first) {
            ulTaskNotifyTake(pdTRUE, 100 / portTICK_PERIOD_MS);
            continue;
        }
        common_service_routine();
    }
}


static void button_task(void *arg)
{
    gpio_isr_handler_add(button_pin, gpio_isr_button, NULL);
    buzzer_set(2730, 500, 200, 1);

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(tag, "button task active, %d\n", gpio_get_val(&button_pin));
        for (int i = 0; i < 10; i++) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            bool val = gpio_get_val(&button_pin);
            if (val) {
                if (i < 5) {
                    ESP_LOGI(tag, "button short press\n");
                    buzzer_set(2703, 25, 25, 1);
                    led_set_w(200);
                    //sent_cmd(csa.p_mac, (uint8_t[]){0x40, 5, 0xa0, 0xac, 0x01, 0x04, 0, 0x10}, 8, false, NULL);
                }
                break;
            } else {
                if (i >= 8) {
                    ESP_LOGI(tag, "button long press\n");
                    buzzer_set(2000, 25, 25, 1);
                    led_set_g(200);
                    //sent_cmd(csa.p_mac, (uint8_t[]){0x40, 5, 0xa0, 0xac, 0x01, 0x02}, 6, false, NULL);
                    break;
                }
            }
        }

        while (!gpio_get_val(&button_pin))
            vTaskDelay(100 / portTICK_PERIOD_MS);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        gpio_isr_handler_add(button_pin, gpio_isr_button, NULL);
    }
}


void cd_main_maintain_task(void)
{
    static uint32_t t_last = 0;

    if (get_systick() - t_last >= 5000) {
        t_last = get_systick();
        ESP_LOGI(tag, "k_st: %d, ble_conn %d, t_ble_conn %08lx, rx %ld, free %ld",
                csa.k_st_ble, csa.ble_connect, csa.t_ble_connect, ble_rx_head.len, esp_get_free_heap_size());
        ESP_LOGI(tag, "bus: %d, pend t %ld r %ld, irq %d, r %ld (lost %ld err %ld full %ld), t %ld (cd %ld err %ld)",
            cdctl_state, cdctl_tx_head.len, cdctl_rx_head.len, !CD_INT_RD(),
            cdctl_rx_cnt, cdctl_rx_lost_cnt, cdctl_rx_error_cnt, cdctl_rx_no_free_node_cnt,
            cdctl_tx_cnt, cdctl_tx_cd_cnt, cdctl_tx_error_cnt);
    }
}


static int multi_output_vprintf(const char *fmt, va_list args) {
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        if (len >= 2) {
            if (buf[len-2] == buf[len-1])
                len--; // "\n\n" -> "\n"
        }
        fwrite(buf, 1, len, stdout);

        if (csa.dbg_en) {
            cd_frame_t *frm = cd_list_get(&frame_free_head);
            if (frm) {
                len = min(CDN_MAX_DAT - 2, len);
                frm->dat[0] = bus_mac;
                frm->dat[1] = 0;
                frm->dat[2] = 2 + len;
                frm->dat[3] = 0x40;
                frm->dat[4] = 9;
                memcpy(frm->dat + 5, buf, len);
                cdctl_put_tx_frame(frm);
            }
        }
    }
    return len;
}


void cd_main_early(void)
{
    ESP_LOGI(tag, "start cd_main_early ...\n");
    configure_clock_output();

    for (int i = 0; i < FRAME_MAX; i++)
        cd_list_put(&frame_free_head, &frame_alloc[i]);

    load_conf();
    cdctl_spi_wr_init();
    cdctl_dev_init(&csa.bus_cfg);

    gpio_config_t io_conf_cd_int_n = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << cd_int_n),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf_cd_int_n);

    gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    gpio_isr_handler_add(cd_int_n, gpio_isr_cd_int_n, NULL);

    esp_log_set_vprintf(multi_output_vprintf);
}

void cd_main_late(void)
{
    ESP_LOGI(tag, "start cd_main_late ...\n");

    common_service_init();
    xTaskCreate(dispatch_task, "dispatch_task", 4096, NULL, 20, &dispatch_task_handle);
    xTaskCreate(button_task, "button_task", 4096, NULL, 2, &button_task_handle);

    gpio_config_t io_conf_button = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << button_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf_button);
    csa_list_show();
}
