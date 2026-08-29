#include "usb_msc.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "storage.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

static const char *TAG = "usb_msc";

static bool s_active;
static volatile bool s_host_mounted;

static void on_mount_changed(tinyusb_msc_event_t *event)
{
    s_host_mounted = event->mount_changed_data.is_mounted;
    ESP_LOGI(TAG, "host %s the volume", s_host_mounted ? "mounted" : "released");
}

esp_err_t usb_msc_start(void)
{
    if (s_active) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(storage_mounted(), ESP_ERR_NOT_FOUND, TAG, "no card");

    /* Give the card up before TinyUSB takes it: two FAT drivers writing the
     * same volume is how filesystems die. */
    sdmmc_card_t *card = NULL;
    ESP_RETURN_ON_ERROR(storage_release_for_usb(&card), TAG, "release card");

    const tinyusb_msc_sdmmc_config_t msc_cfg = {
        .card = card,
        .callback_mount_changed = on_mount_changed,
        .mount_config = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 16 * 1024,
        },
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_sdmmc(&msc_cfg), TAG, "msc init");

    const tinyusb_config_t usb_cfg = {
        .device_descriptor = NULL,     /* defaults from Kconfig */
        .string_descriptor = NULL,
        .external_phy = false,
        .configuration_descriptor = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&usb_cfg), TAG, "tinyusb install");

    s_active = true;
    ESP_LOGI(TAG, "microSD exposed over USB (console is gone until reboot)");
    return ESP_OK;
}

bool usb_msc_active(void) { return s_active; }
bool usb_msc_host_mounted(void) { return s_host_mounted; }

void usb_msc_stop(void)
{
    ESP_LOGI(TAG, "leaving transfer mode, rebooting");

    /*
     * Handing the PHY back is not optional, and a reboot on its own does not
     * do it. Installing TinyUSB pointed the one internal FSLS PHY at the OTG
     * core by writing sw_usb_phy_sel in RTC_CNTL_USB_CONF - a register in the
     * RTC domain, which a software reset leaves untouched. Restart without
     * undoing it and the chip comes back with the Serial/JTAG console mapped
     * to an external PHY this board does not have, while the OTG core sits
     * uninitialised: D+/D- are driven by nobody and the host sees no device at
     * all, neither disk nor console, until someone pulls the battery.
     */
    if (tinyusb_driver_uninstall() != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB teardown failed, giving the PHY back anyway");
    }
    usb_serial_jtag_ll_phy_enable_external(false);

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
