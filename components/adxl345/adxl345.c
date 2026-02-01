#include "adxl345.h"
#include "esp_log.h"
#include <math.h>

// Rejestry ADXL345

// Rejestr ID urządzenia (Tylko do odczytu). Służy do weryfikacji, czy chip to na pewno ADXL345.
#define ADXL345_REG_DEVID 0x00

// Kontrola przepustowości i prędkości (Data Rate). Tu ustawiamy częstotliwość próbkowania (np. 100Hz, 3200Hz).
#define ADXL345_REG_BW_RATE 0x2C

// Zarządzanie zasilaniem. Najważniejszy jest tu bit 'Measure' (D3) - bez niego czujnik śpi.
#define ADXL345_REG_POWER_CTL 0x2D

// Format danych. Tu konfigurujemy zakres (2g/4g/8g/16g), tryb Full Resolution i wyrównanie bitów.
#define ADXL345_REG_DATA_FORMAT 0x31

// Pierwszy bajt danych (Oś X, LSB). Od tego adresu zaczynamy ciągły odczyt 6 bajtów (X, Y, Z).
#define ADXL345_REG_DATAX0 0x32

// Oczekiwana wartość w rejestrze DEVID (Fabryczny "podpis" czujnika: 11100101).
#define ADXL345_DEVID_VAL 0xE5

// Maska bitu D3 w rejestrze DATA_FORMAT. Włącza tryb stałej rozdzielczości (~3.9mg/LSB) niezależnie od zakresu.
#define ADXL345_FULL_RES_BIT 0x08

static const char *TAG = "ADXL345";

struct adxl345_context_t
{
    i2c_port_t bus;     // Numer portu I2C
    uint16_t addr;      // Adres I2C urządzenia
    float scale_factor; // Mnożnik do przeliczania surowych danych na grawitację
};

// Zapis pojedynczego rejestru
static esp_err_t write_reg(adxl345_handle_t handle, uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    return i2c_master_write_to_device(handle->bus, handle->addr, data, sizeof(data), pdMS_TO_TICKS(100));
}

// Odczyt pojedynczego rejestru
static esp_err_t read_reg(adxl345_handle_t handle, uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(handle->bus, handle->addr, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

// Odczyt wielu bajtów z rzędu
static esp_err_t read_bytes(adxl345_handle_t handle, uint8_t reg, uint8_t *buffer, size_t len)
{
    return i2c_master_write_read_device(handle->bus, handle->addr, &reg, 1, buffer, len, pdMS_TO_TICKS(100));
}

adxl345_handle_t adxl345_create(i2c_port_t port, uint16_t dev_addr)
{
    // alokacja pamięci dla struktury
    // zawiera numer portu I2C, adres urządzenia oraz mnożnik do przeliczania surowych danych na grawitację
    struct adxl345_context_t *dev = (struct adxl345_context_t *)calloc(1, sizeof(struct adxl345_context_t));

    if (!dev)
    {
        ESP_LOGE(TAG, "Brak pamieci na strukture ADXL345");
        return NULL;
    }

    dev->bus = port;
    dev->addr = dev_addr;
    dev->scale_factor = 0.0039f; // 0.004g precyzji (tryb Full Res)

    uint8_t devid;
    esp_err_t err = read_reg(dev, ADXL345_REG_DEVID, &devid); // Odczyt ID urządzenia z rejestru DEVID(0x00)
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Blad komunikacji I2C");
        free(dev);
        return NULL;
    }

    if (devid != ADXL345_DEVID_VAL) // Sprawdzenie czy odczytane ID jest zgodne z oczekiwanym (0xE5) - w każdym adxl jest takie samo
    {
        ESP_LOGE(TAG, "Nieprawidlowe ID urzadzenia: 0x%02X (oczekiwano 0x%02X)", devid, ADXL345_DEVID_VAL);
        free(dev);
        return NULL;
    }

    // adxl jest domyślnie w trybie uśpienia, więc trzeba go obudzić ustawiając bit D3 rejestru POWER_CTL(0x2D)
    // 0X08 = ustawienie bitu D3 (Measure) na 1 co włącza pomiary
    write_reg(dev, ADXL345_REG_POWER_CTL, 0x08);

    // Ustaw domyślny zakres (np. 2G)
    // inne zakresy range np 4G, 8G, 16G
    adxl345_set_range(dev, ADXL345_RANGE_2G);

    ESP_LOGI(TAG, "ADXL345 zainicjalizowany poprawnie");
    return dev;
}

void adxl345_delete(adxl345_handle_t handle)
{
    if (handle)
    {
        free(handle);
    }
}

esp_err_t adxl345_set_range(adxl345_handle_t handle, adxl345_range_t range)
{
    if (!handle)
        return ESP_ERR_INVALID_ARG;

    uint8_t current_val;

    // doczyt aktualnej wartości rejestru DATA_FORMAT(0x31)
    // DATA_FORMAT zawiera:
    // bit D3 - Full Resolution
    // bity D1-D0: Range (00=2G, 01=4G, 10=8G, 11=16G)
    // bit D4 nic
    // bit D5 - INT_INVERT
    // bit D6 - SPI
    // bit D7 - SELF_TEST
    esp_err_t err = read_reg(handle, ADXL345_REG_DATA_FORMAT, &current_val);
    if (err != ESP_OK)
        return err;

    // zerujebity zakresu (D3-D0)
    current_val &= ~0x0F;

    // Ustawiamy Full Resolution oraz bity zakresu
    // full res oznacza, że skala jest zawsze 0.0004g/LSB niezależnie od zakresu
    current_val |= (ADXL345_FULL_RES_BIT | range);
    handle->scale_factor = 0.0039f;

    return write_reg(handle, ADXL345_REG_DATA_FORMAT, current_val);
}

esp_err_t adxl345_get_accel(adxl345_handle_t handle, adxl345_accel_t *out_data)
{
    if (!handle || !out_data)
        return ESP_ERR_INVALID_ARG;

    uint8_t raw[6];

    // Odczyt 6 bajtów danych z rejestrów DATAX0(0x32) do DATAZ1(0x37)
    // każdy kanał (X, Y, Z) to 2 bajty (LSB, MSB)
    // szybciej niż po kolei czytać każdy rejestr osobno
    esp_err_t err = read_bytes(handle, ADXL345_REG_DATAX0, raw, 6);
    if (err != ESP_OK)
        return err;

    // Konwersja 2x uint8_t na int16_t (LSB, MSB)
    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);

    out_data->x = (float)x * handle->scale_factor;
    out_data->y = (float)y * handle->scale_factor;
    out_data->z = (float)z * handle->scale_factor;

    return ESP_OK;
}