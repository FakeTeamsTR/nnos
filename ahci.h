#ifndef AHCI_H
#define AHCI_H

#include "types.h"

#define AHCI_DRIVE_BASE 4
#define AHCI_PORT_COUNT 12
#define USB_DRIVE_BASE (AHCI_DRIVE_BASE + AHCI_PORT_COUNT)
#define USB_DRIVE_COUNT 4
#define OHCI_DRIVE_BASE (USB_DRIVE_BASE + USB_DRIVE_COUNT)
#define OHCI_CONTROLLER_COUNT 4
#define OHCI_PORT_COUNT 15
#define OHCI_DRIVE_COUNT (OHCI_CONTROLLER_COUNT * OHCI_PORT_COUNT)
#define EHCI_DRIVE_BASE (OHCI_DRIVE_BASE + OHCI_DRIVE_COUNT)
#define EHCI_CONTROLLER_COUNT 4
#define EHCI_PORT_COUNT 15
#define EHCI_DRIVE_COUNT (EHCI_CONTROLLER_COUNT * EHCI_PORT_COUNT)
#define XHCI_DRIVE_BASE (EHCI_DRIVE_BASE + EHCI_DRIVE_COUNT)
#define XHCI_CONTROLLER_COUNT 2
#define XHCI_STORAGE_PORT_COUNT 15
#define XHCI_DRIVE_COUNT (XHCI_CONTROLLER_COUNT * XHCI_STORAGE_PORT_COUNT)
#define DISK_MAX_DEVICES (XHCI_DRIVE_BASE + XHCI_DRIVE_COUNT)

int ahci_get_sector_count(uint8_t drive, uint64_t* sector_count);
int ahci_read_sector(uint8_t drive, uint64_t lba, uint8_t* buffer);
int ahci_write_sector(uint8_t drive, uint64_t lba, const uint8_t* buffer);

#endif
