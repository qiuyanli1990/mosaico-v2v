#include "bsp/esp_mosaico.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"

esp_err_t __real_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                        const esp_lcd_panel_io_spi_config_t *config,
                                        esp_lcd_panel_io_handle_t *out);

/* The pinned BSP disables PSRAM DMA. Its ten queued color chunks can then
 * allocate >75 KiB of internal DMA bounce buffers after Wi-Fi has started.
 * Keep the fix local to this application's CO5300 transport. */
esp_err_t __wrap_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                        const esp_lcd_panel_io_spi_config_t *config,
                                        esp_lcd_panel_io_handle_t *out)
{
    if (!config || bus != (esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST || !config->flags.quad_mode) {
        return __real_esp_lcd_new_panel_io_spi(bus, config, out);
    }
    esp_lcd_panel_io_spi_config_t adjusted = *config;
    adjusted.flags.psram_dma_direct = true;
    adjusted.trans_queue_depth = 2;
    esp_err_t result = __real_esp_lcd_new_panel_io_spi(bus, &adjusted, out);
    if (result == ESP_OK) {
        ESP_LOGI("DISPLAY_IO", "CO5300: PSRAM DMA enabled, queue=2; no internal color bounce buffers");
    }
    return result;
}
