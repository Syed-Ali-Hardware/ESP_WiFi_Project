#include "wifi_softap.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>
#include "esp_netif.h"

#define SOFTAP_SSID "ESP32_SoftAP"
#define SOFTAP_PASS "12345678"
static const char *TAG = "WiFi_SoftAP";

void wifi_init_softap(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL)
    {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi AP netif");
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = SOFTAP_SSID,
            .ssid_len = strlen(SOFTAP_SSID),
            .password = SOFTAP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK},
    };

    if (strlen(SOFTAP_PASS) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
    ESP_LOGI(TAG, "SoftAP IP Address: " IPSTR, IP2STR(&ip_info.ip));

    ESP_LOGI(TAG, "SoftAP started with SSID: %s, Password: %s", SOFTAP_SSID, SOFTAP_PASS);
}