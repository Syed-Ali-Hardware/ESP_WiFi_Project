#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "wifi_softap.h"
#include "webserver.h"
#include "wifi_sta.h"
#include "esp_netif.h"

// Function prototype for url_decode
void url_decode(char *dst, const char *src, size_t dst_len);

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define WIFI_SSID "EIE"
#define WIFI_PASS "EIE@4321"
#define SOFTAP_SSID "ESP32_SoftAP"
#define SOFTAP_PASS "12345678"

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
static const char *TAG = "WiFi_SoftAP";

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
        vTaskDelay(pdMS_TO_TICKS(5000));  // Wait before retrying
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected to AP. Got IP address:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

static esp_err_t handle_root_get(httpd_req_t *req)
{
    const char *response = "<!DOCTYPE html>"
                           "<html>"
                           "<body>"
                           "<h2>Enter WiFi Credentials</h2>"
                           "<form action=\"/connect\" method=\"post\">"
                           "SSID:<br><input type=\"text\" name=\"ssid\"><br>"
                           "Password:<br><input type=\"password\" name=\"password\"><br><br>"
                           "<input type=\"submit\" value=\"Submit\">"
                           "</form>"
                           "</body>"
                           "</html>";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t handle_connect_post(httpd_req_t *req)
{
    char buf[200];
    int ret, remaining = req->content_len;

    char raw_ssid[64] = {0};
    char raw_password[64] = {0};
    char ssid[32] = {0};
    char password[64] = {0};

    // Read the POST data
    if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf) - 1))) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            return ESP_OK;
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse and decode
    if (sscanf(buf, "ssid=%63[^&]&password=%63s", raw_ssid, raw_password) == 2) {
        // URL decode
        url_decode(ssid, raw_ssid, sizeof(ssid) - 1);
        url_decode(password, raw_password, sizeof(password) - 1);
        ESP_LOGI(TAG, "Received SSID: %s, Password: %s", ssid, password);

        // Save to NVS
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open error");
            return ESP_FAIL;
        }
        err = nvs_set_str(nvs_handle, "ssid", ssid);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS set ssid failed: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS set error");
            return ESP_FAIL;
        }
        err = nvs_set_str(nvs_handle, "password", password);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS set password failed: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS set error");
            return ESP_FAIL;
        }
        err = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS commit error");
            return ESP_FAIL;
        }

        httpd_resp_send(req, "Credentials saved. Rebooting...", HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "Credentials saved, restarting in 500ms...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to parse SSID and password");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid data");
        return ESP_FAIL;
    }
}

static esp_err_t handle_favicon_get(httpd_req_t *req)
{
    // Respond with an empty favicon
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = handle_root_get,
    .user_ctx = NULL};

static httpd_uri_t connect = {
    .uri = "/connect",
    .method = HTTP_POST,
    .handler = handle_connect_post,
    .user_ctx = NULL};

static httpd_uri_t favicon = {
    .uri = "/favicon.ico",
    .method = HTTP_GET,
    .handler = handle_favicon_get,
    .user_ctx = NULL};

void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;        // Increase the number of URI handlers
    config.uri_match_fn = NULL;          // Default URI matching function
    config.stack_size = 8192;            // Increase stack size for HTTP server
    config.recv_wait_timeout = 10;       // Increase timeout for receiving requests
    config.max_open_sockets = 4;         // Allow up to 4 simultaneous connections
    config.lru_purge_enable = true;      // Enable LRU purging for unused sockets
    config.uri_match_fn = NULL;          // Default URI matching function
    config.max_resp_headers = 16;        // Increase max response headers
    //    config.max_req_hdr_len = 1024;       // Removed as the field does not exist
    //    config.max_uri_len = 512;            // Increase max URI length

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting HTTP server...");
    if (httpd_start(&server, &config) == ESP_OK)
    {
        ESP_LOGI(TAG, "Registering URI handlers...");
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &connect);
        httpd_register_uri_handler(server, &favicon); // Register favicon handler
        ESP_LOGI(TAG, "HTTP server started successfully.");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start HTTP server.");
    }
}

void wifi_init_softap(void)
{
    // Create the default Wi-Fi AP network interface
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

    // Get and log the SoftAP IP address
    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
    ESP_LOGI(TAG, "SoftAP IP Address: " IPSTR, IP2STR(&ip_info.ip));

    ESP_LOGI(TAG, "SoftAP started with SSID: %s, Password: %s", SOFTAP_SSID, SOFTAP_PASS);
}

void softap_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Initializing SoftAP...");
    wifi_init_softap();             // Initialize SoftAP
    vTaskDelay(pdMS_TO_TICKS(100)); // Ensure SoftAP is fully initialized
    ESP_LOGI(TAG, "Starting webserver...");
    start_webserver();              // Start the webserver
    ESP_LOGI(TAG, "Webserver started successfully.");
    vTaskDelete(NULL);
}

void sta_task(void *pvParameters)
{
    wifi_connect_sta();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "ESP_WIFI_MODE_AP");

    // Check if credentials exist in NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    size_t ssid_len = 0;
    err = nvs_get_str(nvs_handle, "ssid", NULL, &ssid_len); // Check if "ssid" exists
    nvs_close(nvs_handle);

    if (err == ESP_OK)
    {
        // Credentials exist, connect to Wi-Fi
        ESP_LOGI(TAG, "Credentials found in NVS. Starting STA...");
        xTaskCreate(sta_task, "STA Task", 8192, NULL, 5, NULL);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // No credentials, start SoftAP and webserver
        ESP_LOGW(TAG, "No Wi-Fi credentials found in NVS. Starting SoftAP...");
        xTaskCreate(softap_task, "SoftAP Task", 8192, NULL, 5, NULL);
    }
    else
    {
        ESP_LOGE(TAG, "Error reading NVS: %s", esp_err_to_name(err));
    }
}

void url_decode(char *dst, const char *src, size_t dst_len)
{
    char a, b;
    size_t i = 0;
    while (*src && i + 1 < dst_len) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            *dst++ = (char)((isdigit((unsigned char)a) ? a - '0' : tolower((unsigned char)a) - 'a' + 10) << 4 |
                            (isdigit((unsigned char)b) ? b - '0' : tolower((unsigned char)b) - 'a' + 10));
            src += 3;
            i++;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
            i++;
        } else {
            *dst++ = *src++;
            i++;
        }
    }
    *dst = '\0';
}