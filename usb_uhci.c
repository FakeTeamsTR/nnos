#include "types.h"
#include "ahci.h"
#include "usb_uhci.h"

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA    0x0CFC
#define PCI_COMMAND        0x04
#define PCI_BAR4           0x20
#define PCI_CLASS_REVISION 0x08
#define PCI_COMMAND_IO     0x0001
#define PCI_COMMAND_MASTER 0x0004

#define UHCI_MAX_CONTROLLERS 4
#define UHCI_PORT_COUNT      2
#define UHCI_FRAME_COUNT     1024
#define UHCI_TD_COUNT        40
#define USB_STORAGE_MAX      USB_DRIVE_COUNT
#define USB_MAX_HUBS         8
#define USB_ROOT_PARENT      0xFFu
#define USB_BOT_CBW_SIGNATURE 0x43425355u
#define USB_BOT_CSW_SIGNATURE 0x53425355u
#define UHCI_COMMAND         0x00
#define UHCI_STATUS          0x02
#define UHCI_INTERRUPT       0x04
#define UHCI_FRAME_NUMBER    0x06
#define UHCI_FRAME_BASE      0x08
#define UHCI_SOF_MODULUS     0x0C
#define UHCI_PORT_BASE       0x10

#define UHCI_CMD_RUN         0x0001
#define UHCI_CMD_RESET       0x0002
#define UHCI_CMD_MAX_PACKET  0x0080

#define UHCI_PORT_CONNECTED  0x0001
#define UHCI_PORT_CONNECT_CHANGE 0x0002
#define UHCI_PORT_ENABLED    0x0004
#define UHCI_PORT_ENABLE_CHANGE 0x0008
#define UHCI_PORT_LOW_SPEED  0x0100
#define UHCI_PORT_RESET      0x0200

#define UHCI_LINK_TERMINATE  0x00000001u
#define UHCI_LINK_QUEUE_HEAD 0x00000002u
#define UHCI_TD_ACTIVE       0x00800000u
#define UHCI_TD_ERROR_COUNT  0x0C000000u
#define UHCI_TD_LOW_SPEED    0x04000000u
#define UHCI_TD_SHORT_PACKET 0x20000000u
#define UHCI_TD_ERRORS       0x007F0000u

#define USB_PID_OUT          0xE1
#define USB_PID_IN           0x69
#define USB_PID_SETUP        0x2D

#define USB_ENUMERATION_FAILED  0
#define USB_ENUMERATION_ASSIGNED 1
#define USB_ENUMERATION_KEYBOARD 2
#define USB_ENUMERATION_STORAGE 3
#define USB_ENUMERATION_HUB 4

typedef struct __attribute__((packed, aligned(16)))
{
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
} UhciTransferDescriptor;

typedef struct __attribute__((aligned(16)))
{
    volatile uint32_t horizontal_link;
    volatile uint32_t element_link;
    uint32_t reserved[2];
} UhciQueueHead;

typedef char uhci_td_size_check[
    sizeof(UhciTransferDescriptor) == 16 ? 1 : -1];
typedef char uhci_qh_size_check[
    sizeof(UhciQueueHead) == 16 ? 1 : -1];

typedef struct
{
    uint16_t io_base;
    uint8_t present;
} UhciController;

typedef struct
{
    uint8_t address;
    uint8_t endpoint;
    uint8_t interface_number;
    uint8_t low_speed;
    uint8_t max_packet;
    uint8_t controller;
    uint8_t toggle;
    uint8_t present;
    uint8_t root_port;
    uint8_t parent_hub;
    uint8_t parent_port;
} UsbKeyboard;

typedef struct
{
    uint8_t address;
    uint8_t interface_number;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint8_t max_packet_in;
    uint8_t max_packet_out;
    uint8_t controller;
    uint8_t in_toggle;
    uint8_t out_toggle;
    uint8_t present;
    uint8_t root_port;
    uint8_t parent_hub;
    uint8_t parent_port;
    uint32_t tag;
    uint64_t sector_count;
} UsbStorage;

typedef struct
{
    uint8_t address;
    uint8_t controller;
    uint8_t low_speed;
    uint8_t endpoint_size;
    uint8_t port_count;
    uint8_t root_port;
    uint8_t parent_hub;
    uint8_t parent_port;
    uint8_t present;
} UsbHub;

static UhciController controllers[UHCI_MAX_CONTROLLERS];
static UsbKeyboard keyboard;
static UsbStorage storage_devices[USB_STORAGE_MAX];
static UsbHub hubs[USB_MAX_HUBS];
static uint8_t storage_count;
static uint8_t addresses_in_use[UHCI_MAX_CONTROLLERS][128];
static uint8_t root_addresses[UHCI_MAX_CONTROLLERS][UHCI_PORT_COUNT];
static uint8_t hub_addresses[USB_MAX_HUBS][9];
static uint32_t frame_lists[UHCI_MAX_CONTROLLERS][UHCI_FRAME_COUNT]
    __attribute__((aligned(4096)));
static UhciQueueHead queue_heads[UHCI_MAX_CONTROLLERS]
    __attribute__((aligned(16)));
static UhciTransferDescriptor transfer_descriptors
    [UHCI_MAX_CONTROLLERS][UHCI_TD_COUNT]
    __attribute__((aligned(16)));
static uint8_t setup_packets[UHCI_MAX_CONTROLLERS][16]
    __attribute__((aligned(16)));
static uint8_t control_buffers[UHCI_MAX_CONTROLLERS][256]
    __attribute__((aligned(4096)));
static uint8_t report_buffers[UHCI_MAX_CONTROLLERS][8]
    __attribute__((aligned(16)));
static uint8_t bulk_buffers[UHCI_MAX_CONTROLLERS][4096]
    __attribute__((aligned(4096)));
static uint8_t bot_cbw[UHCI_MAX_CONTROLLERS][31]
    __attribute__((aligned(16)));
static uint8_t bot_csw[UHCI_MAX_CONTROLLERS][13]
    __attribute__((aligned(16)));
static uint8_t scsi_command[UHCI_MAX_CONTROLLERS][16]
    __attribute__((aligned(16)));
static uint8_t previous_report[8];
static uint8_t initialized;
static uint8_t devices_initialized;
static uint32_t poll_countdown;
static uint8_t root_connected[UHCI_MAX_CONTROLLERS][UHCI_PORT_COUNT];
static uint8_t hub_connected[USB_MAX_HUBS][9];

static uint8_t pci_found;

static int storage_identify(UsbStorage* storage);
static void scan_hubs(void);
static void poll_hotplug(void);

static uint8_t allocate_address(uint8_t controller)
{
    uint8_t address;

    for (address = 1; address < 128; address++)
    {
        if (!addresses_in_use[controller][address])
        {
            addresses_in_use[controller][address] = 1;
            return address;
        }
    }
    return 0;
}

static void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"(port)
        : "memory"
    );
}

static uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port)
        : "memory"
    );
}

static void wait_for_next_frame(uint8_t controller)
{
    uint16_t port = controllers[controller].io_base + UHCI_FRAME_NUMBER;
    uint16_t frame = inw(port) & 0x03FFu;
    uint32_t timeout = 1000000;

    while (timeout-- && (inw(port) & 0x03FFu) == frame)
        ;
}

static void delay(uint32_t count)
{
    while (count--)
        __asm__ volatile ("pause");
}

static uint32_t pci_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset)
{
    uint32_t address = 0x80000000u |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFCu);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint16_t value)
{
    uint32_t address = 0x80000000u |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFCu);

    outl(PCI_CONFIG_ADDRESS, address);
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value),
          "Nd"((uint16_t)(PCI_CONFIG_DATA + (offset & 2u)))
        : "memory"
    );
}

static int add_controller(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t class_revision)
{
    uint32_t bar;
    uint32_t command;
    uint16_t base;
    uint8_t index;

    if (((class_revision >> 24) & 0xFFu) != 0x0Cu ||
        ((class_revision >> 16) & 0xFFu) != 0x03u ||
        ((class_revision >> 8) & 0xFFu) != 0x00u ||
        pci_found >= UHCI_MAX_CONTROLLERS)
        return 0;

    bar = pci_read32(bus, device, function, PCI_BAR4);
    if (!(bar & 1u) || (bar & 0xFFFF0000u) != 0)
        return 0;

    base = (uint16_t)(bar & 0xFFE0u);
    if (base < 0x100u || base > 0xFFBDu)
        return 0;

    command = pci_read32(bus, device, function, PCI_COMMAND);
    pci_write16(
        bus, device, function, PCI_COMMAND,
        (uint16_t)((command & 0xFFFFu) |
                   PCI_COMMAND_IO | PCI_COMMAND_MASTER));

    index = pci_found++;
    controllers[index].io_base = base;
    controllers[index].present = 1;

    outw(base + UHCI_INTERRUPT, 0);
    outw(base + UHCI_COMMAND, 0);
    delay(1000000);
    outw(base + UHCI_COMMAND, UHCI_CMD_RESET);
    {
        uint32_t timeout = 1000000;
        while (timeout-- && (inw(base + UHCI_COMMAND) & UHCI_CMD_RESET))
            ;
        if (inw(base + UHCI_COMMAND) & UHCI_CMD_RESET)
        {
            controllers[index].present = 0;
            return 0;
        }
    }

    outw(base + UHCI_STATUS, 0xFFFFu);
    outw(base + UHCI_FRAME_NUMBER, 0);
    outl(base + UHCI_FRAME_BASE, (uint32_t)frame_lists[index]);
    outw(base + UHCI_SOF_MODULUS, 0x40);
    queue_heads[index].horizontal_link = UHCI_LINK_TERMINATE;
    queue_heads[index].element_link = UHCI_LINK_TERMINATE;

    for (uint16_t frame = 0; frame < UHCI_FRAME_COUNT; frame++)
        frame_lists[index][frame] =
            ((uint32_t)&queue_heads[index]) | UHCI_LINK_QUEUE_HEAD;

    outw(base + UHCI_COMMAND, UHCI_CMD_RUN | UHCI_CMD_MAX_PACKET);
    return 1;
}

static void initialize_controllers(void)
{
    uint16_t bus;
    uint8_t device;
    uint8_t function;

    if (initialized)
        return;
    initialized = 1;

    for (bus = 0; bus < 256 && pci_found < UHCI_MAX_CONTROLLERS; bus++)
    {
        for (device = 0; device < 32 &&
             pci_found < UHCI_MAX_CONTROLLERS; device++)
        {
            uint32_t vendor0 = pci_read32((uint8_t)bus, device, 0, 0);
            uint32_t header;
            uint8_t functions = 1;

            if ((vendor0 & 0xFFFFu) == 0xFFFFu)
                continue;

            header = pci_read32((uint8_t)bus, device, 0, 0x0C);
            if (header & (1u << 23))
                functions = 8;

            for (function = 0; function < functions &&
                 pci_found < UHCI_MAX_CONTROLLERS; function++)
            {
                uint32_t vendor =
                    pci_read32((uint8_t)bus, device, function, 0);
                uint32_t class_revision;

                if ((vendor & 0xFFFFu) == 0xFFFFu)
                    continue;

                class_revision = pci_read32(
                    (uint8_t)bus, device, function, PCI_CLASS_REVISION);
                add_controller(
                    (uint8_t)bus, device, function, class_revision);
            }
        }
    }
}

static int uhci_reset_port(uint8_t controller, uint8_t port)
{
    uint16_t base = controllers[controller].io_base;
    uint16_t address = base + UHCI_PORT_BASE + (uint16_t)port * 2u;
    uint16_t status = inw(address);
    uint32_t timeout;

    if (!(status & UHCI_PORT_CONNECTED))
        return 0;

    outw(address, (uint16_t)(
        (status & ~(UHCI_PORT_CONNECT_CHANGE |
                    UHCI_PORT_ENABLE_CHANGE)) |
        (status & (UHCI_PORT_CONNECT_CHANGE |
                   UHCI_PORT_ENABLE_CHANGE)) |
        UHCI_PORT_RESET));
    delay(5000000);

    status = inw(address);
    outw(address, (uint16_t)(
        (status & ~(UHCI_PORT_RESET |
                    UHCI_PORT_CONNECT_CHANGE |
                    UHCI_PORT_ENABLE_CHANGE)) |
        (status & (UHCI_PORT_CONNECT_CHANGE |
                   UHCI_PORT_ENABLE_CHANGE))));
    delay(100000);

    timeout = 1000000;
    while (timeout--)
    {
        status = inw(address);
        if (status & UHCI_PORT_ENABLED)
            return 1;
        if (!(status & UHCI_PORT_CONNECTED))
            return 0;
    }

    return 0;
}

static void uhci_disable_port(uint8_t controller, uint8_t port)
{
    uint16_t base = controllers[controller].io_base;
    uint16_t address = base + UHCI_PORT_BASE + (uint16_t)port * 2u;
    uint16_t status = inw(address);

    outw(address, (uint16_t)(
        (status & ~(UHCI_PORT_ENABLED |
                    UHCI_PORT_CONNECT_CHANGE |
                    UHCI_PORT_ENABLE_CHANGE)) |
        (status & (UHCI_PORT_CONNECT_CHANGE |
                   UHCI_PORT_ENABLE_CHANGE))));
}

static uint32_t td_token(
    uint8_t pid,
    uint8_t address,
    uint8_t endpoint,
    uint8_t toggle,
    uint16_t length)
{
    uint32_t encoded_length = length == 0 ? 0x7FFu : (uint32_t)length - 1u;

    return (uint32_t)pid |
        ((uint32_t)(address & 0x7Fu) << 8) |
        ((uint32_t)(endpoint & 0x0Fu) << 15) |
        ((uint32_t)(toggle & 1u) << 19) |
        (encoded_length << 21);
}

static int submit_transfer(
    uint8_t controller,
    uint8_t count,
    uint32_t timeout)
{
    UhciQueueHead* qh = &queue_heads[controller];
    UhciTransferDescriptor* tds = transfer_descriptors[controller];
    uint32_t i;

    if (count == 0 || count > UHCI_TD_COUNT)
        return 0;

    __asm__ volatile ("" : : : "memory");
    qh->element_link = (uint32_t)&tds[0];
    for (i = 0; i < count; i++)
    {
        uint32_t status;

        do
        {
            status = tds[i].status;
            if (!(status & UHCI_TD_ACTIVE))
                break;
        } while (timeout--);

        if (status & UHCI_TD_ACTIVE)
        {
            qh->element_link = UHCI_LINK_TERMINATE;
            __asm__ volatile ("" : : : "memory");
            wait_for_next_frame(controller);
            return 0;
        }

        if (status & UHCI_TD_ERRORS)
        {
            qh->element_link = UHCI_LINK_TERMINATE;
            __asm__ volatile ("" : : : "memory");
            wait_for_next_frame(controller);
            return 0;
        }
    }

    qh->element_link = UHCI_LINK_TERMINATE;
    __asm__ volatile ("" : : : "memory");
    return 1;
}

static int control_transfer(
    uint8_t controller,
    uint8_t address,
    uint8_t endpoint_size,
    uint8_t low_speed,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint8_t* buffer,
    uint16_t length)
{
    UhciTransferDescriptor* tds = transfer_descriptors[controller];
    uint8_t* setup = setup_packets[controller];
    uint8_t* data = control_buffers[controller];
    uint8_t data_in = (request_type & 0x80u) != 0;
    uint8_t toggle = 1;
    uint16_t offset = 0;
    uint8_t count = 0;
    uint8_t i;

    if (endpoint_size == 0 || endpoint_size > 64 || length > 256)
        return 0;

    if (data_in)
    {
        for (i = 0; i < length; i++)
            data[i] = 0;
    }

    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (uint8_t)value;
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index;
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)length;
    setup[7] = (uint8_t)(length >> 8);

    tds[count].link = (uint32_t)&tds[count + 1];
    tds[count].status = UHCI_TD_ACTIVE | UHCI_TD_ERROR_COUNT |
        (low_speed ? UHCI_TD_LOW_SPEED : 0);
    tds[count].token = td_token(
        USB_PID_SETUP, address, 0, 0, 8);
    tds[count].buffer = (uint32_t)setup;
    count++;

    while (offset < length)
    {
        uint16_t packet = (uint16_t)(length - offset);
        uint8_t* packet_buffer = data + offset;

        if (packet > endpoint_size)
            packet = endpoint_size;
        if (!data_in)
        {
            for (i = 0; i < packet; i++)
                packet_buffer[i] = buffer[offset + i];
        }

        tds[count].link = (uint32_t)&tds[count + 1];
        tds[count].status = UHCI_TD_ACTIVE | UHCI_TD_ERROR_COUNT |
            UHCI_TD_SHORT_PACKET |
            (low_speed ? UHCI_TD_LOW_SPEED : 0);
        tds[count].token = td_token(
            data_in ? USB_PID_IN : USB_PID_OUT,
            address, 0, toggle, packet);
        tds[count].buffer = (uint32_t)packet_buffer;
        count++;

        offset = (uint16_t)(offset + packet);
        toggle ^= 1;
    }

    tds[count].link = UHCI_LINK_TERMINATE;
    tds[count].status = UHCI_TD_ACTIVE | UHCI_TD_ERROR_COUNT |
        (low_speed ? UHCI_TD_LOW_SPEED : 0);
    tds[count].token = td_token(
        data_in ? USB_PID_OUT : USB_PID_IN,
        address, 0, 1, 0);
    tds[count].buffer = 0;
    count++;

    if (!submit_transfer(controller, count, 20000000))
        return 0;

    if (data_in)
    {
        for (i = 0; i < length; i++)
            buffer[i] = data[i];
    }

    return 1;
}

static int enumerate_device(
    uint8_t controller,
    uint8_t address,
    uint8_t low_speed,
    uint8_t root_port,
    uint8_t parent_hub,
    uint8_t parent_port)
{
    uint8_t device_descriptor[18];
    uint8_t config_header[9];
    uint8_t config_descriptor[256];
    uint16_t total_length;
    uint8_t endpoint_size;
    uint8_t config_value;
    uint8_t interface_number = 0;
    uint8_t interface_class = 0;
    uint8_t interface_subclass = 0;
    uint8_t interface_protocol = 0;
    uint8_t keyboard_endpoint = 0;
    uint8_t keyboard_packet_size = 0;
    uint8_t storage_endpoint_in = 0;
    uint8_t storage_endpoint_out = 0;
    uint8_t storage_packet_in = 0;
    uint8_t storage_packet_out = 0;
    uint8_t hub_interface = 0;
    uint8_t active_interface = 0;
    uint16_t offset;

    if (!control_transfer(
            controller, 0, 8, low_speed,
            0x80, 0x06, 0x0100, 0,
            device_descriptor, 8))
        return USB_ENUMERATION_FAILED;

    if (device_descriptor[0] < 8 || device_descriptor[1] != 1)
        return USB_ENUMERATION_FAILED;

    endpoint_size = device_descriptor[7];
    if (endpoint_size != 8 && endpoint_size != 16 &&
        endpoint_size != 32 && endpoint_size != 64)
        return USB_ENUMERATION_FAILED;

    if (!control_transfer(
            controller, 0, endpoint_size, low_speed,
            0x00, 0x05, address, 0, 0, 0))
        return USB_ENUMERATION_FAILED;

    delay(1000000);

    if (!control_transfer(
            controller, address, endpoint_size, low_speed,
            0x80, 0x06, 0x0100, 0,
            device_descriptor, sizeof(device_descriptor)) ||
        !control_transfer(
            controller, address, endpoint_size, low_speed,
            0x80, 0x06, 0x0200, 0,
            config_header, sizeof(config_header)))
        return USB_ENUMERATION_ASSIGNED;

    if (config_header[0] < sizeof(config_header) ||
        config_header[1] != 2)
        return USB_ENUMERATION_ASSIGNED;

    total_length = (uint16_t)config_header[2] |
        ((uint16_t)config_header[3] << 8);
    if (total_length < sizeof(config_header) ||
        total_length > sizeof(config_descriptor))
        return USB_ENUMERATION_ASSIGNED;

    if (!control_transfer(
            controller, address, endpoint_size, low_speed,
            0x80, 0x06, 0x0200, 0,
            config_descriptor, total_length))
        return USB_ENUMERATION_ASSIGNED;

    if (config_descriptor[0] < sizeof(config_header) ||
        config_descriptor[1] != 2)
        return USB_ENUMERATION_ASSIGNED;

    config_value = config_descriptor[5];
    offset = 0;
    while (offset + 2u <= total_length)
    {
        uint8_t descriptor_length = config_descriptor[offset];
        uint8_t descriptor_type = config_descriptor[offset + 1];

        if (descriptor_length < 2 ||
            offset + descriptor_length > total_length)
            break;

        if (descriptor_type == 4 && descriptor_length >= 9)
        {
            interface_class = config_descriptor[offset + 5];
            interface_subclass = config_descriptor[offset + 6];
            interface_protocol = config_descriptor[offset + 7];
            active_interface =
                (!keyboard.present &&
                 interface_class == 0x03 &&
                 interface_subclass == 0x01 &&
                 interface_protocol == 0x01) ||
                (storage_count < USB_STORAGE_MAX &&
                 interface_class == 0x08 &&
                 interface_subclass == 0x06 &&
                 interface_protocol == 0x50) ||
                (interface_class == 0x09);
            if (active_interface)
            {
                interface_number = config_descriptor[offset + 2];
                hub_interface = interface_class == 0x09;
                keyboard_endpoint = 0;
                keyboard_packet_size = 0;
                storage_endpoint_in = 0;
                storage_endpoint_out = 0;
                storage_packet_in = 0;
                storage_packet_out = 0;
            }
        }
        else if (descriptor_type == 5 && descriptor_length >= 7 &&
                 active_interface)
        {
            uint16_t max_packet =
                (uint16_t)config_descriptor[offset + 4] |
                ((uint16_t)config_descriptor[offset + 5] << 8);
            uint8_t endpoint = config_descriptor[offset + 2];
            uint8_t transfer_type = config_descriptor[offset + 3] & 0x03u;

            max_packet &= 0x07FFu;

            if (interface_class == 0x03 &&
                interface_subclass == 0x01 &&
                interface_protocol == 0x01 &&
                (endpoint & 0x80u) &&
                transfer_type == 0x03u &&
                max_packet == 8)
            {
                keyboard_endpoint = endpoint & 0x0Fu;
                keyboard_packet_size = (uint8_t)max_packet;
            }
            else if (interface_class == 0x08 &&
                     interface_subclass == 0x06 &&
                     interface_protocol == 0x50 &&
                     transfer_type == 0x02u &&
                     (max_packet == 8 || max_packet == 16 ||
                      max_packet == 32 || max_packet == 64))
            {
                if (endpoint & 0x80u)
                {
                    storage_endpoint_in = endpoint & 0x0Fu;
                    storage_packet_in = (uint8_t)max_packet;
                }
                else
                {
                    storage_endpoint_out = endpoint & 0x0Fu;
                    storage_packet_out = (uint8_t)max_packet;
                }
            }
        }

        offset = (uint16_t)(offset + descriptor_length);
    }

    if (!control_transfer(
            controller, address, endpoint_size, low_speed,
            0x00, 0x09, config_value, 0, 0, 0))
        return USB_ENUMERATION_ASSIGNED;
    if (config_value == 0)
        return USB_ENUMERATION_ASSIGNED;

    if (hub_interface)
    {
        uint8_t descriptor[9];
        uint8_t hub_index;

        if (!low_speed &&
            control_transfer(
                controller, address, endpoint_size, low_speed,
                0xA0, 0x06, 0x2900, 0, descriptor, sizeof(descriptor)) &&
            descriptor[0] >= 7 &&
            descriptor[1] == 0x29 &&
            descriptor[2] != 0 &&
            descriptor[2] <= 8)
        {
            for (hub_index = 0; hub_index < USB_MAX_HUBS; hub_index++)
            {
                if (!hubs[hub_index].present)
                {
                    uint8_t port;
                    uint16_t power_delay_frames =
                        (uint16_t)descriptor[6] * 2u;

                    hubs[hub_index].address = address;
                    hubs[hub_index].controller = controller;
                    hubs[hub_index].low_speed = low_speed;
                    hubs[hub_index].endpoint_size = endpoint_size;
                    hubs[hub_index].port_count = descriptor[2];
                    hubs[hub_index].root_port = root_port;
                    hubs[hub_index].parent_hub = parent_hub;
                    hubs[hub_index].parent_port = parent_port;
                    hubs[hub_index].present = 1;

                    for (port = 1; port <= descriptor[2]; port++)
                    {
                        control_transfer(
                            controller, address, endpoint_size, low_speed,
                            0x23, 0x03, 8, port, 0, 0);
                    }
                    while (power_delay_frames--)
                        wait_for_next_frame(controller);
                    return USB_ENUMERATION_HUB;
                }
            }
        }
    }

    if (keyboard_endpoint != 0 && keyboard_packet_size != 0 &&
        !keyboard.present &&
        control_transfer(
            controller, address, endpoint_size, low_speed,
            0x21, 0x0B, 0, interface_number, 0, 0))
    {
        keyboard.address = address;
        keyboard.endpoint = keyboard_endpoint;
        keyboard.interface_number = interface_number;
        keyboard.low_speed = low_speed;
        keyboard.max_packet = keyboard_packet_size;
        keyboard.controller = controller;
        keyboard.toggle = 0;
        keyboard.present = 1;
        keyboard.root_port = root_port;
        keyboard.parent_hub = parent_hub;
        keyboard.parent_port = parent_port;
        return USB_ENUMERATION_KEYBOARD;
    }

    if (!low_speed &&
        storage_endpoint_in != 0 &&
        storage_endpoint_out != 0 &&
        storage_packet_in != 0 &&
        storage_packet_out != 0 &&
        storage_count < USB_STORAGE_MAX)
    {
        uint8_t storage_index;

        for (storage_index = 0;
             storage_index < USB_STORAGE_MAX;
             storage_index++)
        {
            if (!storage_devices[storage_index].present)
            {
                UsbStorage* storage = &storage_devices[storage_index];
                storage->address = address;
                storage->interface_number = interface_number;
                storage->endpoint_in = storage_endpoint_in;
                storage->endpoint_out = storage_endpoint_out;
                storage->max_packet_in = storage_packet_in;
                storage->max_packet_out = storage_packet_out;
                storage->controller = controller;
                storage->in_toggle = 0;
                storage->out_toggle = 0;
                storage->tag = 0;
                storage->present = 1;
                storage->root_port = root_port;
                storage->parent_hub = parent_hub;
                storage->parent_port = parent_port;
                storage->sector_count = 0;
                return USB_ENUMERATION_STORAGE;
            }
        }
    }

    return USB_ENUMERATION_ASSIGNED;
}

static void detach_port(
    uint8_t controller,
    uint8_t parent_hub,
    uint8_t port,
    uint8_t root_port)
{
    uint8_t i;

    if (keyboard.present &&
        keyboard.controller == controller &&
        keyboard.parent_hub == parent_hub &&
        keyboard.parent_port == port)
    {
        keyboard.present = 0;
        for (i = 0; i < 8; i++)
            previous_report[i] = 0;
    }

    for (i = 0; i < USB_STORAGE_MAX; i++)
    {
        if (storage_devices[i].present &&
            storage_devices[i].controller == controller &&
            storage_devices[i].parent_hub == parent_hub &&
            storage_devices[i].parent_port == port)
        {
            storage_devices[i].present = 0;
            storage_devices[i].sector_count = 0;
            if (storage_count != 0)
                storage_count--;
        }
    }

    for (i = 0; i < USB_MAX_HUBS; i++)
    {
        uint8_t child_port;
        UsbHub* hub = &hubs[i];

        if (!hub->present ||
            hub->controller != controller ||
            hub->parent_hub != parent_hub ||
            hub->parent_port != port)
            continue;

        for (child_port = 1; child_port <= hub->port_count; child_port++)
        {
            detach_port(controller, i, child_port, root_port);
            hub_connected[i][child_port] = 0;
        }
        if (hub->address < 128)
            addresses_in_use[controller][hub->address] = 0;
        hub->present = 0;
    }

    if (parent_hub == USB_ROOT_PARENT &&
        root_port < UHCI_PORT_COUNT)
    {
        if (root_addresses[controller][root_port] < 128)
            addresses_in_use[controller]
                [root_addresses[controller][root_port]] = 0;
        root_addresses[controller][root_port] = 0;
        root_connected[controller][root_port] = 0;
    }
    else if (parent_hub < USB_MAX_HUBS && port < 9)
    {
        if (hub_addresses[parent_hub][port] < 128)
            addresses_in_use[controller]
                [hub_addresses[parent_hub][port]] = 0;
        hub_addresses[parent_hub][port] = 0;
    }
}

static int hub_read_port_status(
    UsbHub* hub,
    uint8_t port,
    uint8_t* status)
{
    uint8_t response[4] = { 0 };

    if (!control_transfer(
            hub->controller,
            hub->address,
            hub->endpoint_size,
            hub->low_speed,
            0xA3, 0x00, 0, port, response, sizeof(response)))
        return 0;

    status[0] = response[0];
    status[1] = response[1];
    status[2] = response[2];
    status[3] = response[3];
    return 1;
}

static int enumerate_hub_port(uint8_t hub_index, uint8_t port)
{
    UsbHub* hub = &hubs[hub_index];
    uint8_t status[4];
    uint8_t address;
    uint8_t low_speed;
    uint8_t result;
    uint32_t timeout;

    if (!control_transfer(
            hub->controller, hub->address, hub->endpoint_size,
            hub->low_speed, 0x23, 0x03, 4, port, 0, 0))
        return 0;

    delay(5000000);
    if (!hub_read_port_status(hub, port, status) ||
        !(status[0] & 0x01u))
        return 0;

    low_speed = (status[1] & 0x02u) != 0;
    if (status[1] & 0x04u)
        return 0;

    control_transfer(
        hub->controller, hub->address, hub->endpoint_size,
        hub->low_speed, 0x23, 0x01, 20, port, 0, 0);
    control_transfer(
        hub->controller, hub->address, hub->endpoint_size,
        hub->low_speed, 0x23, 0x01, 16, port, 0, 0);

    timeout = 1000000;
    while (timeout--)
    {
        if (!hub_read_port_status(hub, port, status))
            return 0;
        if (!(status[0] & 0x10u))
            break;
    }
    if (status[0] & 0x10u)
        return 0;

    address = allocate_address(hub->controller);
    if (address == 0)
        return 0;
    result = (uint8_t)enumerate_device(
        hub->controller,
        address,
        low_speed,
        hub->root_port,
        hub_index,
        port);
    if (result == USB_ENUMERATION_FAILED)
    {
        addresses_in_use[hub->controller][address] = 0;
        return 0;
    }

    hub_addresses[hub_index][port] = address;
    return 1;
}

static void scan_hubs(void)
{
    uint8_t hub_index;

    for (hub_index = 0; hub_index < USB_MAX_HUBS; hub_index++)
    {
        UsbHub* hub = &hubs[hub_index];
        uint8_t port;

        if (!hub->present)
            continue;

        for (port = 1; port <= hub->port_count; port++)
        {
            uint8_t status[4];
            uint8_t connected;

            if (!hub_read_port_status(hub, port, status))
                continue;

            connected = (status[0] & 0x01u) != 0;
            if (status[2] & 0x01u)
            {
                control_transfer(
                    hub->controller, hub->address,
                    hub->endpoint_size, hub->low_speed,
                    0x23, 0x01, 16, port, 0, 0);
                if (hub_connected[hub_index][port])
                {
                    detach_port(
                        hub->controller, hub_index, port,
                        hub->root_port);
                    hub_connected[hub_index][port] = 0;
                }
            }
            if (status[2] & 0x02u)
            {
                control_transfer(
                    hub->controller, hub->address,
                    hub->endpoint_size, hub->low_speed,
                    0x23, 0x01, 17, port, 0, 0);
            }

            if (hub_connected[hub_index][port] && !connected)
                detach_port(
                    hub->controller, hub_index, port, hub->root_port);
            else if (!hub_connected[hub_index][port] && connected)
                enumerate_hub_port(hub_index, port);

            hub_connected[hub_index][port] = connected;
        }
    }
}

static void enumerate_root_port(uint8_t controller, uint8_t port)
{
    uint16_t address = controllers[controller].io_base +
        UHCI_PORT_BASE + (uint16_t)port * 2u;
    uint16_t status = inw(address);
    uint8_t low_speed;
    uint8_t result;
    uint8_t usb_address;

    if (!(status & UHCI_PORT_CONNECTED) ||
        !uhci_reset_port(controller, port))
        return;

    status = inw(address);
    low_speed = (status & UHCI_PORT_LOW_SPEED) != 0;
    usb_address = allocate_address(controller);
    if (usb_address == 0)
        return;

    result = (uint8_t)enumerate_device(
        controller,
        usb_address,
        low_speed,
        port,
        USB_ROOT_PARENT,
        port);
    if (result != USB_ENUMERATION_FAILED)
        root_addresses[controller][port] = usb_address;
    else
        addresses_in_use[controller][usb_address] = 0;
}

static void initialize_devices(void)
{
    uint8_t controller;
    uint8_t storage_index;

    initialize_controllers();

    for (controller = 0; controller < pci_found;
         controller++)
    {
        uint8_t port;

        if (!controllers[controller].present)
            continue;
        for (port = 0; port < UHCI_PORT_COUNT; port++)
        {
            uint16_t port_status =
                inw(controllers[controller].io_base +
                    UHCI_PORT_BASE + (uint16_t)port * 2u);

            root_connected[controller][port] =
                (port_status & UHCI_PORT_CONNECTED) != 0;
            if (root_connected[controller][port])
                enumerate_root_port(controller, port);
        }
    }

    scan_hubs();

    for (storage_index = 0; storage_index < USB_STORAGE_MAX; storage_index++)
    {
        if (storage_devices[storage_index].present &&
            storage_identify(&storage_devices[storage_index]))
        {
            storage_count++;
        }
        else if (storage_devices[storage_index].present)
            storage_devices[storage_index].present = 0;
    }
    devices_initialized = 1;
}

static void poll_hotplug(void)
{
    uint8_t controller;
    uint8_t storage_index;

    if (!devices_initialized)
    {
        initialize_devices();
        return;
    }

    if (poll_countdown != 0)
    {
        poll_countdown--;
        return;
    }
    poll_countdown = 512;

    for (controller = 0; controller < pci_found; controller++)
    {
        uint8_t port;

        if (!controllers[controller].present)
            continue;

        for (port = 0; port < UHCI_PORT_COUNT; port++)
        {
            uint16_t port_register = controllers[controller].io_base +
                UHCI_PORT_BASE + (uint16_t)port * 2u;
            uint16_t status = inw(port_register);
            uint8_t connected =
                (status & UHCI_PORT_CONNECTED) != 0;

            if (status & (UHCI_PORT_CONNECT_CHANGE |
                          UHCI_PORT_ENABLE_CHANGE))
            {
                uint16_t clear_bits =
                    (status & (UHCI_PORT_ENABLED | UHCI_PORT_RESET)) |
                    (status & (UHCI_PORT_CONNECT_CHANGE |
                               UHCI_PORT_ENABLE_CHANGE));
                outw(port_register, clear_bits);
            }

            if (root_connected[controller][port] &&
                (!connected || (status & UHCI_PORT_CONNECT_CHANGE)))
                detach_port(
                    controller, USB_ROOT_PARENT, port, port);
            if (!root_connected[controller][port] && connected)
                enumerate_root_port(controller, port);

            root_connected[controller][port] = connected;
        }
    }

    scan_hubs();

    for (storage_index = 0; storage_index < USB_STORAGE_MAX;
         storage_index++)
    {
        UsbStorage* storage = &storage_devices[storage_index];
        if (storage->present && storage->sector_count == 0)
        {
            if (storage_identify(storage))
                storage_count++;
            else
                storage->present = 0;
        }
    }
}

static int interrupt_in(uint8_t controller, uint8_t* report)
{
    UhciTransferDescriptor* td = transfer_descriptors[controller];
    uint8_t i;

    for (i = 0; i < keyboard.max_packet; i++)
        report_buffers[controller][i] = 0;

    td[0].link = UHCI_LINK_TERMINATE;
    td[0].status = UHCI_TD_ACTIVE | UHCI_TD_ERROR_COUNT |
        UHCI_TD_SHORT_PACKET |
        (keyboard.low_speed ? UHCI_TD_LOW_SPEED : 0);
    td[0].token = td_token(
        USB_PID_IN, keyboard.address, keyboard.endpoint, keyboard.toggle,
        keyboard.max_packet);
    td[0].buffer = (uint32_t)report_buffers[controller];

    if (!submit_transfer(controller, 1, 2000000))
        return 0;

    keyboard.toggle ^= 1;
    for (i = 0; i < keyboard.max_packet && i < 8; i++)
        report[i] = report_buffers[controller][i];
    return 1;
}

static void store_le32(uint8_t* destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static uint32_t load_le32(const uint8_t* source)
{
    return (uint32_t)source[0] |
        ((uint32_t)source[1] << 8) |
        ((uint32_t)source[2] << 16) |
        ((uint32_t)source[3] << 24);
}

static uint32_t load_be32(const uint8_t* source)
{
    return ((uint32_t)source[0] << 24) |
        ((uint32_t)source[1] << 16) |
        ((uint32_t)source[2] << 8) |
        source[3];
}

static void store_be32(uint8_t* destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static int bulk_transfer(
    UsbStorage* storage,
    uint8_t input,
    uint8_t* buffer,
    uint16_t length)
{
    uint8_t controller = storage->controller;
    uint8_t* bounce = bulk_buffers[controller];
    uint16_t offset = 0;

    if (buffer == 0 && length != 0)
        return 0;

    while (offset < length)
    {
        UhciTransferDescriptor* td =
            &transfer_descriptors[controller][0];
        uint8_t packet_size = input ?
            storage->max_packet_in : storage->max_packet_out;
        uint16_t packet = (uint16_t)(length - offset);
        uint8_t toggle = input ? storage->in_toggle : storage->out_toggle;
        uint8_t endpoint = input ?
            storage->endpoint_in : storage->endpoint_out;
        uint8_t i;

        if (packet > packet_size)
            packet = packet_size;
        if (!input)
        {
            for (i = 0; i < packet; i++)
                bounce[i] = buffer[offset + i];
        }
        else
        {
            for (i = 0; i < packet; i++)
                bounce[i] = 0;
        }

        td->link = UHCI_LINK_TERMINATE;
        td->status = UHCI_TD_ACTIVE | UHCI_TD_ERROR_COUNT |
            (input ? UHCI_TD_SHORT_PACKET : 0);
        td->token = td_token(
            input ? USB_PID_IN : USB_PID_OUT,
            storage->address, endpoint, toggle, packet);
        td->buffer = (uint32_t)bounce;

        if (!submit_transfer(controller, 1, 2000000))
            return 0;

        if (input)
        {
            for (i = 0; i < packet; i++)
                buffer[offset + i] = bounce[i];
            storage->in_toggle ^= 1;
        }
        else
        {
            storage->out_toggle ^= 1;
        }

        offset = (uint16_t)(offset + packet);
    }

    return 1;
}

static int bot_command(
    UsbStorage* storage,
    const uint8_t* cdb,
    uint8_t cdb_length,
    uint8_t input,
    uint8_t* data,
    uint32_t data_length)
{
    uint8_t controller = storage->controller;
    uint8_t* cbw = bot_cbw[controller];
    uint8_t* csw = bot_csw[controller];
    uint32_t tag = ++storage->tag;
    uint8_t i;

    if (cdb == 0 || cdb_length == 0 || cdb_length > 16 ||
        data_length > 4096 || (data_length != 0 && data == 0))
        return 0;

    for (i = 0; i < 31; i++)
        cbw[i] = 0;
    store_le32(cbw, USB_BOT_CBW_SIGNATURE);
    store_le32(cbw + 4, tag);
    store_le32(cbw + 8, data_length);
    cbw[12] = input ? 0x80 : 0;
    cbw[13] = 0;
    cbw[14] = cdb_length;
    for (i = 0; i < cdb_length; i++)
        cbw[15 + i] = cdb[i];

    if (!bulk_transfer(storage, 0, cbw, 31))
        return 0;
    if (data_length != 0 &&
        !bulk_transfer(
            storage, input, data, (uint16_t)data_length))
        return 0;
    if (!bulk_transfer(storage, 1, csw, 13))
        return 0;

    if (load_le32(csw) != USB_BOT_CSW_SIGNATURE ||
        load_le32(csw + 4) != tag ||
        csw[12] != 0 ||
        load_le32(csw + 8) > data_length)
        return 0;

    return 1;
}

static int storage_identify(UsbStorage* storage)
{
    uint8_t* cdb = scsi_command[storage->controller];
    uint8_t inquiry[36];
    uint8_t capacity[8];
    uint8_t i;
    uint32_t last_lba;
    uint32_t block_size;

    for (i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x12;
    cdb[4] = sizeof(inquiry);
    if (!bot_command(
            storage, cdb, 6, 1, inquiry, sizeof(inquiry)))
        return 0;

    for (i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x25;
    if (!bot_command(storage, cdb, 10, 1, capacity, sizeof(capacity)))
        return 0;

    last_lba = load_be32(capacity);
    block_size = load_be32(capacity + 4);
    if (block_size != 512 || last_lba == 0xFFFFFFFFu)
        return 0;

    storage->sector_count = (uint64_t)last_lba + 1u;
    return storage->sector_count != 0;
}

static int storage_transfer(
    UsbStorage* storage,
    uint64_t lba,
    uint8_t* buffer,
    uint8_t write)
{
    uint8_t* cdb = scsi_command[storage->controller];
    uint8_t i;

    if (!storage->present || storage->sector_count == 0 ||
        buffer == 0 || lba >= storage->sector_count ||
        lba > 0xFFFFFFFFu)
        return 0;

    for (i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = write ? 0x2A : 0x28;
    store_be32(cdb + 2, (uint32_t)lba);
    cdb[8] = 1;

    return bot_command(
        storage, cdb, 10, !write, buffer, 512);
}

uint8_t usb_uhci_storage_count(void)
{
    if (!initialized)
        initialize_devices();
    return storage_count;
}

int usb_uhci_storage_sector_count(uint8_t drive, uint64_t* sectors)
{
    uint8_t index;

    if (sectors == 0 || drive < USB_DRIVE_BASE ||
        drive >= USB_DRIVE_BASE + USB_STORAGE_MAX)
        return 0;

    if (!initialized)
        initialize_devices();

    index = (uint8_t)(drive - USB_DRIVE_BASE);
    if (!storage_devices[index].present ||
        storage_devices[index].sector_count == 0)
        return 0;

    *sectors = storage_devices[index].sector_count;
    return 1;
}

int usb_uhci_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer)
{
    uint8_t index;

    if (drive < USB_DRIVE_BASE ||
        drive >= USB_DRIVE_BASE + USB_STORAGE_MAX ||
        buffer == 0)
        return 0;
    if (!initialized)
        initialize_devices();
    index = (uint8_t)(drive - USB_DRIVE_BASE);
    return storage_transfer(&storage_devices[index], lba, buffer, 0);
}

int usb_uhci_storage_write(
    uint8_t drive,
    uint64_t lba,
    const uint8_t* buffer)
{
    uint8_t index;

    if (drive < USB_DRIVE_BASE ||
        drive >= USB_DRIVE_BASE + USB_STORAGE_MAX ||
        buffer == 0)
        return 0;
    if (!initialized)
        initialize_devices();
    index = (uint8_t)(drive - USB_DRIVE_BASE);
    return storage_transfer(
        &storage_devices[index], lba, (uint8_t*)buffer, 1);
}

static char usage_to_key(uint8_t usage, uint8_t shift)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static const char digits[] = "1234567890";

    if (usage >= 0x04 && usage <= 0x1D)
    {
        char result = letters[usage - 0x04];
        return shift ? (char)(result - 'a' + 'A') : result;
    }
    if (usage >= 0x1E && usage <= 0x27)
    {
        static const char shifted_digits[] = "!@#$%^&*()";
        return shift ? shifted_digits[usage - 0x1E] :
                       digits[usage - 0x1E];
    }

    switch (usage)
    {
        case 0x28: return '\n';
        case 0x2A: return '\b';
        case 0x2B: return '\t';
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x4F: return 0x04;
        case 0x50: return 0x03;
        case 0x51: return 0x02;
        case 0x52: return 0x01;
        default: return 0;
    }
}

int usb_uhci_keyboard_poll(char* key)
{
    uint8_t report[8] = { 0 };
    uint8_t i;
    uint8_t shift;
    uint8_t changed = 0;
    char translated;

    if (key == 0)
        return 0;

    poll_hotplug();

    if (!keyboard.present ||
        !controllers[keyboard.controller].present ||
        !interrupt_in(keyboard.controller, report))
        return 0;

    shift = (report[0] & 0x22u) != 0;
    translated = 0;
    for (i = 2; i < 8 && translated == 0; i++)
    {
        uint8_t usage = report[i];
        uint8_t j;
        uint8_t was_pressed = 0;

        if (usage == 0)
            continue;
        for (j = 2; j < 8; j++)
        {
            if (previous_report[j] == usage)
            {
                was_pressed = 1;
                break;
            }
        }

        if (!was_pressed)
        {
            translated = usage_to_key(usage, shift);
            changed = translated != 0;
        }
    }

    for (i = 0; i < 8; i++)
        previous_report[i] = report[i];

    if (!changed)
        return 0;

    *key = translated;
    return 1;
}
