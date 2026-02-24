#include <stdio.h>  // np snprintf, printf
#include <string.h> //np strlen, strncpy

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h" //np esp_restart()
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"   //ESP_LOGI
#include "nvs_flash.h" // obsługa NVS

#include "esp_mac.h"         // czytam MAC
#include "esp_http_server.h" // stawia serwer WWW w trybie SoftAP
#include "cJSON.h"

// libgi do obsługi TCP/IP
#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_manager.h"

static const char *TAG = "WIFI_MGR";          // Tag do logów
static EventGroupHandle_t s_wifi_event_group; // do sprawdzania stanu WiFi np do maina przesyłamy ten uchwyt

#define AP_SSID "ESP32_SETUP_PROV" // Nazwa Hotspota
#define AP_PASS ""                 // Brak hasła (otwarty)

// handluje POST z aplikacji webowej/PWA zawierający
// SSID i hasło do domowego WiFi
// zapisuje je do NVS
// zwraca również MAC adres (potrzebny do identyfikacji urządzenia w MQTT)
// Odbiera: {"ssid": "Dom", "password": "123"}
// Zwraca:  {"status": "ok", "mac": "A1B2C3D4E5F6"}

static esp_err_t wifi_config_handler(httpd_req_t *req)
{
    char buf[150];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf))
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // odczytuje dane z POST
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0)
        return ESP_FAIL;
    buf[ret] = '\0';

    ESP_LOGI(TAG, "Otrzymano konfigurację: %s", buf);

    // Parsuje JSON
    cJSON *root = cJSON_Parse(buf);
    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *password = cJSON_GetObjectItem(root, "password");
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt_uri");      // URI borekera MQTT
    cJSON *pinpad = cJSON_GetObjectItem(root, "pinpad_code"); // kod do pinpada
    if (mqtt && cJSON_IsString(mqtt) && ssid && cJSON_IsString(ssid))
    {
        // zapisz dane o WiFi do NVS
        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, ssid->valuestring, sizeof(wifi_config.sta.ssid));
        if (password && cJSON_IsString(password))
        {
            strncpy((char *)wifi_config.sta.password, password->valuestring, sizeof(wifi_config.sta.password));
        }

        ESP_LOGI(TAG, "Zapisuję SSID: %s", wifi_config.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

        nvs_handle_t my_nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_nvs_handle);
        if (err == ESP_OK)
        {
            err = nvs_set_str(my_nvs_handle, "mqtt_uri", mqtt->valuestring);
            if (pinpad && cJSON_IsString(pinpad))
            {
                nvs_set_str(my_nvs_handle, "pinpad_code", pinpad->valuestring);
            }
            nvs_commit(my_nvs_handle);
            nvs_close(my_nvs_handle);
            ESP_LOGI(TAG, "Saving MQTT URI: %s", mqtt->valuestring);
            if (pinpad && cJSON_IsString(pinpad))
            {
                ESP_LOGI(TAG, "Saving PINPAD code");
            }
        }
        else
        {
            ESP_LOGE(TAG, "Error opening NVS handle!");
        }

        // pobieram MAC do automatycznego przypisania do użytkownika
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char mac_str[17];
        snprintf(mac_str, sizeof(mac_str), "ESP32_MAC_%02X%02X%02X",
                 mac[3], mac[4], mac[5]);

        char resp_str[100];
        snprintf(resp_str, sizeof(resp_str), "{\"status\":\"ok\", \"mac\":\"%s\"}", mac_str);

        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

        ESP_LOGW(TAG, "Restart za 2 sekundy...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    else
    {
        httpd_resp_send_500(req);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// aplikacja jest w internecie a esp w sieci lokalnej
// dlatego musimy zezwolić na CORS (Cross-Origin Resource Sharing)
static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Start serwera HTTP
static void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192; // Większy stos dla JSONa

    ESP_LOGI(TAG, "Startuję Web Server...");

    // uruchamia nasłuchiwanie na porcie 80
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // rejstracja endpointu do konfiguracji WiFi
        httpd_uri_t wifi_cfg = {
            .uri = "/api/wifi",
            .method = HTTP_POST,
            .handler = wifi_config_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &wifi_cfg);

        // rejstracja endpointu dzięki któremu działą CORS
        httpd_uri_t wifi_opt = {
            .uri = "/api/wifi",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &wifi_opt);
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    // próba połączenia do wifi
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    // łączenie do skutku
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // ogika ponownego łączenia
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_EVENT);
        ESP_LOGI(TAG, "Ponawiam łączenie z Wifi.");
    }
    // udało się połączyć do wifi
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Połączono! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_EVENT);
    }
}

void wifi_manager_start(void)
{
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_wifi_event_group = xEventGroupCreate();

    // Inicjalizacja warstwy sieciowej i event loop
    // ESP_ERROR_CHECK - makro do sprawdzania błędów zwracanych przez funkcje esp_err_t
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    // initializacja sterownika wifi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,       // baza zdarzeń (kategoria)
        ESP_EVENT_ANY_ID, // które zdarzenia z tej bazy? (tu: wszystkie)
        &event_handler,   // wskaźnik na funkcję callback
        NULL,
        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &event_handler,
        NULL,
        NULL));

    ESP_LOGI(TAG, "Sprawdzam konfigurację WiFi w NVS...");
    wifi_config_t wifi_config;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK && strlen((char *)wifi_config.sta.ssid) > 0)
    {
        // już wgraliśmy konfigurację wifi wieć normalnie się łączymy
        ESP_LOGI(TAG, "Znaleziono konfigurację WiFi: %s", wifi_config.sta.ssid);
        uint8_t mac[6];

        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "esp32-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        esp_netif_set_hostname(sta_netif, hostname);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    else
    {
        // brak konfiguracji, włączamy AP i serwer http
        ESP_LOGW(TAG, "Brak WiFi. Uruchamiam SoftAP: %s", AP_SSID);

        wifi_config_t ap_config = {
            .ap = {
                .ssid = AP_SSID,
                .ssid_len = strlen(AP_SSID),
                .password = AP_PASS,
                .max_connection = 4,
                .authmode = WIFI_AUTH_OPEN},
        };

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        start_webserver();
        // po wgraniu konfiguracji reset urządzenia
        //  następnie połaczenie się do sieci -> brak AP
    }
}

EventGroupHandle_t wifi_get_event_group(void)
{
    return s_wifi_event_group;
}
#include "esp_partition.h"

void wifi_manager_reset_provisioning(void)
{
    ESP_LOGW(TAG, "FACTORY RESET: Kasowanie WiFi z NVS!");

    const esp_partition_t *nvs_part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_NVS,
                                 NULL);

    ESP_ERROR_CHECK(esp_partition_erase_range(nvs_part, 0, nvs_part->size));

    esp_restart();
}

esp_err_t wifi_manager_get_mqtt_uri(char *buf, size_t buf_len)
{
    nvs_handle_t my_nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_nvs_handle);
    if (err != ESP_OK)
        return err;

    err = nvs_get_str(my_nvs_handle, "mqtt_uri", buf, &buf_len);
    nvs_close(my_nvs_handle);
    return err;
}