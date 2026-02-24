#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/event_groups.h"

#include "wifi_manager.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "mqtt_app.h"
#include "mdns.h"

#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_mac.h"

static const char *TAG = "main";

#define CONNECTED_LED_GPIO ((gpio_num_t)CONFIG_CONNECTED_LED_GPIO)
#define ROBBERY_LED_GPIO ((gpio_num_t)CONFIG_ROBBERY_LED_GPIO)
#define BATTERY_ADC_GPIO ((gpio_num_t)CONFIG_BATTERY_MEASURE_GPIO)

#define BUTTON_GPIO GPIO_NUM_0

static EventGroupHandle_t s_wifi_event_group;

#define DEFAULT_HB_MS 15000
#define DEFAULT_SLEEP_MS 30000
#define DEFAULT_THRESHOLD 0.5
#define BATTERY_READ_PERIOD_MS 5000
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_11
#define BATTERY_DIVIDER_RATIO 2.0f

static void led_task(void *arg);
static void robbery_led_task(void *arg);
static void battery_read_task(void *arg);
static void http_get_task(void *arg);
static void do_http_get_once(void);
void button_watchdog_task(void *pvParameters);
void mdns_set_hostname_from_mac(void);

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_cali_enabled;

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set("BAT", ESP_LOG_INFO);
    esp_log_level_set("main", ESP_LOG_INFO);
    esp_log_level_set("BTN", ESP_LOG_INFO);
    esp_log_level_set("WIFI_MGR", ESP_LOG_INFO);
    esp_log_level_set("MQTT_APP", ESP_LOG_INFO);

    ESP_LOGI(TAG, "Start");

    ESP_LOGI(TAG, "Inicjalizacja przycisku resetu WiFi...");
    xTaskCreate(button_watchdog_task, "button_watchdog_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Start menedżera WiFi...");
    wifi_manager_start();
    s_wifi_event_group = wifi_get_event_group();

    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    xTaskCreate(robbery_led_task, "robbery_led_task", 2048, NULL, 5, NULL);
    xTaskCreate(battery_read_task, "battery_read_task", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Czekam na WiFi...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_EVENT, pdFALSE, pdFALSE, portMAX_DELAY);

    ESP_LOGI(TAG, "Startuję mDNS...");
    ESP_ERROR_CHECK(mdns_init());
    mdns_set_hostname_from_mac();

    ESP_LOGI(TAG, "WiFi OK. Startuję MQTT...");

    char mqtt_uri[128] = {0};
    ESP_ERROR_CHECK(wifi_manager_get_mqtt_uri(mqtt_uri, sizeof(mqtt_uri)));

    mqtt_app_config_t config = {
        .uri = mqtt_uri,
        .heartbeat_interval = DEFAULT_HB_MS,
        .deep_sleep_interval = DEFAULT_SLEEP_MS,
        .theft_threshold = DEFAULT_THRESHOLD};

    mqtt_app_start(&config);

    // milestone wifi zad2 (zbędne do projektu)
    // xTaskCreate(http_get_task, "http_get_task", 4096, NULL, 5, NULL);
}

static void battery_read_task(void *arg)
{
    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &unit, &channel);
    if (err != ESP_OK)
    {
        ESP_LOGE("BAT", "Nieprawidlowy GPIO dla ADC: %d", BATTERY_ADC_GPIO);
        vTaskDelete(NULL);
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, channel, &chan_cfg));

    while (1)
    {
        int raw = 0;
        int mv = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, channel, &raw));

        if (s_adc_cali_enabled)
        {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_adc_cali, raw, &mv));
        }
        else
        {
            const int max_raw = (1 << 12) - 1;
            mv = (raw * 3300) / max_raw;
        }

        float battery_mv = (float)mv * BATTERY_DIVIDER_RATIO;
        // ESP_LOGI("BAT", "Bateria: %.0f mV (ADC=%d)", battery_mv, raw);

        // Przekaż napięcie baterii do modułu MQTT
        mqtt_app_set_battery_mv(battery_mv);

        vTaskDelay(pdMS_TO_TICKS(BATTERY_READ_PERIOD_MS));
    }
}

void mdns_set_hostname_from_mac(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    char hostname[32];
    snprintf(hostname, sizeof(hostname),
             "ESP32_%02x%02x%02x",
             mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "mDNS hostname = %s", hostname);
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
}

void button_watchdog_task(void *pvParameters)
{
    // Konfiguracja pinu
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);

    int press_duration = 0;

    while (1)
    {
        if (gpio_get_level(BUTTON_GPIO) == 0)
        {
            press_duration++;
            // Co 100ms wypisujemy log, że trzymamy
            if (press_duration % 10 == 0)
            {
                ESP_LOGI("BTN", "Trzymanie przycisku: %d ms", press_duration * 100);
            }

            if (press_duration >= 30)
            {

                // Wywołujemy funkcję z wifi_manager
                mqtt_send_delete_db();

                vTaskDelay(1000);
                wifi_manager_reset_provisioning();
            }
        }
        else
        {
            press_duration = 0; // Reset licznika
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Sprawdzaj co 100ms
    }
}
// milestone 1 wifi zad1
static void led_task(void *arg)
{

    gpio_reset_pin(CONNECTED_LED_GPIO);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONNECTED_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 1,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io);
    gpio_set_level(CONNECTED_LED_GPIO, 0);
    gpio_set_drive_capability(CONNECTED_LED_GPIO, GPIO_DRIVE_CAP_3);

    bool wifi_level = false;

    while (1)
    {

        EventBits_t b = xEventGroupGetBits(s_wifi_event_group);

        if ((b & WIFI_CONNECTED_EVENT) == 0)
        {
            wifi_level = !wifi_level;
            gpio_set_level(CONNECTED_LED_GPIO, wifi_level);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        else if (!server_is_alive)
        {

            gpio_set_level(CONNECTED_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            gpio_set_level(CONNECTED_LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(CONNECTED_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            gpio_set_level(CONNECTED_LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        else
        {
            gpio_set_level(CONNECTED_LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void robbery_led_task(void *arg)
{
    gpio_reset_pin(ROBBERY_LED_GPIO);
    gpio_set_direction(ROBBERY_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(ROBBERY_LED_GPIO, GPIO_PULLDOWN_ONLY);
    gpio_set_drive_capability(ROBBERY_LED_GPIO, GPIO_DRIVE_CAP_3);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num = ROBBERY_LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);
    ledc_fade_func_install(0);

    const uint32_t max_duty = (1 << LEDC_TIMER_10_BIT) - 1;

    while (1)
    {
        if (s_alarm_triggered)
        {
            ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, max_duty, 800);
            ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
            ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, 800);
            ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
        }
        else
        {
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            gpio_set_level(ROBBERY_LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

// milestone 2 http zad2
#define HTTP_HOST "example.com"
#define HTTP_PATH "/"
#define HTTP_PORT "80"
static void http_get_task(void *arg)
{
    while (1)
    {
        xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_CONNECTED_EVENT,
                            pdFALSE, pdFALSE,
                            portMAX_DELAY);

        ESP_LOGI(TAG, "WiFi online -> HTTP GET %s%s", HTTP_HOST, HTTP_PATH);
        do_http_get_once();

        // przykładowo: pobierz raz i śpij (albo pobieraj co jakiś czas)
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void do_http_get_once(void)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,       // iPv4
        .ai_socktype = SOCK_STREAM, // TCP
    };

    struct addrinfo *res = NULL;

    // pyta DNS o adres IP serwera HTTP_HOST
    int err = getaddrinfo(HTTP_HOST, HTTP_PORT, &hints, &res);

    if (err != 0 || res == NULL)
    {
        ESP_LOGE(TAG, "getaddrinfo(%s:%s) failed: err=%d", HTTP_HOST, HTTP_PORT, err);
        return;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        freeaddrinfo(res);
        return;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0)
    {
        ESP_LOGE(TAG, "connect() failed: errno=%d", errno);
        close(sock);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);

    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: esp32\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     HTTP_PATH, HTTP_HOST);

    int sent = send(sock, req, n, 0);
    if (sent < 0)
    {
        ESP_LOGE(TAG, "send() failed: errno=%d", errno);
        close(sock);
        return;
    }

    char buf[512];
    while (1)
    {
        int r = recv(sock, buf, sizeof(buf) - 1, 0);
        if (r < 0)
        {
            ESP_LOGE(TAG, "recv() failed: errno=%d", errno);
            break;
        }
        if (r == 0)
        {
            break; // koniec
        }
        buf[r] = '\0';

        printf("%s", buf);
    }

    close(sock);
}
