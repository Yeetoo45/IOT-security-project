#include <stdio.h>  //  np snprintf
#include <stdint.h> // np uint8_t
#include <string.h> // np strlen
#include <math.h>   // np sqrtf

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "driver/i2c.h"
#include "sdkconfig.h"

#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_app.h"
#include "adxl345.h"
#include "pinpad.h"
#include "audio_player.h"

#include "cJSON.h"
#include "nvs_flash.h"

static const char *TAG = "MQTT_APP";

esp_mqtt_client_handle_t client = NULL; // Uchwyt klienta MQTT
volatile bool server_is_alive = false;  // zmienna określa czy serwe mqtt żyje aby migać diodą
static mqtt_app_config_t s_config;      // Konfiguracja aplikacji MQTT (ustawiana przy starcie na wartości deafaultowe lub do zmiany)

static bool s_task_started = false; // czy zadanie już nie działa, żeby nie dublować przy reconnect
static bool s_mqtt_connected_to_server = false;
static int64_t s_last_server_contact = 0;

static char s_topic_data[128];
static char s_topic_config[128];
static char s_uri_with_local[256];

static volatile float s_theft_threshold = 1.5; // Domyślny próg
static volatile bool s_is_armed = false;       // Czy system czuwa?
volatile bool s_alarm_triggered = false;       // Czy wykryto kradzież?

static bool s_pinpad_task_started = false;
static TaskHandle_t s_pinpad_task_handle = NULL;
static TaskHandle_t s_publisher_task_handle = NULL;
#define PINPAD_CODE_MAX_LEN 8
static char s_pinpad_code[PINPAD_CODE_MAX_LEN + 1] = "1234";
static bool s_audio_ready = false;
static bool s_alarm_sound_active = false;
static adxl345_handle_t s_adxl_handle = NULL;
static bool s_adxl_i2c_ready = false;

#define TOPIC_HB "iot/server/status" // jak ESP nie dostanie przez jakiś czas to nie wysyła danych

static void load_pinpad_code(void) // wczytuje kod pinpada z NVS lub ustawia domyślny jeśli się nie zgadza coś
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "PINPAD: brak NVS (%s), używam domyślnego", esp_err_to_name(err));
        return;
    }

    size_t len = sizeof(s_pinpad_code);
    err = nvs_get_str(handle, "pinpad_code", s_pinpad_code, &len);
    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "PINPAD: brak kodu w NVS, używam domyślnego");
        return;
    }

    size_t code_len = strlen(s_pinpad_code);
    if (code_len == 0 || code_len > PINPAD_CODE_MAX_LEN)
    {
        strcpy(s_pinpad_code, "1234");
        return;
    }

    for (size_t i = 0; i < code_len; i++)
    {
        if (s_pinpad_code[i] < '0' || s_pinpad_code[i] > '9')
        {
            strcpy(s_pinpad_code, "1234");
            return;
        }
    }
}
static void generate_topics(void) // ustawienie tematów MQTT na podstawie MAC

{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[17]; // Np. "ESP32_MAC_D4E5F6"
    snprintf(mac_str, sizeof(mac_str), "ESP32_MAC_%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    // dane z czujnika wychodzące (ESP -> Serwer)
    snprintf(s_topic_data, sizeof(s_topic_data), "iot/device/%s/data", mac_str);

    // konfiguracja przychodząca (Serwer -> ESP)
    snprintf(s_topic_config, sizeof(s_topic_config), "iot/device/%s/config", mac_str);

    ESP_LOGI(TAG, "Generated Topics for ID: %s", mac_str);
}
static void build_uri_with_local(const char *in_uri, char *out_uri, size_t out_len) // buduje URI z .local dla mDNS
{
    if (in_uri == NULL || out_uri == NULL || out_len == 0)
    {
        return;
    }

    if (strstr(in_uri, ".local") != NULL)
    {
        snprintf(out_uri, out_len, "%s", in_uri);
        return;
    }

    const char *scheme = strstr(in_uri, "://");
    const char *host_start = scheme ? scheme + 3 : in_uri;
    const char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/')
    {
        host_end++;
    }

    const char *scheme_prefix = scheme ? in_uri : "mqtt://";
    size_t prefix_len = scheme ? (size_t)(host_start - in_uri) : strlen(scheme_prefix);
    size_t host_len = (size_t)(host_end - host_start);

    snprintf(out_uri, out_len, "%.*s%.*s.local%s",
             (int)prefix_len, scheme ? in_uri : scheme_prefix,
             (int)host_len, host_start,
             host_end);
}
static void process_config(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "JSON Parse Error");
        return;
    }

    ESP_LOGI(TAG, "Otrzymano Config: %s", payload);

    // obsługa Komend (ARM / DISARM)
    //{"cmd": "ARM"} lub {"cmd": "DISARM"}
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd) && (cmd->valuestring != NULL))
    {
        if (strcmp(cmd->valuestring, "FACTORY_RESET") == 0)
        {
            ESP_LOGW(TAG, "FACTORY RESET");
            nvs_flash_erase();
            esp_restart();
        }
        else if (strcmp(cmd->valuestring, "ARM") == 0)
        {
            s_is_armed = true;
            // Jeśli uzbrajamy, to resetujemy stary alarm
            s_alarm_triggered = false;

            ESP_LOGW(TAG, "CMD: System UZBROJONY (ARMED)");
        }
        else if (strcmp(cmd->valuestring, "DISARM") == 0)
        {
            s_is_armed = false;
            s_alarm_triggered = false; // Rozbrojenie zawsze kasuje alarm
            if (s_audio_ready)
            {
                audio_player_stop();
                s_alarm_sound_active = false;
            }

            ESP_LOGI(TAG, "CMD: System ROZBROJONY (DISARMED)");
        }
        else if (strcmp(cmd->valuestring, "ROBBERY") == 0)
        {
            ESP_LOGI(TAG, "CMD: WYKRYTO KRADZIEŻ (fake for test)");
            s_alarm_triggered = true; // Zatrzaśnięcie alarmu

            if (!s_alarm_sound_active && s_audio_ready)
            {
                ESP_LOGI(TAG, "CMD: Odtwarzam dźwięk alarmu");
                audio_player_loop_track(1);
                s_alarm_sound_active = true;
            }
        }
    }

    // obsługa Progu Czułości (Threshold)
    //{"threshold": 15.5}
    cJSON *thresh = cJSON_GetObjectItem(root, "threshold");
    if (cJSON_IsNumber(thresh))
    {
        s_theft_threshold = (float)thresh->valuedouble;
        ESP_LOGI(TAG, "CONFIG: Zmieniono threshold na %.2f G", s_theft_threshold);
    }

    cJSON_Delete(root);
}
static void pinpad_disarm_task(void *pvParameters)
{
    (void)pvParameters;

    char entered[PINPAD_CODE_MAX_LEN + 1] = {0};
    uint8_t index = 0;

    for (;;)
    {
        char key = 0;
        if (!pinpad_wait_key(&key, portMAX_DELAY))
        {
            continue;
        }
        else
        {
            ESP_LOGI(TAG, "PINPAD: Wciśnięto klawisz: %c", key);
        }

        if (key == '*')
        {
            index = 0;
            memset(entered, 0, sizeof(entered));
            continue;
        }

        if (key == '#')
        {
            size_t code_len = strlen(s_pinpad_code);
            if (code_len == 0)
            {
                strcpy(s_pinpad_code, "1234");
                code_len = strlen(s_pinpad_code);
            }

            entered[index] = '\0';
            if (index == code_len && strcmp(entered, s_pinpad_code) == 0)
            {
                if (s_is_armed)
                {
                    s_is_armed = false;
                    ESP_LOGI(TAG, "PINPAD: poprawny kod, rozbrojono");
                    if (s_audio_ready)
                    {
                        audio_player_stop();
                        s_alarm_sound_active = false;
                        s_alarm_triggered = false;
                    }
                }
                else
                {
                    s_is_armed = true;
                    s_alarm_triggered = false;
                    ESP_LOGI(TAG, "PINPAD: poprawny kod, uzbrojono");
                }
            }
            else
            {
                ESP_LOGW(TAG, "PINPAD: błędny kod");
            }

            index = 0;
            memset(entered, 0, sizeof(entered));
            continue;
        }

        if (key < '0' || key > '9')
        {
            continue;
        }

        if (s_alarm_sound_active == false && s_audio_ready)
        {
            audio_player_play_track(2);
        }

        if (index < PINPAD_CODE_MAX_LEN)
        {
            entered[index++] = key;
        }
        else
        {
            index = 0;
            memset(entered, 0, sizeof(entered));
        }
    }
}
static esp_err_t adxl345_i2c_init_once(void)
{
    if (s_adxl_i2c_ready)
    {
        return ESP_OK;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_ADXL345_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_ADXL345_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CONFIG_ADXL345_I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(CONFIG_ADXL345_I2C_PORT, &conf);
    if (err != ESP_OK)
    {
        return err;
    }

    err = i2c_driver_install(CONFIG_ADXL345_I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    s_adxl_i2c_ready = true;
    return ESP_OK;
}

static void publisher_task(void *pvParameters)
{
    char payload[256];
    adxl345_accel_t accel;

    int64_t last_publish_time = 0;
    const int64_t PUBLISH_INTERVAL_MS = 5000; // raport co 5s (gdy brak alarmu)
    const int64_t SAMPLING_INTERVAL_MS = 50;  // pomiar co 50ms

    ESP_LOGI(TAG, "Inicjalizacja ADXL345...");

    if (s_adxl_handle == NULL)
    {
        esp_err_t i2c_err = adxl345_i2c_init_once();
        if (i2c_err == ESP_OK)
            s_adxl_handle = adxl345_create(CONFIG_ADXL345_I2C_PORT, CONFIG_ADXL345_I2C_ADDR);
        if (s_adxl_handle)
            adxl345_set_range(s_adxl_handle, ADXL345_RANGE_16G);
    }

    while (1)
    {
        // Pobierz aktualny czas raz na obiegu pętli
        int64_t now_ms = esp_timer_get_time() / 1000;

        // dziala zawsze niezależnie od stanu serwera MQTT
        bool alarm_just_triggered = false;

        if (s_adxl_handle != NULL)
        {
            if (adxl345_get_accel(s_adxl_handle, &accel) == ESP_OK)
            {
                float current_g = sqrtf((accel.x * accel.x) + (accel.y * accel.y) + (accel.z * accel.z));

                if (s_is_armed && current_g > s_theft_threshold)
                {
                    if (!s_alarm_triggered)
                    {
                        // Wykryto nowy atak!
                        s_alarm_triggered = true;
                        alarm_just_triggered = true; // Flaga wymuszenia wysyłki MQTT

                        ESP_LOGE(TAG, "ALARM! Wstrząs: %.2f G. Uruchamiam syrenę lokalnie!", current_g);

                        // Uruchom syrenę natychmiast
                        if (s_audio_ready)
                        {
                            audio_player_loop_track(1);
                            s_alarm_sound_active = true;
                        }
                    }
                }
            }
        }

        // nie ma heartbeatu od serwera przez dłuższy czas
        if ((now_ms - s_last_server_contact) > s_config.heartbeat_interval)
        {
            if (server_is_alive)
            {
                ESP_LOGW(TAG, "Watchdog: Serwer przestał wysyłać Heartbeat. Wstrzymuję publikację.");
                server_is_alive = false;
            }
        }
        else
        {
            if (!server_is_alive)
            {
                ESP_LOGI(TAG, "Watchdog: Serwer wrócił (odebrano Heartbeat). Wznawiam publikację.");
                server_is_alive = true;
            }
        }

        // Warunki wysłania:
        // 1. Klient MQTT połączony
        // 2. Serwer żyje (odbieramy Heartbeat)
        // 3. (Minął czas 5s) LUB (Właśnie wykryto alarm - priorytet)
        if (client != NULL && s_mqtt_connected_to_server && server_is_alive)
        {
            if (alarm_just_triggered || (now_ms - last_publish_time) > PUBLISH_INTERVAL_MS)
            {
                char *status_txt = "DISARMED";
                if (s_alarm_triggered)
                    status_txt = "ROBBERY";
                else if (s_is_armed)
                    status_txt = "ARMED";

                snprintf(payload, sizeof(payload),
                         "{\"status\": \"%s\", \"x\": %.2f, \"y\": %.2f, \"z\": %.2f, \"alarm\": %s}",
                         status_txt, accel.x, accel.y, accel.z,
                         s_alarm_triggered ? "true" : "false");

                // non-blocking (len=0, qos=0) szybciej
                esp_mqtt_client_publish(client, s_topic_data, payload, 0, 0, 0);

                last_publish_time = now_ms;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLING_INTERVAL_MS));
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected_to_server = true;
        ESP_LOGI(TAG, "MQTT Połączono!");

        esp_mqtt_client_subscribe(client, s_topic_config, 0);
        esp_mqtt_client_subscribe(client, TOPIC_HB, 0);

        if (!s_task_started)
        {
            xTaskCreate(publisher_task, "mqtt_publisher", 4096, NULL, 5, &s_publisher_task_handle);
            s_task_started = true;
        }
        else if (s_publisher_task_handle)
        {
            vTaskResume(s_publisher_task_handle);
        }

        if (!s_pinpad_task_started)
        {
            xTaskCreate(pinpad_disarm_task, "pinpad_disarm", 4096, NULL, 5, &s_pinpad_task_handle);
            s_pinpad_task_started = true;
        }
        else if (s_pinpad_task_handle)
        {
            vTaskResume(s_pinpad_task_handle);
        }

        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected_to_server = false;
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Błąd");
        break;
    case MQTT_EVENT_DATA:
    {
        char topic[128];
        snprintf(topic, sizeof(topic), "%.*s", event->topic_len, event->topic);

        if (strcmp(topic, TOPIC_HB) == 0)
        {
            s_last_server_contact = esp_timer_get_time() / 1000;
            server_is_alive = true;
        }
        else if (strcmp(topic, s_topic_config) == 0)
        {
            char payload[256];
            snprintf(payload, sizeof(payload), "%.*s", event->data_len, event->data);
            process_config(payload);
        }
        break;
    }
    default:
        break;
    }
}
void mqtt_app_start(const mqtt_app_config_t *config)
{
    s_config = *config;

    load_pinpad_code();

    if (!s_audio_ready)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS); // chwila na stabilizację systemu przed inicjalizacją audio
        if (audio_player_init() == ESP_OK)
        {
            s_audio_ready = true;
            ESP_LOGI(TAG, "AUDIO: initialized");
        }
        else
        {
            ESP_LOGW(TAG, "AUDIO: init failed");
        }
    }

    if (!s_pinpad_task_started)
    {
        esp_err_t pin_err = pinpad_init();
        if (pin_err != ESP_OK)
        {
            ESP_LOGE(TAG, "PINPAD init failed: %s", esp_err_to_name(pin_err));
        }
    }

    build_uri_with_local(s_config.uri, s_uri_with_local, sizeof(s_uri_with_local));
    ESP_LOGI(TAG, "Inicjalizacja MQTT z URI: %s", s_uri_with_local);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_uri_with_local,
    };

    generate_topics();

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}
void mqtt_send_delete_db(void)
{
    if (client == NULL || !s_mqtt_connected_to_server)
    {
        ESP_LOGW(TAG, "MQTT niepołączony - nie wysyłam DELETE_DB");
        return;
    }
    char *status_txt = "DELETE_DB";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"status\": \"%s\"}",
             status_txt);

    esp_mqtt_client_publish(client, s_topic_data, payload, 0, 0, 0);
    ESP_LOGI(TAG, "Wysłano: %s", payload);
}