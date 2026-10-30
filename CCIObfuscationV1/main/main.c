#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "sscma";

#define UART_PORT   UART_NUM_1
#define UART_TX_PIN 4
#define UART_RX_PIN 5
#define UART_BAUD   115200
#define SSCMA_LINE_MAX    4096

static void handle_line(const char *line)
{
    const char *start = strchr(line, '{');
    if (!start) return;

    cJSON *root = cJSON_Parse(start);
    if (!root) return;

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && code->valueint != 0) {
        ESP_LOGE(TAG, "device error, code=%d", code->valueint);
    }

    if (type && type->valueint == 1) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *boxes = data ? cJSON_GetObjectItem(data, "boxes") : NULL;
        cJSON *box;
        cJSON_ArrayForEach(box, boxes) {
            if (cJSON_GetArraySize(box) < 6) continue;   // [x, y, w, h, score, target]
            int x = cJSON_GetArrayItem(box, 0)->valueint;
            int y = cJSON_GetArrayItem(box, 1)->valueint;
            int w = cJSON_GetArrayItem(box, 2)->valueint;
            int h = cJSON_GetArrayItem(box, 3)->valueint;
            int score = cJSON_GetArrayItem(box, 4)->valueint;
            int target = cJSON_GetArrayItem(box, 5)->valueint;

            // Do your coordinate logic here (x, y are the box center)
            ESP_LOGI(TAG, "target=%d score=%d x=%d y=%d w=%d h=%d",
                     target, score, x, y, w, h);
        }
    }
    cJSON_Delete(root);
}

void uart_innit(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 8192, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

}

void app_main(void)
{
    uart_innit();

    uart_flush_input(UART_PORT);

    static char line[SSCMA_LINE_MAX];
    size_t pos = 0;
    uint8_t c;
    int idle_ms = 0;

    ESP_LOGI(TAG, "UART ready, TX=%d RX=%d baud=%d", UART_TX_PIN, UART_RX_PIN, UART_BAUD);

    // Simple query first, to check that the module answers at all
    const char *probe = "AT+ID?\r\n";
    uart_write_bytes(UART_PORT, probe, strlen(probe));

    while (1) {
        int n = uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(10));
        if (n <= 0) {
            idle_ms += 10;
            if (idle_ms >= 2000) {
                idle_ms = 0;
                ESP_LOGW(TAG, "no data in 2s, resending commands");
                uart_write_bytes(UART_PORT, probe, strlen(probe));
                const char *inv = "AT+INVOKE=1,0,1\r\n";
                uart_write_bytes(UART_PORT, inv, strlen(inv));
            }
            continue;
        }
        idle_ms = 0;

        if (c == '\n') {
            line[pos] = '\0';
            if (pos > 0) {
                ESP_LOGI(TAG, "RX: %s", line);   // raw line, so we can see everything
                handle_line(line);
            }
            pos = 0;
        } else if (c != '\r' && pos < SSCMA_LINE_MAX - 1) {
            line[pos++] = c;
        } else if (pos >= SSCMA_LINE_MAX - 1) {
            pos = 0;
        }
    }
}
