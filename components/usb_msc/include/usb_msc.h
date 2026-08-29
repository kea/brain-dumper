#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Transfer mode: hand the microSD to the host as a USB mass storage device so
 * notes and transcripts can be copied off, and config.ini edited, with no
 * special software on the PC.
 *
 * The ESP32-S3 has a single USB PHY shared between the built-in Serial/JTAG
 * console and the OTG controller TinyUSB drives. Starting MSC therefore takes
 * the console away and the device re-enumerates as a disk; there is no way
 * back without a reboot, which is why usb_msc_stop() reboots - after pointing
 * the PHY back at the console, because the mux that selects between the two
 * lives in the RTC domain and outlives a software reset.
 */

esp_err_t usb_msc_start(void);
void      usb_msc_stop(void) __attribute__((noreturn));
bool      usb_msc_active(void);

/* True while the host has the volume mounted; writing to the card from
 * firmware during that window would corrupt the host's view of the FAT. */
bool      usb_msc_host_mounted(void);
