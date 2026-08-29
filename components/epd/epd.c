#include "epd.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* Waveform LUTs lifted verbatim from Waveshare's port_display.cpp; they are
 * panel-specific and there is no documented way to derive them. */
static const uint8_t LUT_FULL[159] = {
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x08, 0x01, 0x00, 0x02,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00,
    0x00, 0x00, 0x22, 0x17, 0x41, 0x00, 0x32, 0x20,
};

static const uint8_t LUT_PARTIAL[159] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x02,
    0x17, 0x41, 0xB0, 0x32, 0x28,
};

static spi_device_handle_t s_spi;
static uint8_t            *s_fb;              /* EPD_BUF_SIZE, DMA-capable */
static uint8_t            *s_bounce;          /* for the flash-resident LUTs */
#define BOUNCE_SIZE 192
static uint32_t            s_partial_count;
static bool                s_force_full;
static bool                s_asleep;
static bool                s_partial_mode;

static void wait_busy(void)
{
    /* BUSY is high while the panel is refreshing. A full refresh takes ~2 s;
     * bail out after 10 s rather than hanging the UI task forever. */
    for (int i = 0; i < 1000; i++) {
        if (gpio_get_level(BD_EPD_BUSY_PIN) == 0) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "BUSY stuck high");
}

static void spi_write(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = 8 * len,
        .tx_buffer = data,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void send_cmd(uint8_t cmd)
{
    gpio_set_level(BD_EPD_DC_PIN, 0);
    gpio_set_level(BD_EPD_CS_PIN, 0);
    spi_write(&cmd, 1);
    gpio_set_level(BD_EPD_CS_PIN, 1);
}

static void send_data(const uint8_t *data, size_t len)
{
    /* The waveform tables are const and therefore live in flash, which the
     * SPI DMA engine cannot read from. Copy those through a small internal
     * buffer; the framebuffer is already DMA-capable and goes straight out. */
    if (!esp_ptr_dma_capable(data)) {
        assert(len <= BOUNCE_SIZE);
        memcpy(s_bounce, data, len);
        data = s_bounce;
    }
    gpio_set_level(BD_EPD_DC_PIN, 1);
    gpio_set_level(BD_EPD_CS_PIN, 0);
    spi_write(data, len);
    gpio_set_level(BD_EPD_CS_PIN, 1);
}

static void send_byte(uint8_t b) { send_data(&b, 1); }

static void hw_reset(void)
{
    gpio_set_level(BD_EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BD_EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BD_EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    wait_busy();
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    send_cmd(0x44);
    send_byte((x0 >> 3) & 0xFF);
    send_byte((x1 >> 3) & 0xFF);

    send_cmd(0x45);
    send_byte(y0 & 0xFF);
    send_byte((y0 >> 8) & 0xFF);
    send_byte(y1 & 0xFF);
    send_byte((y1 >> 8) & 0xFF);
}

static void set_cursor(uint16_t x, uint16_t y)
{
    send_cmd(0x4E);
    send_byte(x & 0xFF);
    send_cmd(0x4F);
    send_byte(y & 0xFF);
    send_byte((y >> 8) & 0xFF);
}

static void load_lut(const uint8_t *lut)
{
    send_cmd(0x32);
    send_data(lut, 153);
    wait_busy();

    send_cmd(0x3F);
    send_byte(lut[153]);
    send_cmd(0x03);
    send_byte(lut[154]);
    send_cmd(0x04);
    send_byte(lut[155]);
    send_byte(lut[156]);
    send_byte(lut[157]);
    send_cmd(0x2C);
    send_byte(lut[158]);
}

static void panel_init_full(void)
{
    hw_reset();

    send_cmd(0x12);   /* SWRESET */
    wait_busy();

    send_cmd(0x01);   /* driver output control: 200 lines */
    send_byte(0xC7);
    send_byte(0x00);
    send_byte(0x01);

    send_cmd(0x11);   /* data entry mode: X inc, Y dec */
    send_byte(0x01);

    set_window(0, EPD_W - 1, EPD_H - 1, 0);

    send_cmd(0x3C);   /* border waveform */
    send_byte(0x01);

    send_cmd(0x18);   /* use internal temperature sensor */
    send_byte(0x80);

    send_cmd(0x22);
    send_byte(0xB1);
    send_cmd(0x20);

    set_cursor(0, EPD_H - 1);
    wait_busy();

    load_lut(LUT_FULL);
    s_partial_mode = false;
}

static void panel_init_partial(void)
{
    hw_reset();
    load_lut(LUT_PARTIAL);

    send_cmd(0x37);
    for (int i = 0; i < 10; i++) {
        send_byte(i == 5 ? 0x40 : 0x00);
    }

    send_cmd(0x3C);
    send_byte(0x80);

    send_cmd(0x22);
    send_byte(0xC0);
    send_cmd(0x20);
    wait_busy();
    s_partial_mode = true;
}

static void turn_on(bool partial)
{
    send_cmd(0x22);
    send_byte(partial ? 0xCF : 0xC7);
    send_cmd(0x20);
    wait_busy();
}

esp_err_t epd_init(void)
{
    s_fb = heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_bounce = heap_caps_malloc(BOUNCE_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_fb && s_bounce, ESP_ERR_NO_MEM, TAG, "framebuffer alloc");
    memset(s_fb, 0xFF, EPD_BUF_SIZE);

    board_power_epd(true);
    vTaskDelay(pdMS_TO_TICKS(10));

    spi_bus_config_t bus = {
        .miso_io_num = -1,
        .mosi_io_num = BD_EPD_MOSI_PIN,
        .sclk_io_num = BD_EPD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_SIZE + 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BD_EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    /* CS is driven by hand: the panel wants it held low across a command and
     * its data bytes, which the SPI driver's per-transaction CS cannot do. */
    spi_device_interface_config_t dev = {
        .spics_io_num = -1,
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .queue_size = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(BD_EPD_SPI_HOST, &dev, &s_spi), TAG, "spi dev");

    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BD_EPD_RST_PIN) | (1ULL << BD_EPD_DC_PIN) | (1ULL << BD_EPD_CS_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "epd gpio");

    gpio_config_t in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BD_EPD_BUSY_PIN,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "busy gpio");

    gpio_set_level(BD_EPD_CS_PIN, 1);
    gpio_set_level(BD_EPD_RST_PIN, 1);

    /* Full init, push a white frame into both RAM banks so partial refreshes
     * have a valid reference image, then switch to partial mode. */
    panel_init_full();
    send_cmd(0x24);
    send_data(s_fb, EPD_BUF_SIZE);
    send_cmd(0x26);
    send_data(s_fb, EPD_BUF_SIZE);
    turn_on(false);
    panel_init_partial();

    ESP_LOGI(TAG, "panel ready (%dx%d, 1bpp)", EPD_W, EPD_H);
    return ESP_OK;
}

void epd_blit(const uint8_t *frame) { memcpy(s_fb, frame, EPD_BUF_SIZE); }

void epd_clear(void) { memset(s_fb, 0xFF, EPD_BUF_SIZE); }

void epd_request_full_refresh(void) { s_force_full = true; }

void epd_flush(epd_refresh_t mode)
{
    if (s_asleep) {
        epd_wake();
    }

    bool full = (mode == EPD_REFRESH_FULL) || s_force_full ||
                (s_partial_count >= EPD_FULL_REFRESH_EVERY);

    if (full) {
        panel_init_full();
        send_cmd(0x24);
        send_data(s_fb, EPD_BUF_SIZE);
        send_cmd(0x26);
        send_data(s_fb, EPD_BUF_SIZE);
        turn_on(false);
        panel_init_partial();
        s_partial_count = 0;
        s_force_full = false;
    } else {
        if (!s_partial_mode) {
            panel_init_partial();
        }
        send_cmd(0x24);
        send_data(s_fb, EPD_BUF_SIZE);
        turn_on(true);
        s_partial_count++;
    }
}

void epd_sleep(void)
{
    if (s_asleep) {
        return;
    }

    /* Power off before the deep sleep command. panel_init_partial() leaves the
     * clock and the analog block enabled (0x22 0xC0), so without this the
     * charge pump is still holding +/-15 V on the source drivers when the rail
     * goes away, and the last frame smears into a patchy black. */
    send_cmd(0x22);
    send_byte(0x83);   /* disable analog, disable clock */
    send_cmd(0x20);
    wait_busy();

    send_cmd(0x10);   /* deep sleep mode 1 */
    send_byte(0x01);

    /* Let the internal rails bleed off before the caller cuts VCC. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Float the control lines: a pad still driven high back-feeds the panel
     * through its ESD diodes and keeps the COG on a supply that is neither on
     * nor off. (SCK idles low on its own; MOSI is the SPI driver's to hold.) */
    gpio_config_t idle = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_DISABLE,
        .pin_bit_mask = (1ULL << BD_EPD_RST_PIN) | (1ULL << BD_EPD_DC_PIN) |
                        (1ULL << BD_EPD_CS_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&idle);

    s_asleep = true;
    s_partial_mode = false;
}

void epd_wake(void)
{
    if (!s_asleep) {
        return;
    }
    s_asleep = false;

    /* Undo the floating done in epd_sleep(), then bring the rail back: the
     * panel came out of deep sleep only in the sense that we are about to
     * reset it. */
    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BD_EPD_RST_PIN) | (1ULL << BD_EPD_DC_PIN) |
                        (1ULL << BD_EPD_CS_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&out);
    gpio_set_level(BD_EPD_CS_PIN, 1);
    gpio_set_level(BD_EPD_RST_PIN, 1);
    board_power_epd(true);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Leaving deep sleep needs a hardware reset and a fresh full init; the
     * next flush will be a full one so the reference image is rebuilt. */
    panel_init_full();
    s_force_full = true;
}
