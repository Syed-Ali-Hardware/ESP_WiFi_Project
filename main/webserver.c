#include "webserver.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h" // Include for esp_restart
#include <ctype.h>

// Define the MIN macro if not already defined
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "WebServer";

static esp_err_t handle_root_get(httpd_req_t *req)
{
    const char *response = "<!DOCTYPE html>"
                           "<html>"
                           "<head>"
                           "    <title>WiFi Configuration</title>"
                           "</head>"
                           "<body>"
                           "    <h2>Enter WiFi Credentials</h2>"
                           "    <form action=\"/connect\" method=\"post\">"
                           "        SSID:<br><input type=\"text\" name=\"ssid\"><br>"
                           "        Password:<br><input type=\"password\" name=\"password\"><br><br>"
                           "        <input type=\"submit\" value=\"Submit\">"
                           "    </form>"
                           "</body>"
                           "</html>";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// Simple URL decode function
void url_decode(char *dst, const char *src, size_t dst_len)
{
    char a, b;
    size_t i = 0;
    while (*src && i + 1 < dst_len)
    {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b)))
        {
            if (i + 1 < dst_len)
            {
                *dst++ = (char)((isdigit(a) ? a - '0' : tolower(a) - 'a' + 10) << 4 |
                                (isdigit(b) ? b - '0' : tolower(b) - 'a' + 10));
                src += 3;
                i++;
            }
        }
        else if (*src == '+')
        {
            *dst++ = ' ';
            src++;
            i++;
        }
        else
        {
            *dst++ = *src++;
            i++;
        }
    }
    *dst = '\0';
}

static esp_err_t handle_connect_post(httpd_req_t *req)
{
    char buf[200]; // Increased buffer size
    int ret, remaining = req->content_len;

    char ssid[32] = {0};
    char password[64] = {0};

    if (remaining >= sizeof(buf))
    {
        ESP_LOGE(TAG, "Content length exceeds buffer size");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);

    if (ret <= 0)
    {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        else
        {
            ESP_LOGE(TAG, "Error receiving request");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read POST data");
        }
        return ESP_FAIL;
    }

    buf[ret] = '\0';
    ESP_LOGI(TAG, "Received data: %s", buf);

    // Parse SSID and Password from the POST data
    char raw_ssid[64] = {0};
    char raw_password[64] = {0};
    if (sscanf(buf, "ssid=%63[^&]&password=%63s", raw_ssid, raw_password) == 2)
    {
        url_decode(ssid, raw_ssid, sizeof(ssid));
        url_decode(password, raw_password, sizeof(password));
        ESP_LOGI(TAG, "Parsed SSID: %s, Password: %s", ssid, password);

        // Save credentials to NVS
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
            return ESP_FAIL;
        }

        err = nvs_set_str(nvs_handle, "ssid", ssid);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "NVS set ssid failed: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
            return ESP_FAIL;
        }

        err = nvs_set_str(nvs_handle, "password", password);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "NVS set password failed: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
            return ESP_FAIL;
        }

        err = nvs_commit(nvs_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
            return ESP_FAIL;
        }

        nvs_close(nvs_handle);

        const char *response = "Credentials saved. Rebooting to connect...";
        httpd_resp_send(req, response, strlen(response));
        ESP_LOGI(TAG, "Credentials saved, restarting in 500ms...");
        vTaskDelay(pdMS_TO_TICKS(500)); // <-- Add this delay
        esp_restart();                  // Restart the ESP32
        return ESP_OK;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to parse SSID and password");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid data");
        return ESP_FAIL;
    }
}

static esp_err_t handle_favicon_get(httpd_req_t *req)
{
    httpd_resp_send(req, NULL, 0); // Respond with an empty body
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

static httpd_uri_t favicon_uri = {
    .uri = "/favicon.ico",
    .method = HTTP_GET,
    .handler = handle_favicon_get,
    .user_ctx = NULL};

void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;   // Increase the number of URI handlers
    config.uri_match_fn = NULL;     // Default URI matching function
    config.stack_size = 8192;       // Increase stack size for HTTP server
    config.recv_wait_timeout = 10;  // Increase timeout for receiving requests
    config.max_open_sockets = 4;    // Allow up to 4 simultaneous connections
    config.lru_purge_enable = true; // Enable LRU purging for unused sockets

    // Correct field to increase request header buffer:
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting HTTP server...");
    if (httpd_start(&server, &config) == ESP_OK)
    {
        ESP_LOGI(TAG, "Registering URI handlers...");
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &connect);
        httpd_register_uri_handler(server, &favicon_uri); // Register favicon handler
        ESP_LOGI(TAG, "HTTP server started successfully.");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start HTTP server.");
    }
}