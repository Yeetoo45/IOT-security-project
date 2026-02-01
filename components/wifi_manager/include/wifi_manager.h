/**
 * @file wifi_manager.h
 * @brief Zarządzanie połączeniem WiFi (Station + konfiguracja Station przez SoftAP Provisioning)
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

// sygnalizuje że ESP32 połączyło się z routerem i ma adres IP
#define WIFI_CONNECTED_EVENT BIT0

  /**
   * @brief Inicjalizuje menedżera WiFi.
   * * Funkcja sprawdza pamięć NVS:
   * - Jeśli są zapisane dane: Łączy się z domowym WiFi (tryb STA).
   * - Jeśli brak danych: Tworzy Hotspot (SoftAP) o nazwie "ESP32_SETUP_PROV"
   * i uruchamia serwer HTTP do konfiguracji.
   * * @return esp_err_t ESP_OK jeśli start się powiódł, w przeciwnym razie kod błędu.
   */
  void wifi_manager_start(void);

  /**
   * @brief Zwraca uchwyt do grupy zdarzeń WiFi.
   * * Używane do synchronizacji w innych zadaniach (np. oczekiwanie na połączenie
   * przed startem MQTT).
   * * @return EventGroupHandle_t Uchwyt do event group z freertos
   */
  EventGroupHandle_t wifi_get_event_group(void);

  /**
   * @brief Wykonuje "Factory Reset" ustawień WiFi.
   * * Kasuje pamięć NVS (zapomniane hasło WiFi) i restartuje ESP32.
   * Po restarcie urządzenie wejdzie w tryb parowania (SoftAP).
   */
  void wifi_manager_reset_provisioning(void);

  /**
   * @brief Pobiera zapisany URI brokera MQTT w NVS.
   * * @param out_uri Bufor do zapisania URI.
   * * @param max_len Maksymalna długość bufora.
   * * @return esp_err_t ESP_OK jeśli odczyt się powiódł, w przeciwnym razie kod błędu.
   */
  esp_err_t wifi_manager_get_mqtt_uri(char *out_uri, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif