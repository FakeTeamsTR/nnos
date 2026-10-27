#include "types.h"
#include "ahci.h"
#include "usb_pci.h"

#define EHCI_MAX_CONTROLLERS 4
#define EHCI_MAX_PORTS       15
#define EHCI_QTD_COUNT       32
#define EHCI_BUFFER_SIZE     4096
#define EHCI_CAP_HCSPARAMS   0x04
#define EHCI_CAP_HCCPARAMS   0x08
#define EHCI_OP_USBCMD       0x00
#define EHCI_OP_USBSTS       0x04
#define EHCI_OP_PERIODIC     0x14
#define EHCI_OP_ASYNC        0x18
#define EHCI_OP_CONFIG       0x40
#define EHCI_OP_PORTSC       0x44
#define EHCI_CMD_PERIODIC    0x00000004u
#define EHCI_CMD_ADVANCE     0x00000040u
#define EHCI_STS_ASYNC_ADVANCE 0x00000020u
#define EHCI_STS_ASYNC_ACTIVE  0x00008000u
#define EHCI_STS_PERIODIC_ACTIVE 0x00004000u
#define EHCI_CMD_RUN         0x00000001u
#define EHCI_CMD_RESET       0x00000002u
#define EHCI_CMD_ASYNC       0x00000020u
#define EHCI_STS_HALTED      0x00001000u
#define EHCI_PORT_CONNECTED  0x00000001u
#define EHCI_PORT_ENABLE     0x00000004u
#define EHCI_PORT_RESET      0x00000100u
#define EHCI_PORT_POWER      0x00001000u
#define EHCI_PORT_OWNER      0x00002000u
#define EHCI_PORT_CHANGE     0x0000002Au
#define EHCI_PORT_LINE_STATUS 0x00000C00u
#define EHCI_PORT_HIGH_SPEED 0x00000800u
#define EHCI_LINK_TERMINATE  0x00000001u
#define EHCI_LINK_QH         0x00000002u
#define EHCI_QTD_TERMINATE   0x00000001u
#define EHCI_QTD_ACTIVE      0x00000080u
#define EHCI_QTD_HALTED      0x00000040u
#define EHCI_QTD_IOC         0x00008000u
#define EHCI_QTD_PID_OUT     (0u << 8)
#define EHCI_QTD_PID_IN      (1u << 8)
#define EHCI_QTD_PID_SETUP   (2u << 8)
#define EHCI_QTD_CERR        (3u << 10)
#define EHCI_QTD_TOGGLE      0x80000000u
#define EHCI_QH_SPEED_HIGH   (2u << 12)
#define EHCI_QH_HEAD         (1u << 15)
#define EHCI_QH_DTC          (1u << 14)
#define EHCI_BOT_CBW_SIGNATURE 0x43425355u
#define EHCI_BOT_CSW_SIGNATURE 0x53425355u
#define EHCI_EHCI_STORAGE_MAX EHCI_MAX_PORTS

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t next;
    volatile uint32_t alternate;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    uint32_t reserved[3];
} EhciTransferDescriptor;

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t horizontal_link;
    volatile uint32_t endpoint;
    volatile uint32_t capabilities;
    volatile uint32_t current_qtd;
    volatile uint32_t next_qtd;
    volatile uint32_t alternate_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    uint32_t reserved[4];
} EhciQueueHead;

typedef struct
{
    uint32_t base;
    uint32_t operational;
    uint8_t ports;
    uint8_t present;
    uint8_t connected[EHCI_MAX_PORTS];
    uint8_t keyboard_present;
    uint8_t keyboard_port;
    uint8_t keyboard_address;
    uint8_t keyboard_endpoint;
    uint8_t keyboard_packet_size;
    uint8_t keyboard_toggle;
    uint8_t previous_report[8];
} EhciController;

typedef struct
{
    uint8_t present;
    uint8_t address;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint16_t packet_in;
    uint16_t packet_out;
    uint8_t toggle_in;
    uint8_t toggle_out;
    uint32_t tag;
    uint64_t sectors;
} EhciStorage;

static EhciController controllers[EHCI_MAX_CONTROLLERS];
static EhciStorage storage_devices[EHCI_MAX_CONTROLLERS][EHCI_EHCI_STORAGE_MAX];
static uint32_t periodic_lists[EHCI_MAX_CONTROLLERS][1024]
    __attribute__((aligned(4096)));
static EhciQueueHead async_heads[EHCI_MAX_CONTROLLERS]
    __attribute__((aligned(32)));
static EhciQueueHead control_heads[EHCI_MAX_CONTROLLERS]
    __attribute__((aligned(32)));
static EhciQueueHead interrupt_heads[EHCI_MAX_CONTROLLERS]
    __attribute__((aligned(32)));
static EhciTransferDescriptor qtds[EHCI_MAX_CONTROLLERS][EHCI_QTD_COUNT]
    __attribute__((aligned(32)));
static uint8_t setup_buffers[EHCI_MAX_CONTROLLERS][8]
    __attribute__((aligned(16)));
static uint8_t control_buffers[EHCI_MAX_CONTROLLERS][EHCI_BUFFER_SIZE]
    __attribute__((aligned(4096)));
static uint8_t keyboard_reports[EHCI_MAX_CONTROLLERS][8]
    __attribute__((aligned(16)));
static uint8_t bulk_buffers[EHCI_MAX_CONTROLLERS][4096]
    __attribute__((aligned(4096)));
static uint8_t bot_cbw[EHCI_MAX_CONTROLLERS][31]
    __attribute__((aligned(16)));
static uint8_t bot_csw[EHCI_MAX_CONTROLLERS][13]
    __attribute__((aligned(16)));
static uint8_t bot_cdb[EHCI_MAX_CONTROLLERS][16]
    __attribute__((aligned(16)));
static uint8_t controller_count;

static int ehci_control_transfer(
    uint8_t controller_index,
    uint8_t address,
    uint8_t endpoint_size,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint8_t* buffer,
    uint16_t length);
static int ehci_enumerate_keyboard(uint8_t controller_index, uint8_t port);
static char ehci_usage_to_key(uint8_t usage, uint8_t shift);

static uint32_t mmio_read(EhciController* controller, uint32_t offset)
{
    return *(volatile uint32_t*)(controller->operational + offset);
}

static void mmio_write(
    EhciController* controller, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t*)(controller->operational + offset) = value;
}

static void wait_for_reset_interval(EhciController* controller)
{
    uint32_t previous = mmio_read(controller, 0x0C) & 0x3FFFu;
    uint32_t elapsed = 0;
    uint32_t timeout = 50000000;

    while (elapsed < 400u && timeout--)
    {
        uint32_t current = mmio_read(controller, 0x0C) & 0x3FFFu;
        if (current != previous)
        {
            elapsed += (current - previous) & 0x3FFFu;
            previous = current;
        }
    }
}

static void reset_high_speed_port(EhciController* controller, uint8_t port)
{
    uint32_t offset = EHCI_OP_PORTSC + (uint32_t)port * 4u;
    uint32_t status = mmio_read(controller, offset);
    uint32_t timeout;

    if (!(status & EHCI_PORT_CONNECTED))
        return;
    if ((status & EHCI_PORT_LINE_STATUS) == 0x00000400u)
    {
        mmio_write(controller, offset,
            (status & ~(EHCI_PORT_CHANGE | EHCI_PORT_RESET)) |
            (status & EHCI_PORT_CHANGE) | EHCI_PORT_OWNER);
        return;
    }

    mmio_write(controller, offset,
        (status & ~(EHCI_PORT_CHANGE | EHCI_PORT_ENABLE)) |
        EHCI_PORT_POWER | EHCI_PORT_RESET);
    wait_for_reset_interval(controller);
    status = mmio_read(controller, offset);
    mmio_write(controller, offset,
        (status & ~(EHCI_PORT_CHANGE | EHCI_PORT_RESET)) |
        (status & EHCI_PORT_CHANGE) | EHCI_PORT_POWER);
    timeout = 1000000;
    while (timeout-- && !(mmio_read(controller, offset) & EHCI_PORT_ENABLE))
        ;
    if (!(mmio_read(controller, offset) & EHCI_PORT_ENABLE))
    {
        status = mmio_read(controller, offset);
        mmio_write(controller, offset,
            (status & ~(EHCI_PORT_CHANGE | EHCI_PORT_RESET)) |
            (status & EHCI_PORT_CHANGE) | EHCI_PORT_OWNER);
    }
}

static void ehci_prepare_qtd(
    uint8_t controller_index,
    uint8_t td_index,
    uint8_t pid,
    uint8_t toggle,
    uint8_t* buffer,
    uint16_t length,
    uint8_t last)
{
    EhciTransferDescriptor* td = &qtds[controller_index][td_index];
    uint32_t token = EHCI_QTD_ACTIVE | EHCI_QTD_CERR |
        ((uint32_t)pid << 8) | ((uint32_t)length << 16);
    uint32_t address;
    uint8_t page;

    if (toggle)
        token |= EHCI_QTD_TOGGLE;
    if (last)
        token |= EHCI_QTD_IOC;
    td->next = last ? EHCI_QTD_TERMINATE :
        (uint32_t)&qtds[controller_index][td_index + 1];
    td->alternate = EHCI_QTD_TERMINATE;
    td->token = token;
    for (page = 0; page < 5; page++)
        td->buffer[page] = 0;
    if (length != 0)
    {
        address = (uint32_t)buffer;
        td->buffer[0] = address;
        address = (address & 0xFFFFF000u) + 0x1000u;
        for (page = 1; page < 5 && length > (uint16_t)(4096u -
             ((uint32_t)buffer & 0xFFFu) + ((uint32_t)(page - 1) * 4096u));
             page++, address += 0x1000u)
            td->buffer[page] = address;
    }
}

static int ehci_submit_control_chain(
    uint8_t controller_index,
    uint8_t address,
    uint16_t endpoint_size,
    uint8_t td_count,
    uint8_t endpoint,
    uint8_t control_endpoint)
{
    EhciController* controller = &controllers[controller_index];
    EhciQueueHead* qh = &async_heads[controller_index];
    uint32_t command = mmio_read(controller, EHCI_OP_USBCMD);
    uint32_t timeout = 10000000;
    uint8_t i;
    uint8_t stopped = 0;
    uint8_t complete = 0;

    mmio_write(controller, EHCI_OP_USBCMD, command & ~EHCI_CMD_ASYNC);
    while (timeout)
    {
        if (!(mmio_read(controller, EHCI_OP_USBSTS) & EHCI_STS_ASYNC_ACTIVE))
        {
            stopped = 1;
            break;
        }
        timeout--;
    }
    if (!stopped)
        return 0;

    qh->horizontal_link =
        ((uint32_t)qh) | EHCI_LINK_QH;
    qh->endpoint = (address & 0x7Fu) |
        ((uint32_t)(endpoint & 0x0Fu) << 8) | EHCI_QH_SPEED_HIGH |
        ((uint32_t)endpoint_size << 16) | EHCI_QH_HEAD |
        (control_endpoint ? (1u << 27) : 0) |
        (control_endpoint ? 0 : EHCI_QH_DTC);
    qh->capabilities = 1u << 30;
    qh->current_qtd = 0;
    qh->next_qtd = (uint32_t)&qtds[controller_index][0];
    qh->alternate_qtd = EHCI_QTD_TERMINATE;
    qh->token = 0;
    for (i = 0; i < 5; i++)
        qh->buffer[i] = 0;
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller, EHCI_OP_USBCMD,
               (command | EHCI_CMD_RUN | EHCI_CMD_ASYNC) &
               ~EHCI_CMD_PERIODIC);

    timeout = 30000000;
    while (timeout)
    {
        uint8_t chain_done = 1;
        __asm__ volatile ("" : : : "memory");
        for (i = 0; i < td_count; i++)
        {
            uint32_t token = qtds[controller_index][i].token;
            if (token & EHCI_QTD_ACTIVE)
            {
                chain_done = 0;
                break;
            }
            if (token & (EHCI_QTD_HALTED | 0x7Cu))
            {
                chain_done = 0;
                timeout = 0;
                break;
            }
        }
        if (chain_done)
        {
            complete = 1;
            break;
        }
        timeout--;
    }

    command = mmio_read(controller, EHCI_OP_USBCMD);
    mmio_write(controller, EHCI_OP_USBCMD, command & ~EHCI_CMD_ASYNC);
    timeout = 10000000;
    stopped = 0;
    while (timeout)
    {
        if (!(mmio_read(controller, EHCI_OP_USBSTS) & EHCI_STS_ASYNC_ACTIVE))
        {
            stopped = 1;
            break;
        }
        timeout--;
    }
    qh->next_qtd = EHCI_QTD_TERMINATE;
    qh->token = 0;
    mmio_write(controller, EHCI_OP_USBCMD,
               (command | EHCI_CMD_RUN | EHCI_CMD_PERIODIC) &
               ~EHCI_CMD_ASYNC);
    return complete && stopped;
}

static int ehci_bulk_transfer(
    uint8_t controller_index,
    EhciStorage* storage,
    uint8_t input,
    uint8_t* buffer,
    uint16_t length)
{
    uint8_t endpoint = input ? storage->endpoint_in : storage->endpoint_out;
    uint16_t packet_size = input ? storage->packet_in : storage->packet_out;
    uint8_t toggle = input ? storage->toggle_in : storage->toggle_out;
    uint32_t token;

    if (!storage->present || endpoint == 0 || packet_size == 0 ||
        packet_size > 512 || length > EHCI_BUFFER_SIZE ||
        (length != 0 && buffer == 0))
        return 0;
    ehci_prepare_qtd(controller_index, 0, input ? 1 : 0,
        toggle, buffer, length, 1);
    if (!ehci_submit_control_chain(controller_index, storage->address,
            packet_size, 1, endpoint, 0))
        return 0;
    token = qtds[controller_index][0].token;
    if (token & (EHCI_QTD_ACTIVE | EHCI_QTD_HALTED | 0x7Cu))
        return 0;
    if (input)
        storage->toggle_in ^= (uint8_t)(((length + packet_size - 1u) /
            packet_size) & 1u);
    else
        storage->toggle_out ^= (uint8_t)(((length + packet_size - 1u) /
            packet_size) & 1u);
    return 1;
}

static uint32_t ehci_load_le32(const uint8_t* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void ehci_store_le32(uint8_t* data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static uint32_t ehci_load_be32(const uint8_t* data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) | data[3];
}

static void ehci_store_be32(uint8_t* data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static int ehci_bot_command(
    uint8_t controller_index,
    EhciStorage* storage,
    const uint8_t* cdb,
    uint8_t cdb_length,
    uint8_t input,
    uint8_t* data,
    uint16_t length)
{
    uint8_t* cbw = bot_cbw[controller_index];
    uint8_t* csw = bot_csw[controller_index];
    uint32_t tag;

    if (cdb_length == 0 || cdb_length > 16)
        return 0;
    for (uint8_t i = 0; i < 31; i++)
        cbw[i] = 0;
    tag = ++storage->tag;
    ehci_store_le32(cbw, EHCI_BOT_CBW_SIGNATURE);
    ehci_store_le32(cbw + 4, tag);
    ehci_store_le32(cbw + 8, length);
    cbw[12] = input ? 0x80 : 0;
    cbw[14] = cdb_length;
    for (uint8_t i = 0; i < cdb_length; i++)
        cbw[15 + i] = cdb[i];
    if (!ehci_bulk_transfer(controller_index, storage, 0, cbw, 31) ||
        (length != 0 &&
         !ehci_bulk_transfer(controller_index, storage, input, data, length)))
        return 0;
    for (uint8_t i = 0; i < 13; i++)
        csw[i] = 0;
    if (!ehci_bulk_transfer(controller_index, storage, 1, csw, 13))
        return 0;
    return ehci_load_le32(csw) == EHCI_BOT_CSW_SIGNATURE &&
        ehci_load_le32(csw + 4) == tag &&
        ehci_load_le32(csw + 8) == 0 && csw[12] == 0;
}

static int ehci_storage_identify(
    uint8_t controller_index, EhciStorage* storage)
{
    uint8_t inquiry[36];
    uint8_t capacity[8];
    uint8_t* cdb = bot_cdb[controller_index];

    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x12;
    cdb[4] = sizeof(inquiry);
    if (!ehci_bot_command(controller_index, storage, cdb, 6, 1,
                          inquiry, sizeof(inquiry)))
        return 0;
    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x25;
    if (!ehci_bot_command(controller_index, storage, cdb, 10, 1,
                          capacity, sizeof(capacity)))
        return 0;
    if (ehci_load_be32(capacity + 4) != 512u ||
        ehci_load_be32(capacity) == 0xFFFFFFFFu)
        return 0;
    storage->sectors = (uint64_t)ehci_load_be32(capacity) + 1u;
    return storage->sectors != 0;
}

static int ehci_enumerate_storage(uint8_t controller_index, uint8_t port)
{
    EhciStorage* storage = &storage_devices[controller_index][port];
    uint8_t descriptor[18];
    uint8_t config_header[9];
    uint8_t config[256];
    uint8_t address = (uint8_t)(port + 1u);
    uint8_t endpoint_size;
    uint8_t endpoint_in = 0;
    uint8_t endpoint_out = 0;
    uint16_t total;
    uint16_t offset;

    if (ehci_control_transfer(controller_index, 0, 64,
            0x80, 0x06, 0x0100, 0, descriptor, 8) &&
        descriptor[0] >= 8 && descriptor[1] == 1)
    {
        if (!ehci_control_transfer(controller_index, 0, 64,
                0, 0x05, address, 0, 0, 0))
            return 0;
        for (uint32_t delay = 0; delay < 1000000; delay++)
            __asm__ volatile ("pause");
    }
    else if (!ehci_control_transfer(controller_index, address, 64,
                 0x80, 0x06, 0x0100, 0, descriptor, 8) ||
             descriptor[0] < 8 || descriptor[1] != 1)
        return 0;
    endpoint_size = descriptor[7];
    if (endpoint_size != 8 && endpoint_size != 16 &&
        endpoint_size != 32 && endpoint_size != 64)
        return 0;
    if (!ehci_control_transfer(controller_index, address, endpoint_size,
            0x80, 0x06, 0x0200, 0, config_header, sizeof(config_header)))
        return 0;
    total = (uint16_t)config_header[2] |
        ((uint16_t)config_header[3] << 8);
    if (config_header[0] < 9 || config_header[1] != 2 || total < 9 ||
        total > sizeof(config) ||
        !ehci_control_transfer(controller_index, address, endpoint_size,
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
            uint8_t class_code = config[offset + 5];
            uint8_t subclass = config[offset + 6];
            uint8_t protocol = config[offset + 7];
            if (class_code == 8 && subclass == 6 && protocol == 0x50)
                endpoint_in = endpoint_out = 0xFF;
            else
                endpoint_in = endpoint_out = 0;
        }
        else if (type == 5 && size >= 7 &&
                 (config[offset + 3] & 3u) == 2u &&
                 (endpoint_in == 0xFF || endpoint_out == 0xFF))
        {
            uint16_t max_packet = (uint16_t)config[offset + 4] |
                ((uint16_t)config[offset + 5] << 8);
            if (max_packet == 512)
            {
                if (config[offset + 2] & 0x80u)
                    endpoint_in = config[offset + 2] & 0x0Fu;
                else
                    endpoint_out = config[offset + 2] & 0x0Fu;
            }
        }
        offset = (uint16_t)(offset + size);
    }
    if (endpoint_in == 0 || endpoint_out == 0 ||
        endpoint_in == 0xFF || endpoint_out == 0xFF || config[5] == 0 ||
        !ehci_control_transfer(controller_index, address, endpoint_size,
            0, 0x09, config[5], 0, 0, 0))
        return 0;
    storage->address = address;
    storage->endpoint_in = endpoint_in;
    storage->endpoint_out = endpoint_out;
    storage->packet_in = storage->packet_out = 512;
    storage->toggle_in = storage->toggle_out = 0;
    storage->tag = 0;
    storage->sectors = 0;
    storage->present = 1;
    if (!ehci_storage_identify(controller_index, storage))
    {
        storage->present = 0;
        return 0;
    }
    return 1;
}

static int ehci_control_transfer(
    uint8_t controller_index,
    uint8_t address,
    uint8_t endpoint_size,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint8_t* buffer,
    uint16_t length)
{
    uint8_t* setup = setup_buffers[controller_index];
    uint8_t* staging = control_buffers[controller_index];
    uint16_t offset = 0;
    uint8_t td_count = 0;
    uint8_t toggle = 1;
    uint16_t i;

    if (endpoint_size == 0 || endpoint_size > 64 ||
        length > EHCI_BUFFER_SIZE || (length != 0 && buffer == 0))
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

    ehci_prepare_qtd(controller_index, td_count++, 2, 0, setup, 8, 0);
    while (offset < length)
    {
        uint16_t packet = (uint16_t)(length - offset);
        if (packet > endpoint_size)
            packet = endpoint_size;
        if (td_count >= EHCI_QTD_COUNT - 2)
            return 0;
        ehci_prepare_qtd(controller_index, td_count++,
            (request_type & 0x80u) ? 1 : 0,
            toggle, staging + offset, packet, 0);
        toggle ^= 1;
        offset = (uint16_t)(offset + packet);
    }
    ehci_prepare_qtd(controller_index, td_count++,
        (request_type & 0x80u) ? 0 : 1, 1, 0, 0, 1);
    if (!ehci_submit_control_chain(
            controller_index, address, endpoint_size, td_count, 0, 1))
        return 0;
    if (request_type & 0x80u)
        for (i = 0; i < length; i++)
            buffer[i] = staging[i];
    return 1;
}

static int ehci_interrupt_in(uint8_t controller_index, uint8_t* report)
{
    EhciController* controller = &controllers[controller_index];
    EhciQueueHead* qh = &interrupt_heads[controller_index];
    uint32_t token;
    uint32_t timeout = 3000000;
    uint8_t complete = 0;

    if (!controller->keyboard_present)
        return 0;
    ehci_prepare_qtd(controller_index, 0, 1,
        controller->keyboard_toggle, keyboard_reports[controller_index],
        controller->keyboard_packet_size, 1);
    qh->current_qtd = 0;
    qh->next_qtd = (uint32_t)&qtds[controller_index][0];
    qh->alternate_qtd = EHCI_QTD_TERMINATE;
    qh->token = 0;
    __asm__ volatile ("" : : : "memory");
    while (timeout)
    {
        if (!(qtds[controller_index][0].token & EHCI_QTD_ACTIVE))
        {
            complete = 1;
            break;
        }
        timeout--;
    }
    token = qtds[controller_index][0].token;
    if (!complete ||
        (token & (EHCI_QTD_ACTIVE | EHCI_QTD_HALTED | 0x7Cu)))
        return 0;
    controller->keyboard_toggle ^= 1;
    for (uint8_t i = 0; i < controller->keyboard_packet_size && i < 8; i++)
        report[i] = keyboard_reports[controller_index][i];
    return 1;
}

static int ehci_enumerate_keyboard(uint8_t controller_index, uint8_t port)
{
    EhciController* controller = &controllers[controller_index];
    uint8_t descriptor[18];
    uint8_t config_header[9];
    uint8_t config_descriptor[256];
    uint8_t address = (uint8_t)(port + 1u);
    uint8_t endpoint_size;
    uint8_t endpoint = 0;
    uint8_t packet_size = 0;
    uint8_t interface_number = 0xFF;
    uint16_t total_length;
    uint16_t offset;

    if (address == 0 || address >= 128)
        return 0;
    if (!ehci_control_transfer(controller_index, 0, 64,
            0x80, 0x06, 0x0100, 0, descriptor, 8) ||
        descriptor[0] < 8 || descriptor[1] != 1)
        return 0;
    endpoint_size = descriptor[7];
    if (endpoint_size != 8 && endpoint_size != 16 &&
        endpoint_size != 32 && endpoint_size != 64)
        return 0;
    if (!ehci_control_transfer(controller_index, 0, 64,
            0x00, 0x05, address, 0, 0, 0))
        return 0;

    for (uint32_t delay = 0; delay < 1000000; delay++)
        __asm__ volatile ("pause");
    if (!ehci_control_transfer(controller_index, address, endpoint_size,
            0x80, 0x06, 0x0100, 0, descriptor, sizeof(descriptor)) ||
        !ehci_control_transfer(controller_index, address, endpoint_size,
            0x80, 0x06, 0x0200, 0, config_header, sizeof(config_header)))
        return 0;
    if (config_header[0] < 9 || config_header[1] != 2)
        return 0;
    total_length = (uint16_t)config_header[2] |
        ((uint16_t)config_header[3] << 8);
    if (total_length < 9 || total_length > sizeof(config_descriptor) ||
        !ehci_control_transfer(controller_index, address, endpoint_size,
            0x80, 0x06, 0x0200, 0, config_descriptor, total_length))
        return 0;

    for (offset = 0; offset + 2u <= total_length; )
    {
        uint8_t size = config_descriptor[offset];
        uint8_t type = config_descriptor[offset + 1];
        if (size < 2 || offset + size > total_length)
            break;
        if (type == 4 && size >= 9)
        {
            if (config_descriptor[offset + 5] == 3 &&
                config_descriptor[offset + 6] == 1 &&
                config_descriptor[offset + 7] == 1)
            {
                interface_number = config_descriptor[offset + 2];
                endpoint = 0;
                packet_size = 0;
            }
            else
                interface_number = 0xFF;
        }
        else if (type == 5 && size >= 7 && interface_number != 0xFF)
        {
            uint16_t max_packet = (uint16_t)config_descriptor[offset + 4] |
                ((uint16_t)config_descriptor[offset + 5] << 8);
            if ((config_descriptor[offset + 2] & 0x80u) &&
                (config_descriptor[offset + 3] & 3u) == 3u &&
                (max_packet & 0x7FFu) == 8)
            {
                endpoint = config_descriptor[offset + 2] & 0x0Fu;
                packet_size = 8;
            }
        }
        offset = (uint16_t)(offset + size);
    }

    if (endpoint == 0 || packet_size == 0 || config_descriptor[5] == 0 ||
        !ehci_control_transfer(controller_index, address, endpoint_size,
            0x00, 0x09, config_descriptor[5], 0, 0, 0) ||
        !ehci_control_transfer(controller_index, address, endpoint_size,
            0x21, 0x0B, 0, interface_number, 0, 0))
        return 0;

    controller->keyboard_port = (uint8_t)(port + 1u);
    controller->keyboard_address = address;
    controller->keyboard_endpoint = endpoint;
    controller->keyboard_packet_size = packet_size;
    controller->keyboard_toggle = 0;
    controller->keyboard_present = 1;
    interrupt_heads[controller_index].horizontal_link = EHCI_LINK_TERMINATE;
    interrupt_heads[controller_index].endpoint = address |
        ((uint32_t)endpoint << 8) |
        EHCI_QH_SPEED_HIGH | EHCI_QH_DTC |
        ((uint32_t)packet_size << 16);
    interrupt_heads[controller_index].capabilities = 1u | (1u << 16);
    interrupt_heads[controller_index].current_qtd = 0;
    interrupt_heads[controller_index].next_qtd = EHCI_QTD_TERMINATE;
    interrupt_heads[controller_index].alternate_qtd = EHCI_QTD_TERMINATE;
    interrupt_heads[controller_index].token = 0;
    for (uint8_t i = 0; i < 5; i++)
        interrupt_heads[controller_index].buffer[i] = 0;
    for (uint16_t i = 0; i < 1024; i++)
        periodic_lists[controller_index][i] =
            (uint32_t)&interrupt_heads[controller_index] | EHCI_LINK_QH;
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller, EHCI_OP_USBCMD,
        mmio_read(controller, EHCI_OP_USBCMD) | EHCI_CMD_PERIODIC);
    return 1;
}

static char ehci_usage_to_key(uint8_t usage, uint8_t shift)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static const char digits[] = "1234567890";
    if (usage >= 4 && usage <= 0x1D)
    {
        char ch = letters[usage - 4];
        return shift ? (char)(ch - 'a' + 'A') : ch;
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

int usb_ehci_keyboard_poll(char* key)
{
    uint8_t report[8] = { 0 };
    if (key == 0)
        return 0;
    for (uint8_t c = 0; c < controller_count; c++)
    {
        EhciController* controller = &controllers[c];
        if (!controller->keyboard_present ||
            !ehci_interrupt_in(c, report))
            continue;
        for (uint8_t i = 2; i < 8; i++)
        {
            uint8_t previous = 0;
            uint8_t j;
            char translated;
            if (report[i] == 0)
                continue;
            for (j = 2; j < 8; j++)
                if (controller->previous_report[j] == report[i])
                    previous = 1;
            if (previous)
                continue;
            translated = ehci_usage_to_key(report[i],
                (report[0] & 0x22u) != 0);
            if (translated != 0)
            {
                for (j = 0; j < 8; j++)
                    controller->previous_report[j] = report[j];
                *key = translated;
                return 1;
            }
        }
        for (uint8_t i = 0; i < 8; i++)
            controller->previous_report[i] = report[i];
    }
    return 0;
}

int usb_ehci_start(uint8_t bus, uint8_t device, uint8_t function)
{
    EhciController* controller;
    uint32_t base;
    uint32_t cap_length;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint32_t command;
    uint32_t timeout;

    if (controller_count >= EHCI_MAX_CONTROLLERS)
        return 0;
    base = usb_pci_enable_mmio(bus, device, function, 0x10);
    if (base == 0)
        return 0;

    cap_length = *(volatile uint8_t*)base;
    if (cap_length < 0x10u || cap_length > 0x40u)
        return 0;
    controller = &controllers[controller_count];
    controller->base = base;
    controller->operational = base + cap_length;
    controller->present = 0;
    hcsparams = *(volatile uint32_t*)(base + EHCI_CAP_HCSPARAMS);
    hccparams = *(volatile uint32_t*)(base + EHCI_CAP_HCCPARAMS);
    controller->ports = (uint8_t)(hcsparams & 0x0Fu);
    if (controller->ports == 0 || controller->ports > EHCI_MAX_PORTS)
        return 0;

    if (!usb_pci_ehci_handoff(bus, device, function, hccparams))
        return 0;

    command = mmio_read(controller, EHCI_OP_USBCMD);
    mmio_write(controller, EHCI_OP_USBCMD, command & ~EHCI_CMD_RUN);
    timeout = 1000000;
    while (timeout-- && !(mmio_read(controller, EHCI_OP_USBSTS) & EHCI_STS_HALTED))
        ;
    if (!(mmio_read(controller, EHCI_OP_USBSTS) & EHCI_STS_HALTED))
        return 0;

    mmio_write(controller, EHCI_OP_USBCMD, EHCI_CMD_RESET);
    timeout = 1000000;
    while (timeout-- && (mmio_read(controller, EHCI_OP_USBCMD) & EHCI_CMD_RESET))
        ;
    if (mmio_read(controller, EHCI_OP_USBCMD) & EHCI_CMD_RESET)
        return 0;

    async_heads[controller_count].horizontal_link =
        ((uint32_t)&async_heads[controller_count]) | EHCI_LINK_QH;
    async_heads[controller_count].endpoint = 1u << 15;
    async_heads[controller_count].next_qtd = EHCI_LINK_TERMINATE;
    async_heads[controller_count].alternate_qtd = EHCI_LINK_TERMINATE;
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller, EHCI_OP_PERIODIC,
               (uint32_t)periodic_lists[controller_count]);
    mmio_write(controller, EHCI_OP_ASYNC,
               (uint32_t)&async_heads[controller_count]);
    mmio_write(controller, EHCI_OP_CONFIG, 1);
    mmio_write(controller, EHCI_OP_USBCMD, EHCI_CMD_RUN | EHCI_CMD_ASYNC);
    timeout = 1000000;
    while (timeout-- && (mmio_read(controller, EHCI_OP_USBSTS) & EHCI_STS_HALTED))
        ;
    if (mmio_read(controller, EHCI_OP_USBSTS) & EHCI_STS_HALTED)
        return 0;

    controller->present = 1;
    controller_count++;
    for (uint16_t frame = 0; frame < 1024; frame++)
        periodic_lists[controller_count - 1][frame] = EHCI_LINK_TERMINATE;
    for (uint8_t port = 0; port < controller->ports; port++)
    {
        uint32_t offset = EHCI_OP_PORTSC + (uint32_t)port * 4u;
        uint32_t status = mmio_read(controller, offset);
        if (hcsparams & (1u << 4))
            mmio_write(controller, offset,
                (status & ~(EHCI_PORT_CHANGE | EHCI_PORT_RESET)) |
                (status & EHCI_PORT_CHANGE) | EHCI_PORT_POWER);
    }
    if (hcsparams & (1u << 4))
        wait_for_reset_interval(controller);
    for (uint8_t port = 0; port < controller->ports; port++)
    {
        uint32_t offset = EHCI_OP_PORTSC + (uint32_t)port * 4u;
        uint32_t status = mmio_read(controller, offset);
        controller->connected[port] = (status & EHCI_PORT_CONNECTED) != 0;
        if (status & EHCI_PORT_CONNECTED)
        {
            if (status & EHCI_PORT_OWNER)
                continue;
            reset_high_speed_port(controller, port);
            status = mmio_read(controller, offset);
            if ((status & EHCI_PORT_ENABLE) &&
                (status & EHCI_PORT_LINE_STATUS) == EHCI_PORT_HIGH_SPEED)
            {
                if (controller->keyboard_present ||
                    !ehci_enumerate_keyboard(
                        (uint8_t)(controller_count - 1), port))
                    ehci_enumerate_storage(
                        (uint8_t)(controller_count - 1), port);
            }
        }
    }
    return 1;
}

void usb_ehci_poll(void)
{
    uint8_t index;

    for (index = 0; index < controller_count; index++)
    {
        EhciController* controller = &controllers[index];
        uint8_t port;

        if (!controller->present)
            continue;
        for (port = 0; port < controller->ports; port++)
        {
            uint32_t offset = EHCI_OP_PORTSC + (uint32_t)port * 4u;
            uint32_t status = mmio_read(controller, offset);
            uint8_t connected = (status & EHCI_PORT_CONNECTED) != 0;

            if (status & EHCI_PORT_CHANGE)
                mmio_write(controller, offset,
                    (status & ~(EHCI_PORT_CHANGE | EHCI_PORT_RESET)) |
                    (status & EHCI_PORT_CHANGE));
            if (!connected && controller->keyboard_present &&
                controller->keyboard_port == (uint8_t)(port + 1u))
            {
                controller->keyboard_present = 0;
                controller->keyboard_toggle = 0;
                for (uint8_t key = 0; key < 8; key++)
                    controller->previous_report[key] = 0;
                for (uint16_t frame = 0; frame < 1024; frame++)
                    periodic_lists[index][frame] = EHCI_LINK_TERMINATE;
                mmio_write(controller, EHCI_OP_USBCMD,
                    mmio_read(controller, EHCI_OP_USBCMD) &
                    ~EHCI_CMD_PERIODIC);
            }
            if (!connected)
            {
                storage_devices[index][port].present = 0;
                storage_devices[index][port].sectors = 0;
            }
            if (connected && !controller->connected[port])
            {
                if (status & EHCI_PORT_OWNER)
                {
                    controller->connected[port] = connected;
                    continue;
                }
                reset_high_speed_port(controller, port);
                status = mmio_read(controller, offset);
                if ((status & EHCI_PORT_ENABLE) &&
                    (status & EHCI_PORT_LINE_STATUS) == EHCI_PORT_HIGH_SPEED)
                {
                    if (controller->keyboard_present ||
                        !ehci_enumerate_keyboard(index, port))
                        ehci_enumerate_storage(index, port);
                    else
                    {
                        controller->connected[port] = connected;
                        continue;
                    }
                }
            }
            controller->connected[port] = connected;
        }
    }

}

int usb_ehci_storage_sector_count(uint8_t drive, uint64_t* sectors)
{
    uint8_t slot;
    uint8_t controller_index;
    uint8_t port;
    EhciStorage* storage;

    if (sectors == 0 || drive < EHCI_DRIVE_BASE ||
        drive >= EHCI_DRIVE_BASE + EHCI_DRIVE_COUNT)
        return 0;
    slot = (uint8_t)(drive - EHCI_DRIVE_BASE);
    controller_index = (uint8_t)(slot / EHCI_PORT_COUNT);
    port = (uint8_t)(slot % EHCI_PORT_COUNT);
    if (controller_index >= controller_count)
        return 0;
    storage = &storage_devices[controller_index][port];
    if (!storage->present || storage->sectors == 0)
        return 0;
    *sectors = storage->sectors;
    return 1;
}

int usb_ehci_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer)
{
    uint8_t slot, controller_index, port;
    uint8_t* cdb;
    EhciStorage* storage;

    if (buffer == 0 || drive < EHCI_DRIVE_BASE ||
        drive >= EHCI_DRIVE_BASE + EHCI_DRIVE_COUNT)
        return 0;
    slot = (uint8_t)(drive - EHCI_DRIVE_BASE);
    controller_index = (uint8_t)(slot / EHCI_PORT_COUNT);
    port = (uint8_t)(slot % EHCI_PORT_COUNT);
    if (controller_index >= controller_count)
        return 0;
    storage = &storage_devices[controller_index][port];
    if (!storage->present || lba >= storage->sectors || lba > 0xFFFFFFFFu)
        return 0;
    cdb = bot_cdb[controller_index];
    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x28;
    ehci_store_be32(cdb + 2, (uint32_t)lba);
    cdb[8] = 1;
    if (!ehci_bot_command(controller_index, storage, cdb, 10, 1,
            bulk_buffers[controller_index], 512))
        return 0;
    for (uint16_t i = 0; i < 512; i++)
        buffer[i] = bulk_buffers[controller_index][i];
    return 1;
}

int usb_ehci_storage_write(
    uint8_t drive, uint64_t lba, const uint8_t* buffer)
{
    uint8_t slot, controller_index, port;
    uint8_t* cdb;
    EhciStorage* storage;

    if (buffer == 0 || drive < EHCI_DRIVE_BASE ||
        drive >= EHCI_DRIVE_BASE + EHCI_DRIVE_COUNT)
        return 0;
    slot = (uint8_t)(drive - EHCI_DRIVE_BASE);
    controller_index = (uint8_t)(slot / EHCI_PORT_COUNT);
    port = (uint8_t)(slot % EHCI_PORT_COUNT);
    if (controller_index >= controller_count)
        return 0;
    storage = &storage_devices[controller_index][port];
    if (!storage->present || lba >= storage->sectors || lba > 0xFFFFFFFFu)
        return 0;
    for (uint16_t i = 0; i < 512; i++)
        bulk_buffers[controller_index][i] = buffer[i];
    cdb = bot_cdb[controller_index];
    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x2A;
    ehci_store_be32(cdb + 2, (uint32_t)lba);
    cdb[8] = 1;
    return ehci_bot_command(controller_index, storage, cdb, 10, 0,
        bulk_buffers[controller_index], 512);
}
