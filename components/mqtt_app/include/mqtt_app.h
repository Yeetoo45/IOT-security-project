/**
 * @file mqtt_app.h
 * @brief Zarządzanie połączeniem MQTT (konfiguracja i obsługa połączenia MQTT)
 */

#ifndef MQTT_APP_H
#define MQTT_APP_H
#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

    extern volatile bool s_alarm_triggered; // Czy system czuwa?
    extern volatile bool server_is_alive;

    /**
     * @brief Struktura konfiguracji aplikacji MQTT.
     * Zawiera parametry potrzebne do inicjalizacji i działania klienta MQTT.
     */
    typedef struct
    {
        const char *uri;
        int64_t heartbeat_interval;
        int64_t deep_sleep_interval;
        float theft_threshold;
    } mqtt_app_config_t;

    /**
     * @brief Inicjalizuje i uruchamia klienta MQTT z podaną konfiguracją.
     * * @param config Wskaźnik na strukturę z konfiguracją MQTT.
     */
    void mqtt_app_start(const mqtt_app_config_t *config);

    /**
     * @brief Sprawdza, czy alarm kradzieży jest aktywny.
     * * @return bool true jeśli alarm jest aktywny, false w przeciwnym razie.
     */
    bool mqtt_app_is_alarm_active(void);

    /**
     * @brief Resetuje stan alarmu kradzieży.
     */
    void mqtt_app_reset_alarm(void);

    /**
     * @brief Wysyła do serwera żądanie usunięcia danych urządzenia z bazy.
     */
    void mqtt_send_delete_db(void);

    /**
     * @brief Ustawia aktualne napięcie baterii (mV) do wysyłki MQTT.
     * @param battery_mv Napięcie baterii w mV.
     */
    void mqtt_app_set_battery_mv(float battery_mv);

#ifdef __cplusplus
}
#endif
#endif