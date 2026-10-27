#ifndef USB_UHCI_H
#define USB_UHCI_H

#include "types.h"

int usb_uhci_keyboard_poll(char* key);
uint8_t usb_uhci_storage_count(void);
int usb_uhci_storage_sector_count(uint8_t drive, uint64_t* sectors);
int usb_uhci_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer);
int usb_uhci_storage_write(uint8_t drive, uint64_t lba, const uint8_t* buffer);

#endif
