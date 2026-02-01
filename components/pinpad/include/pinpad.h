#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Konfiguruje piny GPIO dla klawiatury (Wiersze: Output, Kolumny: Input Pullup)
     */
    esp_err_t pinpad_init(void);

    /**
     * @brief Czeka na wciśnięcie klawisza.
     * * Funkcja blokuje zadanie (task) do momentu wciśnięcia klawisza lub upłynięcia czasu.
     * zawiera  debounce i obsługę puszczania klawisza.
     * * @param out_key Wskaźnik, gdzie zostanie zapisany znak ('0'-'9', '*', '#')
     * @param ticks_to_wait Czas oczekiwania w tickach FreeRTOS (portMAX_DELAY = czekaj bez końca)
     * @return true jeśli odebrano znak, false jeśli timeout
     */
    bool pinpad_wait_key(char *out_key, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif