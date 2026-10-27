#ifndef USB_PCI_H
#define USB_PCI_H

#include "types.h"

#define USB_PCI_CONFIG_ADDRESS 0x0CF8
#define USB_PCI_CONFIG_DATA    0x0CFC
#define USB_PCI_COMMAND        0x04
#define USB_PCI_COMMAND_MEMORY 0x0002
#define USB_PCI_COMMAND_MASTER 0x0004

uint32_t usb_pci_read32(
    uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void usb_pci_write16(
    uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint16_t value);
void usb_pci_write32(
    uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint32_t value);
int usb_pci_ehci_handoff(
    uint8_t bus, uint8_t device, uint8_t function, uint32_t hccparams);
uint32_t usb_pci_enable_mmio(
    uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_offset);

int usb_ohci_start(uint8_t bus, uint8_t device, uint8_t function);
void usb_ohci_poll(void);
int usb_ohci_keyboard_poll(char* key);
int usb_ehci_start(uint8_t bus, uint8_t device, uint8_t function);
void usb_ehci_poll(void);
int usb_ehci_keyboard_poll(char* key);
int usb_xhci_start(uint8_t bus, uint8_t device, uint8_t function);
void usb_xhci_poll(void);
int usb_xhci_keyboard_poll(char* key);
int usb_ohci_storage_sector_count(uint8_t drive, uint64_t* sectors);
int usb_ohci_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer);
int usb_ohci_storage_write(
    uint8_t drive, uint64_t lba, const uint8_t* buffer);
int usb_ehci_storage_sector_count(uint8_t drive, uint64_t* sectors);
int usb_ehci_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer);
int usb_ehci_storage_write(
    uint8_t drive, uint64_t lba, const uint8_t* buffer);
int usb_xhci_storage_sector_count(uint8_t drive, uint64_t* sectors);
int usb_xhci_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer);
int usb_xhci_storage_write(
    uint8_t drive, uint64_t lba, const uint8_t* buffer);

#endif
