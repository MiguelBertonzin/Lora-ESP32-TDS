/*
 * ESP32 + SX1276 + Sensor TDS + ThingSpeak
 * Controle via menuconfig (SENDER/RECEIVER)
 * Inclui calibração ADC e reconexão Wi-Fi confiável
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/adc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "lora.h"

/* ===== CONFIGURAÇÕES DE REDE ===== */
#define WIFI_SSID      "Wifi"
#define WIFI_PASS      "Senha"
#define THINGSPEAK_URL "http://api.thingspeak.com/update"
#define API_KEY        "APIKEY"

/* ===== CONFIGURAÇÕES DO SENSOR TDS ===== */
#define TDS_ADC_CHANNEL   ADC_CHANNEL_4  // GPIO32
#define TDS_ADC_ATTEN     ADC_ATTEN_DB_11
#define TDS_ADC_UNIT      ADC_UNIT_1
#define TDS_ADC_BITWIDTH  ADC_BITWIDTH_12
#define NUM_SAMPLES       64

static adc_cali_handle_t adc_cali_handle = NULL;
static const char *TAG = "LORA_TDS";

/* ==================== EVENTOS WIFI ==================== */
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi desconectado, tentando reconectar...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Conectando ao Wi-Fi: %s...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(15000)
    );

    if (bits & WIFI_CONNECTED_BIT)
        ESP_LOGI(TAG, " Conectado ao Wi-Fi e IP obtido!");
    else
        ESP_LOGW(TAG, " Falha ao conectar em 15s, seguirá tentando...");
}

/* ==================== CALIBRAÇÃO ADC ==================== */
static bool init_adc_calibration(adc_unit_t unit, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = TDS_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &handle) == ESP_OK)
        calibrated = true;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = TDS_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &handle) == ESP_OK)
        calibrated = true;
#endif

    *out_handle = handle;
    return calibrated;
}

/* ==================== LEITURA TDS ==================== */
double read_tds_ppm(void)
{
    uint32_t acc_raw = 0;
    for (int i = 0; i < NUM_SAMPLES; i++) acc_raw += adc1_get_raw(TDS_ADC_CHANNEL);
    uint32_t raw = acc_raw / NUM_SAMPLES;

    int voltage_mv;
    if (adc_cali_raw_to_voltage(adc_cali_handle, raw, &voltage_mv) != ESP_OK)
        voltage_mv = (int)((raw / 4095.0) * 3600.0);

    double v = voltage_mv / 1000.0;
    double temperature_c = 25.0;
    double compensation_coeff = 1.0 + 0.02 * (temperature_c - 25.0);
    double v_comp = v / compensation_coeff;
    double ec_us = 133.42 * pow(v_comp, 3) - 255.86 * pow(v_comp, 2) + 857.39 * v_comp;
    double tds_ppm = ec_us * 0.5;

    ESP_LOGI(TAG, "TDS=%.1f ppm (%.1f uS/cm, %.2f V)", tds_ppm, ec_us, v);
    return tds_ppm;
}

/* ==================== ENVIO PARA THINGSPEAK ==================== */
void send_to_thingspeak(const char *field_data)
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi não conectado, ignorando envio");
        return;
    }

    char encoded_data[128];
    int j = 0;
    for (int i = 0; field_data[i] != '\0' && j < sizeof(encoded_data) - 4; i++) {
        if (field_data[i] == ' ')
            strcpy(&encoded_data[j], "%20"), j += 3;
        else
            encoded_data[j++] = field_data[i];
    }
    encoded_data[j] = '\0';

    char url[256];
    snprintf(url, sizeof(url), "%s?api_key=%s&field1=%s", THINGSPEAK_URL, API_KEY, encoded_data);

    esp_http_client_config_t config = {.url = url, .method = HTTP_METHOD_GET};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Falha ao criar cliente HTTP");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "ThingSpeak OK (HTTP %d): %s", esp_http_client_get_status_code(client), encoded_data);
    else
        ESP_LOGE(TAG, "Erro HTTP: %s", esp_err_to_name(err));

    esp_http_client_cleanup(client);
}

/* ==================== TASK DE TRANSMISSÃO ==================== */
#if CONFIG_SENDER
void task_tx(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "Modo EMISSOR iniciado");

    adc1_config_width(TDS_ADC_BITWIDTH);
    adc1_config_channel_atten(TDS_ADC_CHANNEL, TDS_ADC_ATTEN);
    init_adc_calibration(TDS_ADC_UNIT, TDS_ADC_ATTEN, &adc_cali_handle);

    uint8_t buf[255];

    while (1) {
        double tds = read_tds_ppm();
        snprintf((char *)buf, sizeof(buf), "%.1f ppm", tds);

        lora_send_packet(buf, strlen((char *)buf));
        ESP_LOGI(pcTaskGetName(NULL), "Enviado via LoRa: %s", buf);
        send_to_thingspeak((char *)buf);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif

/* ==================== TASK DE RECEPÇÃO ==================== */
#if CONFIG_RECEIVER
void task_rx(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "Modo RECEPTOR iniciado");

    uint8_t buf[255];
    while (1) {
        lora_receive();
        if (lora_received()) {
            int rxLen = lora_receive_packet(buf, sizeof(buf));
            buf[rxLen] = '\0';
            ESP_LOGI(pcTaskGetName(NULL), "Pacote recebido (%d bytes): [%s]", rxLen, buf);
            send_to_thingspeak((char *)buf);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

/* ==================== MAIN ==================== */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    ESP_LOGI(TAG, "Inicializando LoRa...");
    if (lora_init() == 0) {
        ESP_LOGE(TAG, "SX1276 não detectado!");
        while (1) vTaskDelay(1);
    }

    lora_set_frequency(915e6);
    lora_enable_crc();
    lora_set_coding_rate(1);
    lora_set_bandwidth(7);
    lora_set_spreading_factor(7);

#if CONFIG_SENDER
    xTaskCreate(&task_tx, "TX", 4096, NULL, 5, NULL);
#endif
#if CONFIG_RECEIVER
    xTaskCreate(&task_rx, "RX", 4096, NULL, 5, NULL);
#endif
}
