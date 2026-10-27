#include "types.h"
#include "ahci.h"
#include "usb_pci.h"

#define OHCI_MAX_CONTROLLERS 4
#define OHCI_MAX_PORTS       15
#define OHCI_CONTROL         0x04
#define OHCI_COMMAND_STATUS  0x08
#define OHCI_INTERRUPT_DISABLE 0x14
#define OHCI_HCCA            0x18
#define OHCI_CONTROL_HEAD_ED 0x20
#define OHCI_FRAME_INTERVAL  0x34
#define OHCI_PERIODIC_START  0x40
#define OHCI_COMMAND_LIST_FILLED 0x00000002u
#define OHCI_RH_DESCRIPTOR_A 0x48
#define OHCI_RH_STATUS       0x50
#define OHCI_RH_PORT_STATUS  0x54
#define OHCI_CONTROL_RESET   0x00000001u
#define OHCI_CONTROL_RUN     0x00000080u
#define OHCI_PORT_CONNECTED  0x00000001u
#define OHCI_PORT_ENABLE     0x00000002u
#define OHCI_PORT_RESET      0x00000010u
#define OHCI_PORT_POWER      0x00000100u
#define OHCI_PORT_LOW_SPEED   0x00000200u
#define OHCI_PORT_CONNECT_CHANGE 0x00010000u
#define OHCI_PORT_ENABLE_CHANGE  0x00020000u
#define OHCI_PORT_RESET_CHANGE   0x00100000u
#define OHCI_CONTROL_PERIODIC 0x00000004u
#define OHCI_CONTROL_CONTROL  0x00000010u
#define OHCI_TD_CC_NOT_ACCESSED 0xF0000000u
#define OHCI_TD_DI_NONE       (7u << 21)
#define OHCI_TD_CERR          (3u << 26)
#define OHCI_TD_PID_SETUP     (0u << 19)
#define OHCI_TD_PID_OUT       (1u << 19)
#define OHCI_TD_PID_IN        (2u << 19)
#define OHCI_TD_T_DATA0       (2u << 24)
#define OHCI_TD_T_DATA1       (3u << 24)
#define OHCI_TD_TOGGLE_CARRY  (0u << 24)
#define OHCI_TD_COUNT 32
#define OHCI_PACKET_BUFFER_SIZE 4096
#define OHCI_BOT_CBW_SIGNATURE 0x43425355u
#define OHCI_BOT_CSW_SIGNATURE 0x53425355u

typedef struct __attribute__((packed, aligned(16)))
{
    volatile uint32_t control;
    volatile uint32_t tail_pointer;
    volatile uint32_t head_pointer;
    volatile uint32_t next;
} OhciEndpointDescriptor;

typedef struct __attribute__((packed, aligned(16)))
{
    volatile uint32_t control;
    volatile uint32_t current_buffer;
    volatile uint32_t next;
    volatile uint32_t buffer_end;
} OhciTransferDescriptor;

typedef struct
{
    uint32_t interrupt_table[32];
    uint16_t frame_number;
    uint16_t pad;
    uint32_t done_head;
    uint8_t reserved[120];
} OhciHcca;

typedef struct
{
    uint32_t base;
    uint8_t ports;
    uint16_t power_delay_ms;
    uint8_t present;
    uint8_t connected[OHCI_MAX_PORTS];
    uint8_t keyboard_port;
    uint8_t keyboard_address;
    uint8_t keyboard_endpoint;
    uint8_t keyboard_packet_size;
    uint8_t keyboard_toggle;
    uint8_t keyboard_present;
    uint8_t previous_report[8];
} OhciController;

typedef struct
{
    uint8_t present;
    uint8_t address;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint8_t packet_in;
    uint8_t packet_out;
    uint8_t toggle_in;
    uint8_t toggle_out;
    uint32_t tag;
    uint64_t sectors;
} OhciStorage;

static OhciController controllers[OHCI_MAX_CONTROLLERS];
static OhciStorage storage_devices[OHCI_MAX_CONTROLLERS][OHCI_MAX_PORTS];
static OhciHcca hcca[OHCI_MAX_CONTROLLERS] __attribute__((aligned(256)));
static OhciEndpointDescriptor control_ed[OHCI_MAX_CONTROLLERS]
    __attribute__((aligned(16)));
static OhciEndpointDescriptor bulk_ed[OHCI_MAX_CONTROLLERS]
    __attribute__((aligned(16)));
static OhciEndpointDescriptor interrupt_ed[OHCI_MAX_CONTROLLERS]
    __attribute__((aligned(16)));
static OhciTransferDescriptor transfer_descriptors[OHCI_MAX_CONTROLLERS]
    [OHCI_TD_COUNT] __attribute__((aligned(16)));
static OhciTransferDescriptor interrupt_descriptors[OHCI_MAX_CONTROLLERS]
    [2] __attribute__((aligned(16)));
static uint8_t setup_buffer[OHCI_MAX_CONTROLLERS][8]
    __attribute__((aligned(16)));
static uint8_t control_buffer[OHCI_MAX_CONTROLLERS][OHCI_PACKET_BUFFER_SIZE]
    __attribute__((aligned(4096)));
static uint8_t interrupt_buffer[OHCI_MAX_CONTROLLERS][8]
    __attribute__((aligned(16)));
static uint8_t bulk_buffer[OHCI_MAX_CONTROLLERS][4096]
    __attribute__((aligned(4096)));
static uint8_t bot_cbw[OHCI_MAX_CONTROLLERS][31]
    __attribute__((aligned(16)));
static uint8_t bot_csw[OHCI_MAX_CONTROLLERS][13]
    __attribute__((aligned(16)));
static uint8_t bot_cdb[OHCI_MAX_CONTROLLERS][16]
    __attribute__((aligned(16)));
static uint8_t controller_count;

static int ohci_control_transfer(
    uint8_t controller_index,
    uint8_t address,
    uint8_t endpoint_size,
    uint8_t low_speed,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint8_t* buffer,
    uint16_t length);
static int enumerate_keyboard(uint8_t controller_index, uint8_t port);
static char ohci_usage_to_key(uint8_t usage, uint8_t shift);

static uint32_t mmio_read(OhciController* controller, uint32_t offset)
{
    return *(volatile uint32_t*)(controller->base + offset);
}

static void mmio_write(
    OhciController* controller, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t*)(controller->base + offset) = value;
}

static void wait_frames(OhciController* controller, uint8_t frames)
{
    uint16_t previous = (uint16_t)mmio_read(controller, 0x3C);
    uint16_t elapsed = 0;
    uint32_t timeout = 20000000;

    while (elapsed < frames && timeout--)
    {
        uint16_t current = (uint16_t)mmio_read(controller, 0x3C);
        if (current != previous)
        {
            elapsed = (uint16_t)(elapsed + (uint16_t)(current - previous));
            previous = current;
        }
    }
}

static int ohci_submit(
    uint8_t controller_index,
    OhciEndpointDescriptor* ed,
    uint8_t td_count)
{
    OhciController* controller = &controllers[controller_index];
    uint32_t control;
    uint32_t timeout = 20000000;
    uint8_t complete = 0;
    uint16_t i;

    if (td_count == 0 || td_count >= OHCI_TD_COUNT)
        return 0;
    ed->next = 0;
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller, OHCI_CONTROL_HEAD_ED, (uint32_t)ed);
    control = mmio_read(controller, OHCI_CONTROL);
    mmio_write(controller, OHCI_CONTROL,
        (control & ~0xC0u) | 0x80u | OHCI_CONTROL_CONTROL);
    mmio_write(controller, OHCI_COMMAND_STATUS, OHCI_COMMAND_LIST_FILLED);

    while (timeout--)
    {
        complete = 1;
        __asm__ volatile ("" : : : "memory");
        for (i = 0; i < td_count; i++)
        {
            if ((transfer_descriptors[controller_index][i].control &
                 OHCI_TD_CC_NOT_ACCESSED) == OHCI_TD_CC_NOT_ACCESSED)
            {
                complete = 0;
                break;
            }
        }
        if (complete)
            break;
    }

    control = mmio_read(controller, OHCI_CONTROL);
    mmio_write(controller, OHCI_CONTROL, control & ~OHCI_CONTROL_CONTROL);
    mmio_write(controller, OHCI_CONTROL_HEAD_ED, 0);
    if (!complete)
        return 0;
    for (i = 0; i < td_count; i++)
    {
        if ((transfer_descriptors[controller_index][i].control >> 28) != 0)
            return 0;
    }
    return 1;
}

static int enumerate_keyboard(uint8_t controller_index, uint8_t port)
{
    OhciController* controller = &controllers[controller_index];
    uint8_t device_descriptor[18];
    uint8_t config_header[9];
    uint8_t config_descriptor[256];
    uint8_t address = (uint8_t)(port + 1u);
    uint8_t endpoint_size;
    uint8_t config_value;
    uint8_t interface_number = 0;
    uint8_t endpoint = 0;
    uint8_t packet_size = 0;
    uint8_t low_speed;
    uint16_t total_length;
    uint16_t offset;

    if (address >= 128)
        return 0;
    low_speed = (mmio_read(controller,
        OHCI_RH_PORT_STATUS + (uint32_t)port * 4u) &
        OHCI_PORT_LOW_SPEED) != 0;
    if (!ohci_control_transfer(controller_index, 0, 8, low_speed,
            0x80, 0x06, 0x0100, 0, device_descriptor, 8) ||
        device_descriptor[0] < 8 || device_descriptor[1] != 1)
        return 0;
    endpoint_size = device_descriptor[7];
    if (endpoint_size != 8 && endpoint_size != 16 &&
        endpoint_size != 32 && endpoint_size != 64)
        return 0;

    if (!ohci_control_transfer(controller_index, 0, 8, low_speed,
            0x00, 0x05, address, 0, 0, 0))
        return 0;
    wait_frames(controller, 2);
    if (!ohci_control_transfer(controller_index, address, endpoint_size, low_speed,
            0x80, 0x06, 0x0100, 0, device_descriptor,
            sizeof(device_descriptor)) ||
        !ohci_control_transfer(controller_index, address, endpoint_size, low_speed,
            0x80, 0x06, 0x0200, 0, config_header,
            sizeof(config_header)))
        return 0;
    if (config_header[0] < sizeof(config_header) ||
        config_header[1] != 2)
        return 0;
    total_length = (uint16_t)config_header[2] |
        ((uint16_t)config_header[3] << 8);
    if (total_length < sizeof(config_header) ||
        total_length > sizeof(config_descriptor))
        return 0;
    if (!ohci_control_transfer(controller_index, address, endpoint_size, low_speed,
            0x80, 0x06, 0x0200, 0, config_descriptor, total_length))
        return 0;

    for (offset = 0; offset + 2u <= total_length; )
    {
        uint8_t descriptor_length = config_descriptor[offset];
        uint8_t descriptor_type = config_descriptor[offset + 1];

        if (descriptor_length < 2 ||
            offset + descriptor_length > total_length)
            break;
        if (descriptor_type == 4 && descriptor_length >= 9)
        {
            uint8_t cls = config_descriptor[offset + 5];
            uint8_t sub = config_descriptor[offset + 6];
            uint8_t proto = config_descriptor[offset + 7];
            if (cls == 3 && sub == 1 && proto == 1)
            {
                interface_number = config_descriptor[offset + 2];
                endpoint = 0;
                packet_size = 0;
            }
            else
            {
                interface_number = 0xFF;
            }
        }
        else if (descriptor_type == 5 && descriptor_length >= 7 &&
                 interface_number != 0xFF)
        {
            uint16_t max_packet = (uint16_t)config_descriptor[offset + 4] |
                ((uint16_t)config_descriptor[offset + 5] << 8);
            if ((config_descriptor[offset + 2] & 0x80u) &&
                (config_descriptor[offset + 3] & 3u) == 3u &&
                max_packet >= 8 && max_packet <= 8)
            {
                endpoint = config_descriptor[offset + 2] & 0x0Fu;
                packet_size = (uint8_t)max_packet;
            }
        }
        offset = (uint16_t)(offset + descriptor_length);
    }

    config_value = config_descriptor[5];
    if (endpoint == 0 || packet_size == 0 || config_value == 0 ||
        !ohci_control_transfer(controller_index, address, endpoint_size, low_speed,
            0x00, 0x09, config_value, 0, 0, 0) ||
        !ohci_control_transfer(controller_index, address, endpoint_size, low_speed,
            0x21, 0x0B, 0, interface_number, 0, 0))
        return 0;

    controller->keyboard_port = (uint8_t)(port + 1u);
    controller->keyboard_address = address;
    controller->keyboard_endpoint = endpoint;
    controller->keyboard_packet_size = packet_size;
    controller->keyboard_toggle = 0;
    controller->keyboard_present = 1;

    interrupt_ed[controller_index].control = address |
        ((uint32_t)endpoint << 7) | (2u << 11) |
        ((uint32_t)packet_size << 16);
    interrupt_ed[controller_index].tail_pointer =
        (uint32_t)&interrupt_descriptors[controller_index][1];
    interrupt_ed[controller_index].head_pointer =
        (uint32_t)&interrupt_descriptors[controller_index][0];
    interrupt_ed[controller_index].next = 0;
    for (uint8_t i = 0; i < 32; i++)
        hcca[controller_index].interrupt_table[i] =
            (uint32_t)&interrupt_ed[controller_index];
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller, OHCI_CONTROL,
        (mmio_read(controller, OHCI_CONTROL) | OHCI_CONTROL_PERIODIC) &
        ~0xC0u | 0x80u);
    return 1;
}

static char ohci_usage_to_key(uint8_t usage, uint8_t shift)
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
        static const char shifted[] = "!@#$%^&*()";
        return shift ? shifted[usage - 0x1E] : digits[usage - 0x1E];
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

static int poll_keyboard(uint8_t controller_index, uint8_t* report);

int usb_ohci_keyboard_poll(char* key)
{
    uint8_t report[8] = { 0 };
    uint8_t controller_index;

    if (key == 0)
        return 0;
    for (controller_index = 0; controller_index < controller_count;
         controller_index++)
    {
        OhciController* controller = &controllers[controller_index];
        uint8_t shift;
        uint8_t i;

        if (!controller->keyboard_present ||
            !poll_keyboard(controller_index, report))
            continue;
        shift = (report[0] & 0x22u) != 0;
        for (i = 2; i < 8; i++)
        {
            uint8_t was_pressed = 0;
            uint8_t j;
            char translated;

            if (report[i] == 0)
                continue;
            for (j = 2; j < 8; j++)
                if (controller->previous_report[j] == report[i])
                    was_pressed = 1;
            if (was_pressed)
                continue;
            translated = ohci_usage_to_key(report[i], shift);
            if (translated != 0)
            {
                for (j = 0; j < 8; j++)
                    controller->previous_report[j] = report[j];
                *key = translated;
                return 1;
            }
        }
        for (i = 0; i < 8; i++)
            controller->previous_report[i] = report[i];
    }
    return 0;
}

static int poll_keyboard(uint8_t controller_index, uint8_t* report)
{
    OhciController* controller = &controllers[controller_index];
    OhciEndpointDescriptor* ed = &interrupt_ed[controller_index];
    OhciTransferDescriptor* td =
        &interrupt_descriptors[controller_index][0];
    uint32_t timeout = 2000000;
    uint32_t head;
    uint8_t complete = 0;

    if (!controller->keyboard_present)
        return 0;
    td->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_DI_NONE |
        OHCI_TD_PID_IN | OHCI_TD_TOGGLE_CARRY;
    td->current_buffer = (uint32_t)interrupt_buffer[controller_index];
    td->next = (uint32_t)&interrupt_descriptors[controller_index][1];
    td->buffer_end = (uint32_t)(
        interrupt_buffer[controller_index] +
        controller->keyboard_packet_size - 1);
    interrupt_descriptors[controller_index][1].control =
        OHCI_TD_CC_NOT_ACCESSED;
    ed->tail_pointer = (uint32_t)&interrupt_descriptors[controller_index][1];
    head = (uint32_t)&interrupt_descriptors[controller_index][0];
    if (controller->keyboard_toggle)
        head |= 2u;
    ed->head_pointer = head;
    mmio_write(controller, OHCI_CONTROL,
        (mmio_read(controller, OHCI_CONTROL) | OHCI_CONTROL_PERIODIC) &
        ~0xC0u | 0x80u);

    while (timeout--)
    {
        __asm__ volatile ("" : : : "memory");
        if ((td->control & OHCI_TD_CC_NOT_ACCESSED) !=
            OHCI_TD_CC_NOT_ACCESSED)
        {
            complete = 1;
            break;
        }
    }
    if (!complete || (td->control >> 28) != 0)
        return 0;

    controller->keyboard_toggle ^= 1;
    for (uint8_t i = 0; i < controller->keyboard_packet_size && i < 8; i++)
        report[i] = interrupt_buffer[controller_index][i];
    return 1;
}

static uint8_t build_td(
    uint8_t controller_index,
    uint8_t td_index,
    uint32_t pid,
    uint32_t toggle,
    uint8_t* buffer,
    uint16_t length,
    uint8_t last)
{
    OhciTransferDescriptor* td =
        &transfer_descriptors[controller_index][td_index];

    td->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_DI_NONE |
        pid | toggle;
    td->current_buffer = length == 0 ? 0 : (uint32_t)buffer;
    td->next = (uint32_t)&transfer_descriptors[controller_index][td_index + 1];
    td->buffer_end = length == 0 ? 0 : (uint32_t)(buffer + length - 1);
    if (last)
        td->next = (uint32_t)&transfer_descriptors[controller_index][td_index + 1];
    return (uint8_t)(td_index + 1);
}

static int ohci_control_transfer(
    uint8_t controller_index,
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
    OhciEndpointDescriptor* ed = &control_ed[controller_index];
    uint8_t* setup = setup_buffer[controller_index];
    uint8_t* staging = control_buffer[controller_index];
    uint8_t td_count = 0;
    uint16_t offset = 0;
    uint8_t toggle = 1;
    uint16_t i;

    if (endpoint_size == 0 || endpoint_size > 64 ||
        length > OHCI_PACKET_BUFFER_SIZE || (length != 0 && buffer == 0))
        return 0;

    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (uint8_t)value;
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index;
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)length;
    setup[7] = (uint8_t)(length >> 8);
    for (i = 0; i < length; i++)
        staging[i] = (request_type & 0x80u) ? 0 : buffer[i];

    build_td(controller_index, td_count++, OHCI_TD_PID_SETUP,
             OHCI_TD_T_DATA0, setup, 8, 0);
    while (offset < length)
    {
        uint16_t packet = (uint16_t)(length - offset);
        if (packet > endpoint_size)
            packet = endpoint_size;
        build_td(controller_index, td_count++,
            (request_type & 0x80u) ? OHCI_TD_PID_IN : OHCI_TD_PID_OUT,
            toggle ? OHCI_TD_T_DATA1 : OHCI_TD_T_DATA0,
            staging + offset, packet, 0);
        toggle ^= 1;
        offset = (uint16_t)(offset + packet);
        if (td_count >= OHCI_TD_COUNT - 1)
            return 0;
    }

    build_td(controller_index, td_count++,
        (request_type & 0x80u) ? OHCI_TD_PID_OUT : OHCI_TD_PID_IN,
        OHCI_TD_T_DATA1, 0, 0, 1);
    transfer_descriptors[controller_index][td_count].control =
        OHCI_TD_CC_NOT_ACCESSED;
    transfer_descriptors[controller_index][td_count].current_buffer = 0;
    transfer_descriptors[controller_index][td_count].next = 0;
    transfer_descriptors[controller_index][td_count].buffer_end = 0;

    ed->control = address | (low_speed ? (1u << 13) : 0) |
        ((uint32_t)endpoint_size << 16);
    ed->tail_pointer =
        (uint32_t)&transfer_descriptors[controller_index][td_count];
    ed->head_pointer = (uint32_t)&transfer_descriptors[controller_index][0];
    if (!ohci_submit(controller_index, ed, td_count))
        return 0;

    if (request_type & 0x80u)
        for (i = 0; i < length; i++)
            buffer[i] = staging[i];
    return 1;
}

static uint32_t ohci_load_le32(const uint8_t* value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
        ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static void ohci_store_le32(uint8_t* value, uint32_t data)
{
    value[0] = (uint8_t)data;
    value[1] = (uint8_t)(data >> 8);
    value[2] = (uint8_t)(data >> 16);
    value[3] = (uint8_t)(data >> 24);
}

static uint32_t ohci_load_be32(const uint8_t* value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
        ((uint32_t)value[2] << 8) | value[3];
}

static void ohci_store_be32(uint8_t* value, uint32_t data)
{
    value[0] = (uint8_t)(data >> 24);
    value[1] = (uint8_t)(data >> 16);
    value[2] = (uint8_t)(data >> 8);
    value[3] = (uint8_t)data;
}

static int ohci_bulk_transfer(
    uint8_t controller_index,
    OhciStorage* storage,
    uint8_t input,
    uint8_t* buffer,
    uint16_t length)
{
    OhciController* controller = &controllers[controller_index];
    OhciEndpointDescriptor* ed = &bulk_ed[controller_index];
    uint8_t packet_size = input ? storage->packet_in : storage->packet_out;
    uint8_t endpoint = input ? storage->endpoint_in : storage->endpoint_out;
    uint8_t td_count = 0;
    uint16_t offset = 0;
    uint32_t control;
    uint32_t timeout = 20000000;
    uint8_t complete = 0;

    if (!storage->present || packet_size == 0 ||
        packet_size > 64 || (length != 0 && buffer == 0))
        return 0;

    while (offset < length)
    {
        uint16_t packet = (uint16_t)(length - offset);
        OhciTransferDescriptor* td;
        if (packet > packet_size)
            packet = packet_size;
        if (td_count >= OHCI_TD_COUNT - 1)
            return 0;
        td = &transfer_descriptors[controller_index][td_count];
        td->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_DI_NONE |
            OHCI_TD_CERR |
            (input ? OHCI_TD_PID_IN : OHCI_TD_PID_OUT) |
            OHCI_TD_TOGGLE_CARRY;
        td->current_buffer = (uint32_t)(buffer + offset);
        td->next = (uint32_t)&transfer_descriptors[controller_index]
            [td_count + 1u];
        td->buffer_end = (uint32_t)(buffer + offset + packet - 1u);
        td_count++;
        offset = (uint16_t)(offset + packet);
    }

    transfer_descriptors[controller_index][td_count].control =
        OHCI_TD_CC_NOT_ACCESSED;
    transfer_descriptors[controller_index][td_count].current_buffer = 0;
    transfer_descriptors[controller_index][td_count].buffer_end = 0;
    transfer_descriptors[controller_index][td_count].next = 0;
    ed->control = storage->address | ((uint32_t)endpoint << 7) |
        ((uint32_t)packet_size << 16);
    ed->tail_pointer = (uint32_t)&transfer_descriptors[controller_index]
        [td_count];
    ed->head_pointer = (uint32_t)&transfer_descriptors[controller_index][0] |
        ((input ? storage->toggle_in : storage->toggle_out) ? 2u : 0u);
    ed->next = 0;
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller, 0x28, (uint32_t)ed);
    control = mmio_read(controller, OHCI_CONTROL);
    mmio_write(controller, OHCI_CONTROL,
        (control & ~0xC0u) | 0x80u | (1u << 5));
    mmio_write(controller, OHCI_COMMAND_STATUS, 0x00000004u);

    while (timeout--)
    {
        complete = 1;
        for (uint8_t i = 0; i < td_count; i++)
        {
            if ((transfer_descriptors[controller_index][i].control &
                 OHCI_TD_CC_NOT_ACCESSED) == OHCI_TD_CC_NOT_ACCESSED)
            {
                complete = 0;
                break;
            }
        }
        if (complete)
            break;
    }
    control = mmio_read(controller, OHCI_CONTROL);
    mmio_write(controller, OHCI_CONTROL, control & ~(1u << 5));
    mmio_write(controller, 0x28, 0);
    if (!complete)
        return 0;
    for (uint8_t i = 0; i < td_count; i++)
        if ((transfer_descriptors[controller_index][i].control >> 28) != 0)
            return 0;
    if (input)
        storage->toggle_in = (uint8_t)((ed->head_pointer >> 1) & 1u);
    else
        storage->toggle_out = (uint8_t)((ed->head_pointer >> 1) & 1u);
    return 1;
}

static int ohci_bot_command(
    uint8_t controller_index,
    OhciStorage* storage,
    const uint8_t* cdb,
    uint8_t cdb_length,
    uint8_t input,
    uint8_t* data,
    uint32_t data_length)
{
    uint8_t* cbw = bot_cbw[controller_index];
    uint8_t* csw = bot_csw[controller_index];
    uint32_t tag;
    uint32_t offset;
    uint32_t transfer_limit;

    if (cdb_length == 0 || cdb_length > 16 ||
        data_length > sizeof(bulk_buffer[controller_index]))
        return 0;
    for (uint8_t i = 0; i < 31; i++)
        cbw[i] = 0;
    tag = ++storage->tag;
    ohci_store_le32(cbw, OHCI_BOT_CBW_SIGNATURE);
    ohci_store_le32(cbw + 4, tag);
    ohci_store_le32(cbw + 8, data_length);
    cbw[12] = input ? 0x80 : 0;
    cbw[14] = cdb_length;
    for (uint8_t i = 0; i < cdb_length; i++)
        cbw[15 + i] = cdb[i];
    if (!ohci_bulk_transfer(controller_index, storage, 0, cbw, 31))
        return 0;
    transfer_limit = (uint32_t)(OHCI_TD_COUNT - 1u) *
        (input ? storage->packet_in : storage->packet_out);
    for (offset = 0; offset < data_length; )
    {
        uint16_t chunk = (uint16_t)(data_length - offset);
        if (chunk > transfer_limit)
            chunk = (uint16_t)transfer_limit;
        if (!ohci_bulk_transfer(controller_index, storage, input,
                                data + offset, chunk))
            return 0;
        offset += chunk;
    }
    for (uint8_t i = 0; i < 13; i++)
        csw[i] = 0;
    if (!ohci_bulk_transfer(controller_index, storage, 1, csw, 13))
        return 0;
    return ohci_load_le32(csw) == OHCI_BOT_CSW_SIGNATURE &&
        ohci_load_le32(csw + 4) == tag &&
        ohci_load_le32(csw + 8) == 0 && csw[12] == 0;
}

static int ohci_storage_identify(
    uint8_t controller_index, OhciStorage* storage)
{
    uint8_t inquiry[36];
    uint8_t capacity[8];
    uint8_t* cdb = bot_cdb[controller_index];
    uint32_t last_lba;

    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x12;
    cdb[4] = sizeof(inquiry);
    if (!ohci_bot_command(controller_index, storage, cdb, 6, 1,
                          inquiry, sizeof(inquiry)))
        return 0;
    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x25;
    if (!ohci_bot_command(controller_index, storage, cdb, 10, 1,
                          capacity, sizeof(capacity)))
        return 0;
    last_lba = ohci_load_be32(capacity);
    if (ohci_load_be32(capacity + 4) != 512u ||
        last_lba == 0xFFFFFFFFu)
        return 0;
    storage->sectors = (uint64_t)last_lba + 1u;
    return storage->sectors != 0;
}

static int ohci_enumerate_storage(uint8_t controller_index, uint8_t port)
{
    OhciController* controller = &controllers[controller_index];
    OhciStorage* storage = &storage_devices[controller_index][port];
    uint8_t descriptor[18];
    uint8_t config_header[9];
    uint8_t config[256];
    uint8_t address = (uint8_t)(port + 1u);
    uint8_t low_speed =
        (mmio_read(controller, OHCI_RH_PORT_STATUS + (uint32_t)port * 4u) &
         OHCI_PORT_LOW_SPEED) != 0;
    uint8_t endpoint_size;
    uint8_t endpoint_in = 0;
    uint8_t endpoint_out = 0;
    uint8_t packet_in = 0;
    uint8_t packet_out = 0;
    uint8_t interface_number = 0xFF;
    uint16_t total;
    uint16_t offset;

    if (low_speed)
        return 0;
    if (ohci_control_transfer(controller_index, 0, 8, 0,
            0x80, 0x06, 0x0100, 0, descriptor, 8) &&
        descriptor[0] >= 8 && descriptor[1] == 1)
    {
        if (!ohci_control_transfer(controller_index, 0, 8, 0,
                0, 0x05, address, 0, 0, 0))
            return 0;
        wait_frames(controller, 2);
    }
    else if (!ohci_control_transfer(controller_index, address, 8, 0,
                 0x80, 0x06, 0x0100, 0, descriptor, 8) ||
             descriptor[0] < 8 || descriptor[1] != 1)
    {
        return 0;
    }
    endpoint_size = descriptor[7];
    if (endpoint_size != 8 && endpoint_size != 16 &&
        endpoint_size != 32 && endpoint_size != 64)
        return 0;
    if (!ohci_control_transfer(controller_index, address, endpoint_size, 0,
            0x80, 0x06, 0x0200, 0, config_header, sizeof(config_header)))
        return 0;
    total = (uint16_t)config_header[2] |
        ((uint16_t)config_header[3] << 8);
    if (config_header[0] < 9 || config_header[1] != 2 ||
        total < 9 || total > sizeof(config) ||
        !ohci_control_transfer(controller_index, address, endpoint_size, 0,
            0x80, 0x06, 0x0200, 0, config, total))
        return 0;
    for (offset = 0; offset + 2u <= total; )
    {
        uint8_t size = config[offset];
        uint8_t type = config[offset + 1u];
        if (size < 2 || offset + size > total)
            return 0;
        if (type == 4 && size >= 9)
        {
            interface_number = config[offset + 5] == 8 &&
                config[offset + 6] == 6 && config[offset + 7] == 0x50 ?
                config[offset + 2] : 0xFF;
            endpoint_in = endpoint_out = packet_in = packet_out = 0;
        }
        else if (type == 5 && size >= 7 && interface_number != 0xFF &&
                 (config[offset + 3] & 3u) == 2u)
        {
            uint16_t max_packet = (uint16_t)config[offset + 4] |
                ((uint16_t)config[offset + 5] << 8);
            if (max_packet != 8 && max_packet != 16 &&
                max_packet != 32 && max_packet != 64)
                return 0;
            if (config[offset + 2] & 0x80u)
            {
                endpoint_in = config[offset + 2] & 0x0Fu;
                packet_in = (uint8_t)max_packet;
            }
            else
            {
                endpoint_out = config[offset + 2] & 0x0Fu;
                packet_out = (uint8_t)max_packet;
            }
        }
        offset = (uint16_t)(offset + size);
    }
    if (interface_number == 0xFF || endpoint_in == 0 || endpoint_out == 0 ||
        packet_in == 0 || packet_out == 0 || config[5] == 0)
        return 0;
    if (!ohci_control_transfer(controller_index, address, endpoint_size, 0,
            0, 0x09, config[5], 0, 0, 0))
        return 0;

    storage->present = 1;
    storage->address = address;
    storage->endpoint_in = endpoint_in;
    storage->endpoint_out = endpoint_out;
    storage->packet_in = packet_in;
    storage->packet_out = packet_out;
    storage->toggle_in = storage->toggle_out = 0;
    storage->tag = 0;
    storage->sectors = 0;
    if (!ohci_storage_identify(controller_index, storage))
    {
        storage->present = 0;
        return 0;
    }
    return 1;
}

static int reset_port(OhciController* controller, uint8_t port)
{
    uint32_t offset = OHCI_RH_PORT_STATUS + (uint32_t)port * 4u;
    uint32_t status = mmio_read(controller, offset);
    uint32_t timeout;

    if (!(status & OHCI_PORT_CONNECTED))
        return 0;
    mmio_write(controller, offset, OHCI_PORT_POWER);
    {
        uint32_t previous = mmio_read(controller, 0x3C) & 0xFFFFu;
        uint32_t elapsed = 0;
        uint32_t timeout = 50000000;
        while (elapsed < controller->power_delay_ms && timeout--)
        {
            uint32_t current = mmio_read(controller, 0x3C) & 0xFFFFu;
            if (current != previous)
            {
                elapsed += (current - previous) & 0xFFFFu;
                previous = current;
            }
        }
    }
    mmio_write(controller, offset, OHCI_PORT_RESET);
    timeout = 1000000;
    while (timeout-- && (mmio_read(controller, offset) & OHCI_PORT_RESET))
        ;
    status = mmio_read(controller, offset);
    return (status & (OHCI_PORT_CONNECTED | OHCI_PORT_ENABLE)) ==
        (OHCI_PORT_CONNECTED | OHCI_PORT_ENABLE);
}

int usb_ohci_start(uint8_t bus, uint8_t device, uint8_t function)
{
    OhciController* controller;
    uint32_t base;
    uint32_t descriptor;
    uint32_t timeout;

    if (controller_count >= OHCI_MAX_CONTROLLERS)
        return 0;
    base = usb_pci_enable_mmio(bus, device, function, 0x10);
    if (base == 0)
        return 0;

    controller = &controllers[controller_count];
    controller->base = base;
    controller->present = 0;
    mmio_write(controller, OHCI_INTERRUPT_DISABLE, 0xFFFFFFFFu);

    mmio_write(controller, OHCI_COMMAND_STATUS, OHCI_CONTROL_RESET);
    timeout = 1000000;
    while (timeout-- &&
           (mmio_read(controller, OHCI_COMMAND_STATUS) & OHCI_CONTROL_RESET))
        ;
    if (mmio_read(controller, OHCI_COMMAND_STATUS) & OHCI_CONTROL_RESET)
        return 0;

    descriptor = mmio_read(controller, OHCI_RH_DESCRIPTOR_A);
    controller->ports = (uint8_t)(descriptor & 0xFFu);
    controller->power_delay_ms =
        (uint16_t)(((descriptor >> 24) & 0xFFu) * 2u);
    if (controller->ports == 0 || controller->ports > OHCI_MAX_PORTS)
        return 0;

    mmio_write(controller, OHCI_HCCA, (uint32_t)&hcca[controller_count]);
    mmio_write(controller, OHCI_FRAME_INTERVAL, 0x27782EDFu);
    mmio_write(controller, OHCI_PERIODIC_START, 0x2A2Fu);
    mmio_write(controller, OHCI_RH_STATUS, 0x00010000u);
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller, OHCI_CONTROL, OHCI_CONTROL_RUN);
    wait_frames(controller, 1);

    controller->present = 1;
    controller_count++;
    for (uint8_t port = 0; port < controller->ports; port++)
    {
        uint32_t status = mmio_read(
            controller, OHCI_RH_PORT_STATUS + (uint32_t)port * 4u);
        controller->connected[port] =
            (status & OHCI_PORT_CONNECTED) != 0;
        if (controller->connected[port])
        {
            if (reset_port(controller, port))
            {
                wait_frames(controller, 10);
                if (!controller->keyboard_present &&
                    enumerate_keyboard((uint8_t)(controller_count - 1), port))
                    continue;
                ohci_enumerate_storage(
                    (uint8_t)(controller_count - 1), port);
            }
        }
    }
    return 1;
}

void usb_ohci_poll(void)
{
    uint8_t index;

    for (index = 0; index < controller_count; index++)
    {
        OhciController* controller = &controllers[index];
        uint8_t port;

        if (!controller->present)
            continue;
        for (port = 0; port < controller->ports; port++)
        {
            uint32_t offset = OHCI_RH_PORT_STATUS + (uint32_t)port * 4u;
            uint32_t status = mmio_read(controller, offset);
            uint8_t connected = (status & OHCI_PORT_CONNECTED) != 0;

            if (status & OHCI_PORT_CONNECT_CHANGE)
                mmio_write(controller, offset, OHCI_PORT_CONNECT_CHANGE);
            if (status & OHCI_PORT_ENABLE_CHANGE)
                mmio_write(controller, offset, OHCI_PORT_ENABLE_CHANGE);
            if (status & OHCI_PORT_RESET_CHANGE)
                mmio_write(controller, offset, OHCI_PORT_RESET_CHANGE);
            if (!connected && controller->keyboard_present &&
                controller->keyboard_port == (uint8_t)(port + 1u))
            {
                controller->keyboard_present = 0;
                controller->keyboard_toggle = 0;
                interrupt_ed[index].control |= 1u << 14;
                for (uint8_t frame = 0; frame < 32; frame++)
                    hcca[index].interrupt_table[frame] = 0;
                for (uint8_t key = 0; key < 8; key++)
                    controller->previous_report[key] = 0;
            }
            if (!connected)
            {
                storage_devices[index][port].present = 0;
                storage_devices[index][port].sectors = 0;
            }
            if (connected && !controller->connected[port])
            {
                if (reset_port(controller, port))
                {
                    wait_frames(controller, 10);
                    if (!controller->keyboard_present &&
                        enumerate_keyboard(index, port))
                    {
                        controller->connected[port] = connected;
                        continue;
                    }
                    ohci_enumerate_storage(index, port);
                }
                else
                {
                    controller->connected[port] = 0;
                    continue;
                }
            }

            controller->connected[port] = connected;
        }
    }
}

static int ohci_storage_transfer(
    uint8_t drive, uint64_t lba, uint8_t* buffer, uint8_t write)
{
    uint8_t slot;
    uint8_t controller_index;
    uint8_t port;
    uint8_t* cdb;
    OhciStorage* storage;

    if (drive < OHCI_DRIVE_BASE ||
        drive >= OHCI_DRIVE_BASE + OHCI_DRIVE_COUNT || buffer == 0)
        return 0;
    slot = (uint8_t)(drive - OHCI_DRIVE_BASE);
    controller_index = (uint8_t)(slot / OHCI_MAX_PORTS);
    port = (uint8_t)(slot % OHCI_MAX_PORTS);
    if (controller_index >= controller_count)
        return 0;
    storage = &storage_devices[controller_index][port];
    if (!storage->present || storage->sectors == 0 ||
        lba >= storage->sectors || lba > 0xFFFFFFFFu)
        return 0;

    cdb = bot_cdb[controller_index];
    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = write ? 0x2A : 0x28;
    ohci_store_be32(cdb + 2, (uint32_t)lba);
    cdb[8] = 1;
    if (write)
        for (uint16_t i = 0; i < 512; i++)
            bulk_buffer[controller_index][i] = buffer[i];
    if (!ohci_bot_command(controller_index, storage, cdb, 10,
            !write, bulk_buffer[controller_index], 512))
        return 0;
    if (!write)
        for (uint16_t i = 0; i < 512; i++)
            buffer[i] = bulk_buffer[controller_index][i];
    return 1;
}

int usb_ohci_storage_sector_count(uint8_t drive, uint64_t* sectors)
{
    uint8_t slot;
    uint8_t controller_index;
    uint8_t port;
    OhciStorage* storage;

    if (sectors == 0 || drive < OHCI_DRIVE_BASE ||
        drive >= OHCI_DRIVE_BASE + OHCI_DRIVE_COUNT)
        return 0;
    slot = (uint8_t)(drive - OHCI_DRIVE_BASE);
    controller_index = (uint8_t)(slot / OHCI_MAX_PORTS);
    port = (uint8_t)(slot % OHCI_MAX_PORTS);
    if (controller_index >= controller_count)
        return 0;
    storage = &storage_devices[controller_index][port];
    if (!storage->present || storage->sectors == 0)
        return 0;
    *sectors = storage->sectors;
    return 1;
}

int usb_ohci_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer)
{
    return ohci_storage_transfer(drive, lba, buffer, 0);
}

int usb_ohci_storage_write(
    uint8_t drive, uint64_t lba, const uint8_t* buffer)
{
    return ohci_storage_transfer(drive, lba, (uint8_t*)buffer, 1);
}
