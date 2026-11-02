#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"

static const char *TAG = "sscma";

#define UART_PORT       UART_NUM_1
#define UART_TX_PIN     4
#define UART_RX_PIN     5
#define UART_BAUD       921600

#define LINE_CAP        (64 * 1024)   // one JSON line incl. base64 image
#define JPG_CAP         (48 * 1024)   // decoded (binary) JPEG from the module
#define OUT_JPG_CAP     (48 * 1024)   // re-encoded JPEG
#define OUT_B64_CAP     (66 * 1024)

static char    *line;
static uint8_t *jpg_in;
static uint8_t *jpg_out;
static char    *b64_out;

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 16384, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// Fill a box (center x,y and size w,h) with black in an RGB888 buffer
static void blackout_box(uint8_t *buf, int W, int H, int cx, int cy, int bw, int bh)
{
    int x0 = cx - bw / 2, x1 = cx + bw / 2;
    int y0 = cy - bh / 2, y1 = cy + bh / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    if (x1 < x0 || y1 < y0) return;
    for (int y = y0; y <= y1; y++) {
        memset(&buf[(y * W + x0) * 3], 0, (x1 - x0 + 1) * 3);
    }
}

static void process_frame(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "bad json"); return; }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *img  = data ? cJSON_GetObjectItem(data, "image") : NULL;
    if (!cJSON_IsString(img)) { cJSON_Delete(root); return; }

    // Coordinate space the boxes are in (240x240 for your model)
    int res_w = 240, res_h = 240;
    cJSON *res = cJSON_GetObjectItem(data, "resolution");
    if (cJSON_GetArraySize(res) == 2) {
        res_w = cJSON_GetArrayItem(res, 0)->valueint;
        res_h = cJSON_GetArrayItem(res, 1)->valueint;
    }

    // base64 -> JPEG bytes
    size_t jpg_len = 0;
    if (mbedtls_base64_decode(jpg_in, JPG_CAP, &jpg_len,
                              (const uint8_t *)img->valuestring,
                              strlen(img->valuestring)) != 0) {
        ESP_LOGW(TAG, "base64 decode failed (corrupt line or buffer too small)");
        cJSON_Delete(root);
        return;
    }

    // JPEG -> RGB888
    jpeg_dec_config_t dc = DEFAULT_JPEG_DEC_CONFIG();
    dc.output_type = JPEG_PIXEL_FORMAT_RGB888;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&dc, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed");
        cJSON_Delete(root);
        return;
    }
    jpeg_dec_io_t io = {0};
    jpeg_dec_header_info_t hdr = {0};
    io.inbuf = jpg_in;
    io.inbuf_len = jpg_len;
    if (jpeg_dec_parse_header(dec, &io, &hdr) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg header parse failed");
        jpeg_dec_close(dec);
        cJSON_Delete(root);
        return;
    }
    int W = hdr.width, H = hdr.height;
    uint8_t *rgb = jpeg_calloc_align(W * H * 3, 16);
    if (!rgb) {
        ESP_LOGE(TAG, "out of memory for %dx%d frame, is PSRAM enabled?", W, H);
        jpeg_dec_close(dec);
        cJSON_Delete(root);
        return;
    }
    io.outbuf = rgb;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg decode failed");
    }
    jpeg_dec_close(dec);

    // Black out every box
    cJSON *boxes = cJSON_GetObjectItem(data, "boxes");
    cJSON *b;
    cJSON_ArrayForEach(b, boxes) {
        if (cJSON_GetArraySize(b) < 6) continue;   // [x, y, w, h, score, target]
        int cx = cJSON_GetArrayItem(b, 0)->valueint;
        int cy = cJSON_GetArrayItem(b, 1)->valueint;
        int bw = cJSON_GetArrayItem(b, 2)->valueint;
        int bh = cJSON_GetArrayItem(b, 3)->valueint;
        int score  = cJSON_GetArrayItem(b, 4)->valueint;
        int target = cJSON_GetArrayItem(b, 5)->valueint;

        // ---- your coordinate logic goes here ----
        ESP_LOGI(TAG, "target=%d score=%d x=%d y=%d w=%d h=%d", target, score, cx, cy, bw, bh);

        blackout_box(rgb, W, H,
                     cx * W / res_w, cy * H / res_h,
                     bw * W / res_w, bh * H / res_h);
    }

    // RGB888 -> JPEG
    jpeg_enc_config_t ec = DEFAULT_JPEG_ENC_CONFIG();
    ec.width = W;
    ec.height = H;
    ec.src_type = JPEG_PIXEL_FORMAT_RGB888;
    ec.subsampling = JPEG_SUBSAMPLE_420;
    ec.quality = 80;
    ec.rotate = JPEG_ROTATE_0D;
    ec.task_enable = false;
    jpeg_enc_handle_t enc = NULL;
    int out_len = 0;
    if (jpeg_enc_open(&ec, &enc) == JPEG_ERR_OK) {
        if (jpeg_enc_process(enc, rgb, W * H * 3, jpg_out, OUT_JPG_CAP, &out_len) == JPEG_ERR_OK) {
            size_t b64_len = 0;
            mbedtls_base64_encode((unsigned char *)b64_out, OUT_B64_CAP, &b64_len, jpg_out, out_len);
            b64_out[b64_len] = '\0';
            printf("MASKED_BEGIN\n%s\nMASKED_END\n", b64_out);
        } else {
            ESP_LOGW(TAG, "jpeg encode failed");
        }
        jpeg_enc_close(enc);
    }

    jpeg_free_align(rgb);
    cJSON_Delete(root);
}

void app_main(void)
{
    line    = big_alloc(LINE_CAP);
    jpg_in  = big_alloc(JPG_CAP);
    jpg_out = big_alloc(OUT_JPG_CAP);
    b64_out = big_alloc(OUT_B64_CAP);
    if (!line || !jpg_in || !jpg_out || !b64_out) {
        ESP_LOGE(TAG, "buffer allocation failed, enable PSRAM");
        return;
    }

    uart_init();
    uart_flush_input(UART_PORT);

    const char *cmd = "AT+INVOKE=1,0,0\r\n";   // one inference, image included
    uart_write_bytes(UART_PORT, cmd, strlen(cmd));

    size_t pos = 0;
    uint8_t chunk[256];
    int idle_ms = 0;

    while (1) {
        int n = uart_read_bytes(UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (n <= 0) {
            idle_ms += 20;
            if (idle_ms >= 5000) {            // nothing for 5 s, ask again
                idle_ms = 0;
                pos = 0;
                uart_write_bytes(UART_PORT, cmd, strlen(cmd));
            }
            continue;
        }
        idle_ms = 0;

        for (int i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            if (c == '\n') {
                line[pos] = '\0';
                if (pos > 0 && line[0] == '{' && strstr(line, "\"image\"")) {
                    process_frame(line);
                    uart_write_bytes(UART_PORT, cmd, strlen(cmd));  // request next frame
                }
                pos = 0;
            } else if (c != '\r') {
                if (pos < LINE_CAP - 1) line[pos++] = c;
                else pos = 0;                 // overflow, drop the line
            }
        }
    }
}
