#include "pinpad.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

// Konfiguracja pinów
static const gpio_num_t R[] = {
    (gpio_num_t)CONFIG_PINPAD_ROW0_GPIO,
    (gpio_num_t)CONFIG_PINPAD_ROW1_GPIO,
    (gpio_num_t)CONFIG_PINPAD_ROW2_GPIO,
    (gpio_num_t)CONFIG_PINPAD_ROW3_GPIO,
};
static const gpio_num_t C[] = {
    (gpio_num_t)CONFIG_PINPAD_COL0_GPIO,
    (gpio_num_t)CONFIG_PINPAD_COL1_GPIO,
    (gpio_num_t)CONFIG_PINPAD_COL2_GPIO,
};
static const char KEYMAP[] = "123456789*0#";

#define ROW_CNT (sizeof(R) / sizeof(R[0]))
#define COL_CNT (sizeof(C) / sizeof(C[0]))

// Funkcja pomocnicza: Skanuje fizycznie matrycę i zwraca znak lub 0
static char pinpad_scan_raw(void)
{
    char key = 0;

    for (int r = 0; r < ROW_CNT; r++)
    {
        // Aktywuj wiersz (Stan niski 0, reszta wysoka 1)
        for (int i = 0; i < ROW_CNT; i++)
            gpio_set_level(R[i], (i == r) ? 0 : 1);

        // Krótkie opóźnienie, by sygnał się ustalił (ważne przy długich kablach)
        esp_rom_delay_us(50);

        // Sprawdź kolumny
        for (int c = 0; c < COL_CNT; c++)
        {
            if (gpio_get_level(C[c]) == 0)
            { // Jeśli 0, to zwarcie (znaczy wciśnięty)
                key = KEYMAP[r * COL_CNT + c];
                break;
            }
        }

        // 3. Dezaktywuj wiersz (Powrót do 1)
        gpio_set_level(R[r], 1);

        if (key)
            break; // Znaleziono, przerywamy skanowanie
    }
    return key;
}

esp_err_t pinpad_init(void)
{
    // Konfiguracja Wierszy (Wyjścia)
    for (int i = 0; i < ROW_CNT; i++)
    {
        gpio_reset_pin(R[i]);
        gpio_set_direction(R[i], GPIO_MODE_OUTPUT);
        gpio_set_level(R[i], 1); // Domyślnie wysoki
    }

    // Konfiguracja Kolumn (Wejścia z Pull-Up)
    for (int i = 0; i < COL_CNT; i++)
    {
        gpio_reset_pin(C[i]);
        gpio_set_direction(C[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(C[i], GPIO_PULLUP_ONLY);
    }
    return ESP_OK;
}

// Główna funkcja blokująca
bool pinpad_wait_key(char *out_key, TickType_t ticks_to_wait)
{
    if (out_key == NULL)
    {
        return false;
    }

    TickType_t start_tick = xTaskGetTickCount();

    while (1)
    {
        // Sprawdź timeout
        if (ticks_to_wait != portMAX_DELAY && (xTaskGetTickCount() - start_tick > ticks_to_wait))
        {
            return false;
        }

        // Skanuj
        char k = pinpad_scan_raw();

        // Jeżeli wciśnięto klawisz
        if (k != 0)
        {

            // Debounce (proste opóźnienie 40ms)
            vTaskDelay(pdMS_TO_TICKS(40));

            // Sprawdź ponownie, czy to nie zakłócenie
            if (pinpad_scan_raw() == k)
            {
                *out_key = k;

                // czekaj na puszczenie przycisku (żeby nie wpisać "55555" jednym naciśnięciem)
                while (pinpad_scan_raw() != 0)
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                vTaskDelay(pdMS_TO_TICKS(20)); // Mały debounce przy puszczaniu

                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}