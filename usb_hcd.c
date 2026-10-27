#include "types.h"
#include "ahci.h"
#include "usb_hcd.h"
#include "usb_pci.h"

static uint8_t initialized;
#define USB_HCD_PENDING_KEYS 12
static char pending_keys[USB_HCD_PENDING_KEYS];
static uint8_t pending_head;
static uint8_t pending_tail;
static uint8_t pending_count;

static void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

uint32_t usb_pci_read32(
    uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t address = 0x80000000u |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFCu);
    outl(USB_PCI_CONFIG_ADDRESS, address);
    return inl(USB_PCI_CONFIG_DATA);
}

void usb_pci_write16(
    uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint16_t value)
{
    uint32_t address = 0x80000000u |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFCu);
    outl(USB_PCI_CONFIG_ADDRESS, address);
    __asm__ volatile (
        "outw %0, %1" : : "a"(value),
        "Nd"((uint16_t)(USB_PCI_CONFIG_DATA + (offset & 2u))) : "memory");
}

void usb_pci_write32(
    uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint32_t value)
{
    uint32_t address = 0x80000000u |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFCu);
    outl(USB_PCI_CONFIG_ADDRESS, address);
    outl(USB_PCI_CONFIG_DATA, value);
}

uint32_t usb_pci_enable_mmio(
    uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_offset)
{
    uint32_t bar = usb_pci_read32(bus, device, function, bar_offset);
    uint32_t command;

    if ((bar & 1u) || (bar & 0x6u) == 0x2u ||
        (bar & 0x6u) == 0x6u)
        return 0;
    if ((bar & 0x6u) == 0x4u)
    {
        uint32_t upper = usb_pci_read32(
            bus, device, function, (uint8_t)(bar_offset + 4));
        if (upper != 0)
            return 0;
    }

    bar &= 0xFFFFFFF0u;
    if (bar == 0)
        return 0;

    command = usb_pci_read32(bus, device, function, USB_PCI_COMMAND);
    usb_pci_write16(
        bus, device, function, USB_PCI_COMMAND,
        (uint16_t)((command & 0xFFFFu) |
                   USB_PCI_COMMAND_MEMORY | USB_PCI_COMMAND_MASTER));
    return bar;
}

int usb_pci_ehci_handoff(
    uint8_t bus, uint8_t device, uint8_t function, uint32_t hccparams)
{
    uint8_t offset = (uint8_t)((hccparams >> 8) & 0xFFu);
    uint8_t examined = 0;

    while (offset >= 0x40u && offset <= 0xF8u && examined++ < 48)
    {
        uint32_t header = usb_pci_read32(bus, device, function, offset);
        uint8_t id = (uint8_t)header;
        uint8_t next = (uint8_t)(header >> 8);

        if (id == 1)
        {
            uint32_t timeout = 1000000;
            uint16_t ownership = (uint16_t)(header >> 16);
            usb_pci_write16(
                bus, device, function, (uint8_t)(offset + 2),
                (uint16_t)(ownership | 0x0100u));
            while (timeout-- &&
                   (usb_pci_read32(
                       bus, device, function, offset) & 0x00010000u))
                ;
            if (usb_pci_read32(
                    bus, device, function, offset) & 0x00010000u)
                return 0;
            usb_pci_write16(
                bus, device, function, (uint8_t)(offset + 4), 0);
            return 1;
        }
        if (next == 0)
            break;
        offset = next;
    }
    return 1;
}

static void discover_controllers(void)
{
    uint16_t bus;
    uint8_t device;
    uint8_t function;

    if (initialized)
        return;
    initialized = 1;

    for (bus = 0; bus < 256; bus++)
    {
        for (device = 0; device < 32; device++)
        {
            uint32_t id = usb_pci_read32((uint8_t)bus, device, 0, 0);
            uint32_t header;
            uint8_t functions = 1;

            if ((id & 0xFFFFu) == 0xFFFFu)
                continue;
            header = usb_pci_read32((uint8_t)bus, device, 0, 0x0C);
            if (header & (1u << 23))
                functions = 8;

            for (function = 0; function < functions; function++)
            {
                uint32_t function_id =
                    usb_pci_read32((uint8_t)bus, device, function, 0);
                uint32_t class_revision;
                uint8_t class_code;
                uint8_t subclass;
                uint8_t programming_interface;

                if ((function_id & 0xFFFFu) == 0xFFFFu)
                    continue;
                class_revision = usb_pci_read32(
                    (uint8_t)bus, device, function, 0x08);
                class_code = (uint8_t)(class_revision >> 24);
                subclass = (uint8_t)(class_revision >> 16);
                programming_interface = (uint8_t)(class_revision >> 8);
                if (class_code != 0x0C || subclass != 0x03)
                    continue;

                if (programming_interface == 0x10)
                    usb_ohci_start((uint8_t)bus, device, function);
                else if (programming_interface == 0x20)
                    usb_ehci_start((uint8_t)bus, device, function);
                else if (programming_interface == 0x30)
                    usb_xhci_start((uint8_t)bus, device, function);
            }
        }
    }
}

void usb_hcd_poll(void)
{
    static uint16_t poll_countdown;

    discover_controllers();
    if (poll_countdown != 0)
    {
        poll_countdown--;
        return;
    }
    poll_countdown = 256;
    usb_ohci_poll();
    usb_ehci_poll();
    usb_xhci_poll();
}

int usb_hcd_keyboard_poll(char* key)
{
    char next_key;

    if (key == 0)
        return 0;
    if (pending_count < USB_HCD_PENDING_KEYS &&
        usb_ohci_keyboard_poll(&next_key))
    {
        pending_keys[pending_tail] = next_key;
        pending_tail = (uint8_t)((pending_tail + 1u) %
            USB_HCD_PENDING_KEYS);
        pending_count++;
    }
    if (pending_count < USB_HCD_PENDING_KEYS &&
        usb_ehci_keyboard_poll(&next_key))
    {
        pending_keys[pending_tail] = next_key;
        pending_tail = (uint8_t)((pending_tail + 1u) %
            USB_HCD_PENDING_KEYS);
        pending_count++;
    }
    if (pending_count < USB_HCD_PENDING_KEYS &&
        usb_xhci_keyboard_poll(&next_key))
    {
        pending_keys[pending_tail] = next_key;
        pending_tail = (uint8_t)((pending_tail + 1u) %
            USB_HCD_PENDING_KEYS);
        pending_count++;
    }
    if (pending_count == 0)
        return 0;
    *key = pending_keys[pending_head];
    pending_head = (uint8_t)((pending_head + 1u) %
        USB_HCD_PENDING_KEYS);
    pending_count--;
    return 1;
}

int usb_hcd_storage_sector_count(uint8_t drive, uint64_t* sectors)
{
    if (drive >= XHCI_DRIVE_BASE)
        return usb_xhci_storage_sector_count(drive, sectors);
    if (drive >= EHCI_DRIVE_BASE)
        return usb_ehci_storage_sector_count(drive, sectors);
    if (drive >= OHCI_DRIVE_BASE)
        return usb_ohci_storage_sector_count(drive, sectors);
    return 0;
}

int usb_hcd_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer)
{
    if (drive >= XHCI_DRIVE_BASE)
        return usb_xhci_storage_read(drive, lba, buffer);
    if (drive >= EHCI_DRIVE_BASE)
        return usb_ehci_storage_read(drive, lba, buffer);
    if (drive >= OHCI_DRIVE_BASE)
        return usb_ohci_storage_read(drive, lba, buffer);
    return 0;
}

int usb_hcd_storage_write(
    uint8_t drive, uint64_t lba, const uint8_t* buffer)
{
    if (drive >= XHCI_DRIVE_BASE)
        return usb_xhci_storage_write(drive, lba, buffer);
    if (drive >= EHCI_DRIVE_BASE)
        return usb_ehci_storage_write(drive, lba, buffer);
    if (drive >= OHCI_DRIVE_BASE)
        return usb_ohci_storage_write(drive, lba, buffer);
    return 0;
}
