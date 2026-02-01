#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t audio_player_init(void);
    esp_err_t audio_player_play_track(uint16_t track);
    esp_err_t audio_player_loop_track(uint16_t track);
    esp_err_t audio_player_stop(void);

#ifdef __cplusplus
}
#endif
