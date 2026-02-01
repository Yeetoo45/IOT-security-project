#include "audio_player.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define DFPLAYER_FRAME_LEN 10

static uart_port_t s_uart_num = UART_NUM_1;
static bool s_audio_initialized = false;

static void dfplayer_send_cmd(uint8_t cmd, uint16_t param, bool ack)
{
    uint8_t buf[DFPLAYER_FRAME_LEN];
    uint8_t ver = 0xFF;
    uint8_t len = 0x06;
    uint8_t ack_byte = ack ? 0x01 : 0x00;
    uint8_t param_hi = (uint8_t)(param >> 8);
    uint8_t param_lo = (uint8_t)(param & 0xFF);

    uint16_t checksum = 0xFFFF - (ver + len + cmd + ack_byte + param_hi + param_lo) + 1;
    uint8_t chk_hi = (uint8_t)(checksum >> 8);
    uint8_t chk_lo = (uint8_t)(checksum & 0xFF);

    buf[0] = 0x7E;
    buf[1] = ver;
    buf[2] = len;
    buf[3] = cmd;
    buf[4] = ack_byte;
    buf[5] = param_hi;
    buf[6] = param_lo;
    buf[7] = chk_hi;
    buf[8] = chk_lo;
    buf[9] = 0xEF;

    uart_write_bytes(s_uart_num, (const char *)buf, DFPLAYER_FRAME_LEN);
}

static void dfplayer_set_volume(uint8_t volume)
{
    if (volume > 30)
    {
        volume = 30;
    }
    dfplayer_send_cmd(0x06, volume, true);
}

static void dfplayer_select_tf(void)
{
    dfplayer_send_cmd(0x09, 0x0002, true);
}

static void dfplayer_reset(void)
{
    dfplayer_send_cmd(0x0C, 0x0000, true);
}

static bool dfplayer_read_frame(uint8_t *out_buf)
{
    uint8_t byte = 0;
    for (int i = 0; i < 50; i++)
    {
        if (uart_read_bytes(s_uart_num, &byte, 1, pdMS_TO_TICKS(20)) == 1)
        {
            if (byte == 0x7E)
            {
                out_buf[0] = byte;
                int rest = uart_read_bytes(s_uart_num, out_buf + 1, DFPLAYER_FRAME_LEN - 1, pdMS_TO_TICKS(50));
                return rest == DFPLAYER_FRAME_LEN - 1;
            }
        }
    }

    return false;
}

static void dfplayer_dump_feedback(void)
{
    uint8_t resp[DFPLAYER_FRAME_LEN];
    if (!dfplayer_read_frame(resp))
    {
        printf("DFPlayer RX: (none)\n");
        return;
    }

    char line[64];
    int pos = 0;
    for (int i = 0; i < DFPLAYER_FRAME_LEN && pos < (int)sizeof(line) - 3; i++)
    {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", resp[i]);
    }
    line[pos] = '\0';
    printf("DFPlayer RX: %s\n", line);
}

static void dfplayer_query_tf_file_count(void)
{
    dfplayer_send_cmd(0x48, 0x0000, false);
    dfplayer_dump_feedback();
}

esp_err_t audio_player_init(void)
{
    s_uart_num = (uart_port_t)CONFIG_DFPLAYER_UART_NUM;

    uart_config_t uart_config = {
        .baud_rate = CONFIG_DFPLAYER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(s_uart_num, &uart_config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_set_pin(s_uart_num, CONFIG_DFPLAYER_UART_TX_GPIO, CONFIG_DFPLAYER_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_driver_install(s_uart_num, 256, 0, 0, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(1200));
    dfplayer_reset();
    vTaskDelay(pdMS_TO_TICKS(1200));
    dfplayer_select_tf();
    dfplayer_dump_feedback();
    vTaskDelay(pdMS_TO_TICKS(200));
    dfplayer_set_volume((uint8_t)CONFIG_DFPLAYER_VOLUME);
    dfplayer_dump_feedback();
    dfplayer_query_tf_file_count();

    s_audio_initialized = true;
    return ESP_OK;
}

esp_err_t audio_player_play_track(uint16_t track)
{
    if (!s_audio_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (track == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    dfplayer_send_cmd(0x03, track, true);
    dfplayer_dump_feedback();
    return ESP_OK;
}

esp_err_t audio_player_loop_track(uint16_t track)
{
    if (!s_audio_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (track == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    dfplayer_send_cmd(0x08, track, true);
    dfplayer_dump_feedback();
    return ESP_OK;
}

esp_err_t audio_player_stop(void)
{
    if (!s_audio_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    dfplayer_send_cmd(0x16, 0x0000, true);
    dfplayer_dump_feedback();
    return ESP_OK;
}
