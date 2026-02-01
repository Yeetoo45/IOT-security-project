#ifndef ADXL345_H
#define ADXL345_H

#include "driver/i2c.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C"
{
#endif
#endif
    // Definicja uchwytu do urządzenia
    typedef struct adxl345_context_t *adxl345_handle_t;

    // Struktura z wynikami
    typedef struct
    {
        float x;
        float y;
        float z;
    } adxl345_accel_t;

    // Dostępne zakresy pomiarowe
    typedef enum
    {
        ADXL345_RANGE_2G = 0,
        ADXL345_RANGE_4G = 1,
        ADXL345_RANGE_8G = 2,
        ADXL345_RANGE_16G = 3
    } adxl345_range_t;

    /**
     * @brief Tworzy i inicjalizuje instancję sterownika ADXL345
     * @param port Numer portu I2C (np. I2C_NUM_0)
     * @param dev_addr Adres urządzenia
     * @return adxl345_handle_t Uchwyt do urządzenia lub NULL w przypadku błędu
     */
    adxl345_handle_t adxl345_create(i2c_port_t port, uint16_t dev_addr);

    /**
     * @brief Usuwa instancję sterownika i zwalnia pamięć
     */
    void adxl345_delete(adxl345_handle_t handle);

    /**
     * @brief Ustawia zakres pomiarowy (np. 2G, 16G)
     */
    esp_err_t adxl345_set_range(adxl345_handle_t handle, adxl345_range_t range);

    /**
     * @brief Odczytuje przeliczone dane akcelerometru (w jednostkach g)
     */
    esp_err_t adxl345_get_accel(adxl345_handle_t handle, adxl345_accel_t *out_data);

#ifdef __cplusplus
}

#endif