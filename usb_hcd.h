#ifndef USB_HCD_H
#define USB_HCD_H

#include "types.h"

void usb_hcd_poll(void);
int usb_hcd_keyboard_poll(char* key);
int usb_hcd_storage_sector_count(uint8_t drive, uint64_t* sectors);
int usb_hcd_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer);
int usb_hcd_storage_write(
    uint8_t drive, uint64_t lba, const uint8_t* buffer);

#endif
