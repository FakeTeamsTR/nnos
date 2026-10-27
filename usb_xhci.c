#include "types.h"
#include "ahci.h"
#include "usb_pci.h"

#define XHCI_MAX_CONTROLLERS 2
#define XHCI_MAX_SCRATCHPADS 64
#define XHCI_RING_SIZE       256
#define XHCI_TRANSFER_RING_SIZE 16
#define XHCI_USBCMD          0x00
#define XHCI_USBSTS          0x04
#define XHCI_PAGESIZE        0x08
#define XHCI_CRCR            0x18
#define XHCI_DCBAAP          0x30
#define XHCI_CONFIG          0x38
#define XHCI_STS_HALTED      0x00000001u
#define XHCI_STS_NOT_READY   0x00000800u
#define XHCI_CMD_RUN         0x00000001u
#define XHCI_CMD_RESET       0x00000002u
#define XHCI_PORTSC_BASE     0x400
#define XHCI_PORTSC_CONNECTED 0x00000001u
#define XHCI_PORTSC_RESET     0x00000010u
#define XHCI_PORTSC_WARM_RESET 0x80000000u
#define XHCI_PORTSC_POWER     0x00000200u
#define XHCI_PORTSC_CONNECT_CHANGE 0x00020000u
#define XHCI_PORTSC_WARM_RESET_CHANGE 0x00080000u
#define XHCI_PORTSC_RESET_CHANGE   0x00200000u
#define XHCI_PORTSC_RW_MASK        0x8001C3F2u
#define XHCI_PORTSC_ENABLE          0x00000002u
#define XHCI_TRB_CYCLE              0x00000001u
#define XHCI_TRB_IOC                0x00000020u
#define XHCI_TRB_IDT                0x00000040u
#define XHCI_TRB_CHAIN              0x00000010u
#define XHCI_TRB_TYPE_SHIFT         10
#define XHCI_TRB_TYPE_MASK          (0x3Fu << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_SETUP_STAGE        (2u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_DATA_STAGE         (3u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_STATUS_STAGE       (4u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_LINK               (6u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_ENABLE_SLOT        (9u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_DISABLE_SLOT       (10u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_ADDRESS_DEVICE     (11u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_CONFIGURE_ENDPOINT (12u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_EVALUATE_CONTEXT   (13u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_NORMAL             (1u << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_TRANSFER_EVENT     32u
#define XHCI_TRB_COMMAND_EVENT      33u
#define XHCI_TRB_PORT_EVENT         34u
#define XHCI_COMPLETION_SUCCESS     1u
#define XHCI_MAX_CONTEXT_DWORDS     16
#define XHCI_MAX_KEYBOARD_ENDPOINTS 16
#define XHCI_BOT_CBW_SIGNATURE 0x43425355u
#define XHCI_BOT_CSW_SIGNATURE 0x53425355u

typedef struct
{
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
} XhciTrb;

typedef struct __attribute__((aligned(64)))
{
    uint64_t ring_segment;
    uint32_t ring_size;
    uint32_t reserved;
} XhciErstEntry;

typedef struct
{
    uint32_t base;
    uint32_t operational;
    uint32_t runtime;
    uint32_t doorbells;
    uint8_t ports;
    uint8_t slots;
    uint8_t present;
    uint8_t event_cycle;
    uint16_t event_index;
    uint8_t connected[256];
    uint16_t command_index;
    uint8_t command_cycle;
    uint8_t context_size;
    uint8_t ep0_ring_index;
    uint8_t ep0_ring_cycle;
    uint8_t keyboard_slot;
    uint8_t keyboard_port;
    uint8_t keyboard_endpoint_id;
    uint8_t keyboard_packet_size;
    uint8_t keyboard_toggle;
    uint8_t keyboard_ring_index;
    uint8_t keyboard_ring_cycle;
    uint8_t storage_in_index;
    uint8_t storage_in_cycle;
    uint8_t storage_out_index;
    uint8_t storage_out_cycle;
    uint8_t keyboard_interval;
    uint8_t keyboard_present;
    uint8_t storage_present;
    uint8_t storage_port;
    uint8_t storage_endpoint_in;
    uint8_t storage_endpoint_out;
    uint16_t storage_packet_in;
    uint16_t storage_packet_out;
    uint8_t storage_burst_in;
    uint8_t storage_burst_out;
    uint64_t storage_sectors;
    uint32_t storage_tag;
    uint8_t previous_report[8];
} XhciController;

static XhciController controllers[XHCI_MAX_CONTROLLERS];
static uint64_t dcbaa[XHCI_MAX_CONTROLLERS][256]
    __attribute__((aligned(64)));
static uint64_t scratchpad_array[XHCI_MAX_CONTROLLERS][XHCI_MAX_SCRATCHPADS]
    __attribute__((aligned(64)));
static uint8_t scratchpad_pages[XHCI_MAX_CONTROLLERS]
    [XHCI_MAX_SCRATCHPADS][4096] __attribute__((aligned(4096)));
static uint32_t input_contexts[XHCI_MAX_CONTROLLERS][33 *
    XHCI_MAX_CONTEXT_DWORDS] __attribute__((aligned(64)));
static uint32_t output_contexts[XHCI_MAX_CONTROLLERS][256][32 *
    XHCI_MAX_CONTEXT_DWORDS] __attribute__((aligned(64)));
static XhciTrb control_rings[XHCI_MAX_CONTROLLERS]
    [XHCI_TRANSFER_RING_SIZE] __attribute__((aligned(64)));
static XhciTrb keyboard_rings[XHCI_MAX_CONTROLLERS]
    [XHCI_TRANSFER_RING_SIZE] __attribute__((aligned(64)));
static XhciTrb storage_in_rings[XHCI_MAX_CONTROLLERS]
    [XHCI_TRANSFER_RING_SIZE] __attribute__((aligned(64)));
static XhciTrb storage_out_rings[XHCI_MAX_CONTROLLERS]
    [XHCI_TRANSFER_RING_SIZE] __attribute__((aligned(64)));
static uint8_t setup_buffers[XHCI_MAX_CONTROLLERS][8]
    __attribute__((aligned(16)));
static uint8_t control_buffers[XHCI_MAX_CONTROLLERS][256]
    __attribute__((aligned(4096)));
static uint8_t keyboard_reports[XHCI_MAX_CONTROLLERS][8]
    __attribute__((aligned(16)));
static uint8_t storage_buffers[XHCI_MAX_CONTROLLERS][4096]
    __attribute__((aligned(4096)));
static uint8_t storage_cbw[XHCI_MAX_CONTROLLERS][31]
    __attribute__((aligned(16)));
static uint8_t storage_csw[XHCI_MAX_CONTROLLERS][13]
    __attribute__((aligned(16)));
static uint8_t storage_cdb[XHCI_MAX_CONTROLLERS][16]
    __attribute__((aligned(16)));
static uint8_t context_temporary[XHCI_MAX_CONTROLLERS][64]
    __attribute__((aligned(64)));
static XhciTrb command_rings[XHCI_MAX_CONTROLLERS][XHCI_RING_SIZE]
    __attribute__((aligned(64)));
static XhciTrb event_rings[XHCI_MAX_CONTROLLERS][XHCI_RING_SIZE]
    __attribute__((aligned(64)));
static XhciErstEntry erst[XHCI_MAX_CONTROLLERS]
    __attribute__((aligned(64)));
static uint8_t controller_count;

static int command_ok(
    XhciController* controller,
    uint8_t controller_index,
    uint32_t parameter,
    uint32_t command_type,
    uint8_t slot_id,
    XhciTrb* event);
static int enumerate_xhci_storage(uint8_t controller_index, uint8_t port);

static uint32_t mmio_read(uint32_t address)
{
    return *(volatile uint32_t*)address;
}

static void mmio_write(uint32_t address, uint32_t value)
{
    *(volatile uint32_t*)address = value;
}

static int wait_for_port_reset(uint32_t portsc)
{
    uint32_t timeout = 10000000;

    while (timeout--)
    {
        uint32_t status = mmio_read(portsc);
        if (!(status & (XHCI_PORTSC_RESET | XHCI_PORTSC_WARM_RESET)))
            return (status & XHCI_PORTSC_ENABLE) != 0;
    }
    return 0;
}

static int reset_xhci_port(XhciController* controller, uint8_t port)
{
    uint32_t portsc = controller->operational +
        XHCI_PORTSC_BASE + (uint32_t)port * 0x10u;
    uint32_t status = mmio_read(portsc);
    uint32_t reset;
    uint8_t speed;

    if (!(status & XHCI_PORTSC_CONNECTED))
        return 0;
    speed = (uint8_t)((status >> 10) & 0x0Fu);
    reset = speed >= 4 ? XHCI_PORTSC_WARM_RESET : XHCI_PORTSC_RESET;
    mmio_write(portsc,
        (status & XHCI_PORTSC_RW_MASK) |
        XHCI_PORTSC_POWER | reset);
    if (!wait_for_port_reset(portsc))
        return 0;
    return (mmio_read(portsc) & XHCI_PORTSC_CONNECTED) != 0;
}

static void release_xhci_slot(
    XhciController* controller, uint8_t controller_index)
{
    XhciTrb event;
    uint8_t slot = controller->keyboard_slot;

    if (slot != 0)
    {
        if (command_ok(controller, controller_index, 0,
                XHCI_TRB_DISABLE_SLOT, slot, &event))
            dcbaa[controller_index][slot] = 0;
    }
    controller->keyboard_present = 0;
    controller->storage_present = 0;
    controller->storage_sectors = 0;
    controller->keyboard_slot = 0;
    controller->keyboard_toggle = 0;
    for (uint8_t i = 0; i < 8; i++)
        controller->previous_report[i] = 0;
}

static void wait_for_port_power(XhciController* controller)
{
    uint32_t previous = mmio_read(controller->runtime) & 0x3FFFu;
    uint32_t elapsed = 0;
    uint32_t timeout = 20000000;

    while (elapsed < 160u && timeout--)
    {
        uint32_t current = mmio_read(controller->runtime) & 0x3FFFu;
        if (current != previous)
        {
            elapsed += (current - previous) & 0x3FFFu;
            previous = current;
        }
    }
}

static int next_event(
    XhciController* controller, uint8_t index, XhciTrb* result)
{
    XhciTrb* event = &event_rings[index][controller->event_index];
    uint32_t control = event->control;

    __asm__ volatile ("" : : : "memory");
    if ((control & XHCI_TRB_CYCLE) != controller->event_cycle)
        return 0;
    *result = *event;
    controller->event_index++;
    if (controller->event_index == XHCI_RING_SIZE)
    {
        controller->event_index = 0;
        controller->event_cycle ^= 1u;
    }
    mmio_write(controller->runtime + 0x38,
        ((uint32_t)&event_rings[index][controller->event_index]) | 0x08u);
    mmio_write(controller->runtime + 0x3C, 0);
    return 1;
}

static int wait_event(
    XhciController* controller,
    uint8_t controller_index,
    uint8_t event_type,
    uint8_t slot_id,
    uint32_t timeout,
    XhciTrb* result)
{
    XhciTrb event;

    while (timeout--)
    {
        if (!next_event(controller, controller_index, &event))
            continue;
        if (((event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3Fu) != event_type)
            continue;
        if (slot_id != 0 && (event.control >> 24) != slot_id)
            continue;
        if (result != 0)
            *result = event;
        return 1;
    }
    return 0;
}

static void drain_events(XhciController* controller, uint8_t index)
{
    XhciTrb event;
    while (next_event(controller, index, &event))
        ;
}

static int submit_command(
    XhciController* controller,
    uint8_t controller_index,
    uint32_t parameter_low,
    uint32_t parameter_high,
    uint32_t status,
    uint32_t control,
    XhciTrb* completion)
{
    XhciTrb* trb;

    if (controller->command_index >= XHCI_RING_SIZE - 1)
    {
        XhciTrb* link = &command_rings[controller_index][XHCI_RING_SIZE - 1];
        link->parameter_low = (uint32_t)&command_rings[controller_index][0];
        link->parameter_high = 0;
        link->status = 0;
        link->control = XHCI_TRB_LINK |
            (controller->command_cycle ? XHCI_TRB_CYCLE : 0) |
            (1u << 1);
        controller->command_index = 0;
        controller->command_cycle ^= 1u;
    }
    trb = &command_rings[controller_index][controller->command_index];
    trb->parameter_low = parameter_low;
    trb->parameter_high = parameter_high;
    trb->status = status;
    trb->control = control |
        (controller->command_cycle ? XHCI_TRB_CYCLE : 0);
    __asm__ volatile ("" : : : "memory");
    controller->command_index++;
    mmio_write(controller->doorbells, 0);
    return wait_event(controller, controller_index,
        XHCI_TRB_COMMAND_EVENT, 0, 10000000, completion);
}

static void clear_input_context(uint8_t index)
{
    for (uint16_t dword = 0; dword < 33 * XHCI_MAX_CONTEXT_DWORDS; dword++)
        input_contexts[index][dword] = 0;
}

static uint32_t* input_context(
    uint8_t controller_index, uint8_t context_number)
{
    return &input_contexts[controller_index][
        (uint32_t)context_number * (controllers[controller_index].context_size / 4u)];
}

static void push_transfer_trb(
    XhciTrb* ring,
    uint8_t* index,
    uint8_t* cycle,
    uint32_t parameter_low,
    uint32_t parameter_high,
    uint32_t status,
    uint32_t control)
{
    if (*index == XHCI_TRANSFER_RING_SIZE - 1)
    {
        XhciTrb* link = &ring[*index];
        link->parameter_low = (uint32_t)ring;
        link->parameter_high = 0;
        link->status = 0;
        link->control = XHCI_TRB_LINK | (*cycle ? 1u : 0u) | (1u << 1);
        *index = 0;
        *cycle ^= 1u;
    }

    ring[*index].parameter_low = parameter_low;
    ring[*index].parameter_high = parameter_high;
    ring[*index].status = status;
    ring[*index].control = control | (*cycle ? 1u : 0u);
    (*index)++;
}

static void set_input_context64(uint32_t* context, uint8_t dword, uint32_t value)
{
    context[dword] = value;
}

static uint8_t xhci_endpoint_interval(uint8_t speed, uint8_t interval)
{
    uint8_t exponent = 0;
    uint16_t period = 1;

    if (interval == 0)
        interval = 1;
    if (speed >= 3)
        return interval - 1u;
    while (period < interval && exponent < 8)
    {
        period <<= 1;
        exponent++;
    }
    return (uint8_t)(exponent + 3u);
}

static int transfer_control(
    XhciController* controller,
    uint8_t controller_index,
    uint8_t slot_id,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint8_t* buffer,
    uint16_t length)
{
    XhciTrb* ring = control_rings[controller_index];
    uint8_t* setup = setup_buffers[controller_index];
    uint8_t count = 0;
    uint8_t* ring_index = &controller->ep0_ring_index;
    uint8_t* cycle = &controller->ep0_ring_cycle;
    uint8_t data_in = (request_type & 0x80u) != 0;
    XhciTrb event;

    if (length > sizeof(control_buffers[controller_index]) ||
        (length != 0 && buffer == 0))
        return 0;
    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (uint8_t)value;
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index;
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)length;
    setup[7] = (uint8_t)(length >> 8);
    for (uint16_t i = 0; i < length; i++)
        control_buffers[controller_index][i] = data_in ? 0 : buffer[i];

    push_transfer_trb(ring, ring_index, cycle,
        (uint32_t)setup[0] | ((uint32_t)setup[1] << 8) |
        ((uint32_t)setup[2] << 16) | ((uint32_t)setup[3] << 24),
        (uint32_t)setup[4] | ((uint32_t)setup[5] << 8) |
        ((uint32_t)setup[6] << 16) | ((uint32_t)setup[7] << 24),
        8, XHCI_TRB_SETUP_STAGE | XHCI_TRB_IDT | XHCI_TRB_CHAIN |
        (data_in ? (3u << 16) : (length != 0 ? (2u << 16) : 0)));
    count++;

    if (length != 0)
    {
        push_transfer_trb(ring, ring_index, cycle,
            (uint32_t)control_buffers[controller_index], 0, length,
            XHCI_TRB_DATA_STAGE | XHCI_TRB_CHAIN |
            (data_in ? (1u << 16) : 0));
        count++;
    }
    push_transfer_trb(ring, ring_index, cycle, 0, 0, 0,
        XHCI_TRB_STATUS_STAGE | XHCI_TRB_IOC |
        (!data_in ? (1u << 16) : 0));
    count++;
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller->doorbells + (uint32_t)slot_id * 4u, 1);
    if (!wait_event(controller, controller_index,
            XHCI_TRB_TRANSFER_EVENT, slot_id, 10000000, &event) ||
        ((event.status >> 24) & 0xFFu) != XHCI_COMPLETION_SUCCESS)
        return 0;
    if (data_in)
        for (uint16_t i = 0; i < length; i++)
            buffer[i] = control_buffers[controller_index][i];
    (void)count;
    return 1;
}

static int command_ok(
    XhciController* controller,
    uint8_t controller_index,
    uint32_t parameter,
    uint32_t command_type,
    uint8_t slot_id,
    XhciTrb* event)
{
    uint32_t control = command_type;
    if (slot_id != 0)
        control |= (uint32_t)slot_id << 24;
    if (!submit_command(controller, controller_index,
            parameter, 0, 0, control, event))
        return 0;
    return ((event->status >> 24) & 0xFFu) == XHCI_COMPLETION_SUCCESS;
}

static void reset_transfer_ring(
    XhciTrb* ring, uint8_t* index, uint8_t* cycle)
{
    for (uint8_t i = 0; i < XHCI_TRANSFER_RING_SIZE; i++)
    {
        ring[i].parameter_low = 0;
        ring[i].parameter_high = 0;
        ring[i].status = 0;
        ring[i].control = 0;
    }
    *index = 0;
    *cycle = 1;
}

static int update_ep0_packet_size(
    XhciController* controller,
    uint8_t controller_index,
    uint8_t slot_id,
    uint8_t packet_size)
{
    XhciTrb event;
    uint32_t* input_control;
    uint32_t* endpoint0_context;

    clear_input_context(controller_index);
    input_control = input_context(controller_index, 0);
    endpoint0_context = input_context(controller_index, 2);
    set_input_context64(input_control, 1, 1u << 1);
    set_input_context64(endpoint0_context, 0, 0);
    set_input_context64(endpoint0_context, 1,
        (3u << 1) | (4u << 3) | ((uint32_t)packet_size << 16));
    set_input_context64(endpoint0_context, 2,
        (uint32_t)&control_rings[controller_index]
            [controller->ep0_ring_index] |
        (controller->ep0_ring_cycle ? 1u : 0u));
    set_input_context64(endpoint0_context, 3, 0);
    set_input_context64(endpoint0_context, 4, 8);
    __asm__ volatile ("" : : : "memory");
    if (!command_ok(controller, controller_index,
            (uint32_t)&input_contexts[controller_index][0],
            XHCI_TRB_EVALUATE_CONTEXT, slot_id, &event))
        return 0;
    return ((event.status >> 24) & 0xFFu) == XHCI_COMPLETION_SUCCESS;
}

static uint32_t xhci_load_le32(const uint8_t* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void xhci_store_le32(uint8_t* data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static uint32_t xhci_load_be32(const uint8_t* data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) | data[3];
}

static void xhci_store_be32(uint8_t* data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static int xhci_bulk_transfer(
    XhciController* controller,
    uint8_t controller_index,
    uint8_t slot,
    uint8_t endpoint_id,
    XhciTrb* ring,
    uint8_t* ring_index,
    uint8_t* ring_cycle,
    uint8_t* data,
    uint16_t length,
    uint8_t input)
{
    XhciTrb event;
    if (length > 4096 || (length != 0 && data == 0))
        return 0;
    push_transfer_trb(ring, ring_index, ring_cycle,
        (uint32_t)data, 0, length,
        XHCI_TRB_NORMAL | XHCI_TRB_IOC);
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller->doorbells + (uint32_t)slot * 4u,
        endpoint_id);
    return wait_event(controller, controller_index,
        XHCI_TRB_TRANSFER_EVENT, slot, 10000000, &event) &&
        ((event.status >> 24) & 0xFFu) == XHCI_COMPLETION_SUCCESS;
}

static int xhci_bot_command(
    XhciController* controller,
    uint8_t controller_index,
    uint8_t slot,
    const uint8_t* cdb,
    uint8_t cdb_length,
    uint8_t input,
    uint8_t* data,
    uint16_t length)
{
    uint8_t* cbw = storage_cbw[controller_index];
    uint8_t* csw = storage_csw[controller_index];
    uint32_t tag;

    if (cdb_length == 0 || cdb_length > 16)
        return 0;
    for (uint8_t i = 0; i < 31; i++)
        cbw[i] = 0;
    tag = ++controller->storage_tag;
    xhci_store_le32(cbw, XHCI_BOT_CBW_SIGNATURE);
    xhci_store_le32(cbw + 4, tag);
    xhci_store_le32(cbw + 8, length);
    cbw[12] = input ? 0x80 : 0;
    cbw[14] = cdb_length;
    for (uint8_t i = 0; i < cdb_length; i++)
        cbw[15 + i] = cdb[i];
    if (!xhci_bulk_transfer(controller, controller_index, slot,
            controller->storage_endpoint_out, storage_out_rings[controller_index],
            &controller->storage_out_index,
            &controller->storage_out_cycle, cbw, 31, 0))
        return 0;
    if (length != 0 &&
        !xhci_bulk_transfer(controller, controller_index, slot,
            input ? controller->storage_endpoint_in :
                    controller->storage_endpoint_out,
            input ? storage_in_rings[controller_index] :
                    storage_out_rings[controller_index],
            input ? &controller->storage_in_index :
                    &controller->storage_out_index,
            input ? &controller->storage_in_cycle :
                    &controller->storage_out_cycle,
            data, length, input))
        return 0;
    for (uint8_t i = 0; i < 13; i++)
        csw[i] = 0;
    if (!xhci_bulk_transfer(controller, controller_index, slot,
            controller->storage_endpoint_in, storage_in_rings[controller_index],
            &controller->storage_in_index,
            &controller->storage_in_cycle, csw, 13, 1))
        return 0;
    return xhci_load_le32(csw) == XHCI_BOT_CSW_SIGNATURE &&
        xhci_load_le32(csw + 4) == tag &&
        xhci_load_le32(csw + 8) == 0 && csw[12] == 0;
}

static int enumerate_xhci_storage(uint8_t controller_index, uint8_t port)
{
    XhciController* controller = &controllers[controller_index];
    XhciTrb event;
    uint8_t speed;
    uint8_t slot;
    uint8_t ep0_size;
    uint8_t descriptor[18];
    uint8_t config_header[9];
    uint8_t config[256];
    uint8_t endpoint_in = 0;
    uint8_t endpoint_out = 0;
    uint8_t endpoint_in_id;
    uint8_t endpoint_out_id;
    uint8_t last_endpoint_id;
    uint16_t packet_in = 0;
    uint16_t packet_out = 0;
    uint8_t burst_in = 0;
    uint8_t burst_out = 0;
    uint16_t ep_in_offset = 0;
    uint16_t ep_out_offset = 0;
    uint16_t total;
    uint16_t offset;
    uint8_t interface_active = 0;
    uint32_t* input_control;
    uint32_t* slot_context;
    uint32_t* endpoint0_context;
    uint32_t* in_context;
    uint32_t* out_context;

    if (port >= XHCI_STORAGE_PORT_COUNT ||
        controller->storage_present || controller->keyboard_present)
        return 0;
    speed = (uint8_t)((mmio_read(controller->operational +
        XHCI_PORTSC_BASE + (uint32_t)port * 0x10u) >> 10) & 0x0Fu);
    if (speed < 2 || speed > 4)
        return 0;
    ep0_size = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    if (!command_ok(controller, controller_index, 0,
            XHCI_TRB_ENABLE_SLOT, 0, &event))
        return 0;
    slot = (uint8_t)(event.control >> 24);
    if (slot == 0 || slot > controller->slots)
        return 0;
    clear_input_context(controller_index);
    reset_transfer_ring(control_rings[controller_index],
        &controller->ep0_ring_index, &controller->ep0_ring_cycle);
    for (uint8_t i = 0; i < 32 * XHCI_MAX_CONTEXT_DWORDS; i++)
        output_contexts[controller_index][slot][i] = 0;
    dcbaa[controller_index][slot] =
        (uint32_t)&output_contexts[controller_index][slot][0];
    input_control = input_context(controller_index, 0);
    slot_context = input_context(controller_index, 1);
    endpoint0_context = input_context(controller_index, 2);
    set_input_context64(input_control, 1, 3);
    set_input_context64(slot_context, 0,
        ((uint32_t)speed << 20) | (1u << 27));
    set_input_context64(slot_context, 1, (uint32_t)(port + 1u) << 16);
    set_input_context64(endpoint0_context, 0, (3u << 1) | (4u << 3));
    set_input_context64(endpoint0_context, 1, (uint32_t)ep0_size << 16);
    set_input_context64(endpoint0_context, 2,
        (uint32_t)&control_rings[controller_index][0] | 1u);
    set_input_context64(endpoint0_context, 4, 8);
    if (!command_ok(controller, controller_index,
            (uint32_t)&input_contexts[controller_index][0],
            XHCI_TRB_ADDRESS_DEVICE, slot, &event))
        goto failed;
    if (!transfer_control(controller, controller_index, slot,
            0x80, 0x06, 0x0100, 0, descriptor, 8) ||
        descriptor[0] < 8 || descriptor[1] != 1)
        goto failed;
    if (speed != 4 &&
        !update_ep0_packet_size(controller, controller_index, slot,
                                descriptor[7]))
        goto failed;
    if (!transfer_control(controller, controller_index, slot,
            0x80, 0x06, 0x0200, 0, config_header, 9))
        goto failed;
    total = (uint16_t)config_header[2] |
        ((uint16_t)config_header[3] << 8);
    if (config_header[0] < 9 || config_header[1] != 2 ||
        total < 9 || total > sizeof(config) ||
        !transfer_control(controller, controller_index, slot,
            0x80, 0x06, 0x0200, 0, config, total))
        goto failed;
    for (offset = 0; offset + 2u <= total; )
    {
        uint8_t size = config[offset];
        uint8_t type = config[offset + 1u];
        if (size < 2 || offset + size > total)
            goto failed;
        if (type == 4 && size >= 9)
            interface_active = config[offset + 5] == 8 &&
                config[offset + 6] == 6 && config[offset + 7] == 0x50;
        else if (type == 5 && size >= 7 && interface_active &&
                 (config[offset + 3] & 3u) == 2u)
        {
            uint16_t packet = (uint16_t)config[offset + 4] |
                ((uint16_t)config[offset + 5] << 8);
            if (config[offset + 2] & 0x80u)
            {
                endpoint_in = config[offset + 2] & 0x0Fu;
                packet_in = packet;
                ep_in_offset = offset;
            }
            else
            {
                endpoint_out = config[offset + 2] & 0x0Fu;
                packet_out = packet;
                ep_out_offset = offset;
            }
        }
        offset = (uint16_t)(offset + size);
    }
    if (endpoint_in == 0 || endpoint_out == 0 ||
        packet_in == 0 || packet_out == 0 || config[5] == 0)
        goto failed;
    if ((speed == 3 && (packet_in != 512 || packet_out != 512)) ||
        (speed == 4 && (packet_in != 1024 || packet_out != 1024)) ||
        (speed == 2 &&
         ((packet_in != 8 && packet_in != 16 &&
           packet_in != 32 && packet_in != 64) ||
          (packet_out != 8 && packet_out != 16 &&
           packet_out != 32 && packet_out != 64))))
        goto failed;
    if (speed == 4)
    {
        uint16_t companion_in = (uint16_t)ep_in_offset + config[ep_in_offset];
        uint16_t companion_out = (uint16_t)ep_out_offset + config[ep_out_offset];
        if (ep_in_offset == 0 || ep_out_offset == 0 ||
            companion_in + 6u > total || companion_out + 6u > total ||
            config[companion_in] < 6 || config[companion_out] < 6 ||
            config[companion_in + 1] != 0x30 ||
            config[companion_out + 1] != 0x30 ||
            (config[companion_in + 3] & 0x1Fu) != 0 ||
            (config[companion_out + 3] & 0x1Fu) != 0)
            goto failed;
        burst_in = config[companion_in + 2];
        burst_out = config[companion_out + 2];
    }
    endpoint_in_id = (uint8_t)(endpoint_in * 2u + 1u);
    endpoint_out_id = (uint8_t)(endpoint_out * 2u);
    last_endpoint_id = endpoint_in_id > endpoint_out_id ?
        endpoint_in_id : endpoint_out_id;
    reset_transfer_ring(storage_in_rings[controller_index],
        &controller->storage_in_index, &controller->storage_in_cycle);
    reset_transfer_ring(storage_out_rings[controller_index],
        &controller->storage_out_index, &controller->storage_out_cycle);
    clear_input_context(controller_index);
    input_control = input_context(controller_index, 0);
    slot_context = input_context(controller_index, 1);
    in_context = input_context(controller_index,
        (uint8_t)(endpoint_in * 2u + 2u));
    out_context = input_context(controller_index,
        (uint8_t)(endpoint_out * 2u + 1u));
    set_input_context64(input_control, 1,
        1u | (1u << endpoint_in_id) | (1u << endpoint_out_id));
    slot_context[0] = ((uint32_t)speed << 20) |
        ((uint32_t)last_endpoint_id << 27);
    slot_context[1] = (uint32_t)(port + 1u) << 16;
    set_input_context64(in_context, 0,
        ((uint32_t)burst_in << 8) | (6u << 3));
    set_input_context64(in_context, 1, (uint32_t)packet_in << 16);
    set_input_context64(in_context, 2,
        (uint32_t)&storage_in_rings[controller_index][0] | 1u);
    set_input_context64(in_context, 4, packet_in);
    set_input_context64(out_context, 0,
        ((uint32_t)burst_out << 8) | (2u << 3));
    set_input_context64(out_context, 1, (uint32_t)packet_out << 16);
    set_input_context64(out_context, 2,
        (uint32_t)&storage_out_rings[controller_index][0] | 1u);
    set_input_context64(out_context, 4, packet_out);
    if (!command_ok(controller, controller_index,
            (uint32_t)&input_contexts[controller_index][0],
            XHCI_TRB_CONFIGURE_ENDPOINT, slot, &event) ||
        !transfer_control(controller, controller_index, slot,
            0, 0x09, config[5], 0, 0, 0))
        goto failed;
    controller->keyboard_slot = slot;
    controller->storage_port = (uint8_t)(port + 1u);
    controller->storage_endpoint_in = (uint8_t)(endpoint_in * 2u + 1u);
    controller->storage_endpoint_out = (uint8_t)(endpoint_out * 2u);
    controller->storage_packet_in = packet_in;
    controller->storage_packet_out = packet_out;
    controller->storage_burst_in = burst_in;
    controller->storage_burst_out = burst_out;
    controller->storage_tag = 0;
    controller->storage_present = 1;
    for (uint8_t i = 0; i < 16; i++)
        storage_cdb[controller_index][i] = 0;
    storage_cdb[controller_index][0] = 0x12;
    storage_cdb[controller_index][4] = 36;
    if (!xhci_bot_command(controller, controller_index, slot,
            storage_cdb[controller_index], 6, 1,
            storage_buffers[controller_index], 36))
        goto storage_failed;
    for (uint8_t i = 0; i < 16; i++)
        storage_cdb[controller_index][i] = 0;
    storage_cdb[controller_index][0] = 0x25;
    if (!xhci_bot_command(controller, controller_index, slot,
            storage_cdb[controller_index], 10, 1,
            storage_buffers[controller_index], 8) ||
        xhci_load_be32(storage_buffers[controller_index] + 4) != 512u ||
        xhci_load_be32(storage_buffers[controller_index]) == 0xFFFFFFFFu)
        goto storage_failed;
    controller->storage_sectors =
        (uint64_t)xhci_load_be32(storage_buffers[controller_index]) + 1u;
    return controller->storage_sectors != 0;

storage_failed:
    controller->storage_present = 0;
failed:
    command_ok(controller, controller_index, 0,
        XHCI_TRB_DISABLE_SLOT, slot, &event);
    dcbaa[controller_index][slot] = 0;
    return 0;
}

static int enumerate_xhci_keyboard(uint8_t controller_index, uint8_t port)
{
    XhciController* controller = &controllers[controller_index];
    XhciTrb event;
    uint8_t speed;
    uint8_t slot_id;
    uint8_t endpoint_size;
    uint8_t descriptor[18];
    uint8_t config_header[9];
    uint8_t config_descriptor[256];
    uint8_t interface_number = 0xFF;
    uint8_t endpoint = 0;
    uint16_t endpoint_offset = 0;
    uint8_t packet_size = 0;
    uint8_t interval = 1;
    uint16_t total_length;
    uint16_t offset;
    uint32_t* slot_context;
    uint32_t* endpoint0_context;
    uint32_t* endpoint_context;
    uint32_t* input_control;

    if (controller->keyboard_present)
        return 0;
    speed = (uint8_t)((mmio_read(controller->operational +
        XHCI_PORTSC_BASE + (uint32_t)port * 0x10u) >> 10) & 0x0Fu);
    if (speed == 0 || speed > 4)
        return 0;
    endpoint_size = speed == 4 ? 512 : speed == 3 ? 64 : 8;

    if (!command_ok(controller, controller_index, 0,
            XHCI_TRB_ENABLE_SLOT, 0, &event))
        return 0;
    slot_id = (uint8_t)(event.control >> 24);
    if (slot_id == 0 || slot_id > controller->slots)
        return 0;

    clear_input_context(controller_index);
    reset_transfer_ring(control_rings[controller_index],
        &controller->ep0_ring_index, &controller->ep0_ring_cycle);
    for (uint8_t i = 0; i < 32 * XHCI_MAX_CONTEXT_DWORDS; i++)
        output_contexts[controller_index][slot_id][i] = 0;
    dcbaa[controller_index][slot_id] =
        (uint32_t)&output_contexts[controller_index][slot_id][0];

    input_control = input_context(controller_index, 0);
    slot_context = input_context(controller_index, 1);
    endpoint0_context = input_context(controller_index, 2);
    set_input_context64(input_control, 1, 0x3u);
    set_input_context64(slot_context, 0,
        ((uint32_t)speed << 20) | (1u << 27));
    set_input_context64(slot_context, 1, (uint32_t)(port + 1u) << 16);
    set_input_context64(endpoint0_context, 0,
        (3u << 1) | (4u << 3));
    set_input_context64(endpoint0_context, 1,
        (uint32_t)endpoint_size << 16);
    set_input_context64(endpoint0_context, 2,
        (uint32_t)&control_rings[controller_index][0] | 1u);
    set_input_context64(endpoint0_context, 4, 8);
    __asm__ volatile ("" : : : "memory");
    if (!command_ok(controller, controller_index,
            (uint32_t)&input_contexts[controller_index][0],
            XHCI_TRB_ADDRESS_DEVICE, slot_id, &event))
    {
        command_ok(controller, controller_index, 0,
            XHCI_TRB_DISABLE_SLOT, slot_id, &event);
        dcbaa[controller_index][slot_id] = 0;
        return 0;
    }

    if (!transfer_control(controller, controller_index, slot_id,
            0x80, 0x06, 0x0100, 0, descriptor, 8) ||
        descriptor[0] < 8 || descriptor[1] != 1 ||
        (speed == 4 ? descriptor[7] != 9 :
         descriptor[7] != 8 && descriptor[7] != 16 &&
         descriptor[7] != 32 && descriptor[7] != 64) ||
        !update_ep0_packet_size(controller, controller_index,
            slot_id, speed == 4 ? 512 : descriptor[7]) ||
        !transfer_control(controller, controller_index, slot_id,
            0x80, 0x06, 0x0100, 0, descriptor, sizeof(descriptor)) ||
        !transfer_control(controller, controller_index, slot_id,
            0x80, 0x06, 0x0200, 0, config_header, sizeof(config_header)))
    {
        command_ok(controller, controller_index, 0,
            XHCI_TRB_DISABLE_SLOT, slot_id, &event);
        dcbaa[controller_index][slot_id] = 0;
        return 0;
    }

    total_length = (uint16_t)config_header[2] |
        ((uint16_t)config_header[3] << 8);
    if (config_header[0] < 9 || config_header[1] != 2 ||
        total_length < 9 || total_length > sizeof(config_descriptor) ||
        !transfer_control(controller, controller_index, slot_id,
            0x80, 0x06, 0x0200, 0, config_descriptor, total_length))
    {
        command_ok(controller, controller_index, 0,
            XHCI_TRB_DISABLE_SLOT, slot_id, &event);
        dcbaa[controller_index][slot_id] = 0;
        return 0;
    }

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
                max_packet == 8 &&
                (speed < 3 || (config_descriptor[offset + 6] >= 1 &&
                               config_descriptor[offset + 6] <= 16)))
            {
                endpoint = config_descriptor[offset + 2] & 0x0Fu;
                packet_size = 8;
                interval = config_descriptor[offset + 6];
                endpoint_offset = offset;
            }
        }
        offset = (uint16_t)(offset + size);
    }

    if (speed == 4 && endpoint != 0)
    {
        uint16_t companion = (uint16_t)endpoint_offset +
            config_descriptor[endpoint_offset];
        if (endpoint_offset == 0 ||
            companion + 6u > total_length ||
            config_descriptor[companion] < 6 ||
            config_descriptor[companion + 1] != 0x30 ||
            config_descriptor[companion + 2] != 0 ||
            (config_descriptor[companion + 3] & 0x03u) != 0 ||
            ((uint16_t)config_descriptor[companion + 4] |
             ((uint16_t)config_descriptor[companion + 5] << 8)) != 8)
        {
            endpoint = 0;
            packet_size = 0;
        }
    }

    if (endpoint == 0 || packet_size == 0 ||
        endpoint * 2u + 1u >= 32 || config_descriptor[5] == 0)
    {
        command_ok(controller, controller_index, 0,
            XHCI_TRB_DISABLE_SLOT, slot_id, &event);
        dcbaa[controller_index][slot_id] = 0;
        return 0;
    }

    clear_input_context(controller_index);
    input_control = input_context(controller_index, 0);
    slot_context = input_context(controller_index, 1);
    endpoint_context = input_context(
        controller_index, (uint8_t)(endpoint * 2u + 2u));
    set_input_context64(input_control, 1,
        1u | (1u << (endpoint * 2u + 2u)));
    slot_context[0] =
        ((uint32_t)speed << 20) |
        ((uint32_t)(endpoint * 2u + 1u) << 27);
    slot_context[1] = (uint32_t)(port + 1u) << 16;
    set_input_context64(endpoint_context, 0,
        ((uint32_t)xhci_endpoint_interval(speed, interval) << 16) |
        (3u << 1) | (7u << 3));
    set_input_context64(endpoint_context, 1,
        (uint32_t)packet_size << 16);
    set_input_context64(endpoint_context, 2,
        (uint32_t)&keyboard_rings[controller_index][0] | 1u);
    set_input_context64(endpoint_context, 4, packet_size);
    __asm__ volatile ("" : : : "memory");
    if (!command_ok(controller, controller_index,
            (uint32_t)&input_contexts[controller_index][0],
            XHCI_TRB_CONFIGURE_ENDPOINT, slot_id, &event) ||
        !transfer_control(controller, controller_index, slot_id,
            0x00, 0x09, config_descriptor[5], 0, 0, 0) ||
        !transfer_control(controller, controller_index, slot_id,
            0x21, 0x0B, 0, interface_number, 0, 0))
    {
        command_ok(controller, controller_index, 0,
            XHCI_TRB_DISABLE_SLOT, slot_id, &event);
        dcbaa[controller_index][slot_id] = 0;
        return 0;
    }

    reset_transfer_ring(keyboard_rings[controller_index],
        &controller->keyboard_ring_index, &controller->keyboard_ring_cycle);
    controller->keyboard_slot = slot_id;
    controller->keyboard_port = (uint8_t)(port + 1u);
    controller->keyboard_endpoint_id = (uint8_t)(endpoint * 2u + 1u);
    controller->keyboard_packet_size = packet_size;
    controller->keyboard_interval = interval;
    controller->keyboard_toggle = 0;
    controller->keyboard_present = 1;
    return 1;
}

static int xhci_keyboard_interrupt(
    XhciController* controller, uint8_t controller_index, uint8_t* report)
{
    XhciTrb event;
    XhciTrb* ring = keyboard_rings[controller_index];

    for (uint8_t i = 0; i < 8; i++)
        keyboard_reports[controller_index][i] = 0;
    push_transfer_trb(ring,
        &controller->keyboard_ring_index,
        &controller->keyboard_ring_cycle,
        (uint32_t)keyboard_reports[controller_index], 0,
        controller->keyboard_packet_size,
        XHCI_TRB_NORMAL | XHCI_TRB_IOC);
    __asm__ volatile ("" : : : "memory");
    mmio_write(controller->doorbells +
        (uint32_t)controller->keyboard_slot * 4u,
        controller->keyboard_endpoint_id);
    if (!wait_event(controller, controller_index,
            XHCI_TRB_TRANSFER_EVENT, controller->keyboard_slot,
            3000000, &event) ||
        ((event.status >> 24) & 0xFFu) != XHCI_COMPLETION_SUCCESS)
        return 0;
    for (uint8_t i = 0; i < controller->keyboard_packet_size && i < 8; i++)
        report[i] = keyboard_reports[controller_index][i];
    return 1;
}

static char xhci_usage_to_key(uint8_t usage, uint8_t shift)
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

int usb_xhci_keyboard_poll(char* key)
{
    uint8_t report[8] = { 0 };
    if (key == 0)
        return 0;
    for (uint8_t c = 0; c < controller_count; c++)
    {
        XhciController* controller = &controllers[c];
        if (!controller->keyboard_present ||
            !xhci_keyboard_interrupt(controller, c, report))
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
            translated = xhci_usage_to_key(report[i],
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

static int handoff_legacy(uint32_t base, uint32_t hccparams)
{
    uint32_t offset = ((hccparams >> 16) & 0xFFFFu) * 4u;
    uint32_t timeout = 1000000;
    uint8_t examined = 0;

    while (offset != 0 && examined++ < 64)
    {
        uint32_t capability = base + offset;
        uint32_t header = mmio_read(capability);
        uint8_t id = (uint8_t)header;
        uint8_t next = (uint8_t)(header >> 8);

        if (id == 1)
        {
            uint32_t sem = mmio_read(capability);
            mmio_write(capability, sem | 0x01000000u);
            while (timeout-- && (mmio_read(capability) & 0x00010000u))
                ;
            if (mmio_read(capability) & 0x00010000u)
                return 0;
            mmio_write(capability + 4, 0);
            return 1;
        }
        if (next == 0)
            break;
        offset += (uint32_t)next * 4u;
    }
    return 1;
}

int usb_xhci_start(uint8_t bus, uint8_t device, uint8_t function)
{
    XhciController* controller;
    uint32_t base;
    uint32_t cap_length;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hccparams1;
    uint32_t scratchpad_count;
    uint32_t command;
    uint32_t timeout;
    uint8_t index = controller_count;
    uint32_t runtime_offset;
    uint32_t doorbell_offset;

    if (controller_count >= XHCI_MAX_CONTROLLERS)
        return 0;
    base = usb_pci_enable_mmio(bus, device, function, 0x10);
    if (base == 0)
        return 0;

    cap_length = *(volatile uint8_t*)base;
    if (cap_length < 0x20u || cap_length > 0x80u)
        return 0;
    controller = &controllers[index];
    controller->base = base;
    controller->operational = base + cap_length;
    controller->present = 0;
    hcsparams1 = mmio_read(base + 0x04);
    hcsparams2 = mmio_read(base + 0x08);
    hccparams1 = mmio_read(base + 0x10);
    controller->slots = (uint8_t)(hcsparams1 & 0xFFu);
    controller->ports = (uint8_t)(hcsparams1 >> 24);
    scratchpad_count = (((hcsparams2 >> 21) & 0x1Fu) << 5) |
        ((hcsparams2 >> 16) & 0x1Fu);
    if (controller->slots == 0 || controller->ports == 0 ||
        scratchpad_count > XHCI_MAX_SCRATCHPADS)
        return 0;
    if (!handoff_legacy(base, hccparams1))
        return 0;

    command = mmio_read(controller->operational + XHCI_USBCMD);
    if (command & XHCI_CMD_RUN)
        mmio_write(controller->operational + XHCI_USBCMD,
                   command & ~XHCI_CMD_RUN);
    timeout = 1000000;
    while (timeout-- &&
           !(mmio_read(controller->operational + XHCI_USBSTS) &
             XHCI_STS_HALTED))
        ;
    if (!(mmio_read(controller->operational + XHCI_USBSTS) &
          XHCI_STS_HALTED))
        return 0;

    mmio_write(controller->operational + XHCI_USBCMD, XHCI_CMD_RESET);
    timeout = 1000000;
    while (timeout-- &&
           (mmio_read(controller->operational + XHCI_USBCMD) &
            XHCI_CMD_RESET))
        ;
    if (mmio_read(controller->operational + XHCI_USBCMD) &
        XHCI_CMD_RESET)
        return 0;
    timeout = 1000000;
    while (timeout-- &&
           (mmio_read(controller->operational + XHCI_USBSTS) &
            XHCI_STS_NOT_READY))
        ;
    if (mmio_read(controller->operational + XHCI_USBSTS) &
        XHCI_STS_NOT_READY)
        return 0;

    if (mmio_read(controller->operational + XHCI_PAGESIZE) & 1u)
        return 0;
    runtime_offset = mmio_read(base + 0x18) & 0xFFFFFFE0u;
    doorbell_offset = mmio_read(base + 0x14) & 0xFFFFFFFCu;
    if (runtime_offset == 0 || doorbell_offset == 0 ||
        base > 0xFFFFFFFFu - runtime_offset ||
        base > 0xFFFFFFFFu - doorbell_offset)
        return 0;
    controller->runtime = base + runtime_offset;
    controller->doorbells = base + doorbell_offset;
    controller->event_cycle = 1;
    controller->event_index = 0;
    controller->command_cycle = 1;
    controller->command_index = 0;
    controller->context_size = (hccparams1 & (1u << 2)) ? 64 : 32;
    controller->ep0_ring_index = 0;
    controller->ep0_ring_cycle = 1;

    for (uint16_t entry = 0; entry < XHCI_RING_SIZE; entry++)
    {
        command_rings[index][entry].parameter_low = 0;
        command_rings[index][entry].parameter_high = 0;
        command_rings[index][entry].status = 0;
        command_rings[index][entry].control = 0;
        event_rings[index][entry].parameter_low = 0;
        event_rings[index][entry].parameter_high = 0;
        event_rings[index][entry].status = 0;
        event_rings[index][entry].control = 0;
    }
    for (uint16_t entry = 0; entry < 256; entry++)
        dcbaa[index][entry] = 0;
    for (uint16_t entry = 0; entry < XHCI_MAX_SCRATCHPADS; entry++)
        scratchpad_array[index][entry] = 0;
    if (scratchpad_count != 0)
    {
        for (uint16_t entry = 0; entry < scratchpad_count; entry++)
            scratchpad_array[index][entry] =
                (uint32_t)&scratchpad_pages[index][entry][0];
        dcbaa[index][0] = (uint32_t)&scratchpad_array[index][0];
    }

    erst[index].ring_segment = (uint32_t)&event_rings[index][0];
    erst[index].ring_size = XHCI_RING_SIZE;
    command_rings[index][XHCI_RING_SIZE - 1].parameter_low =
        (uint32_t)&command_rings[index][0];
    command_rings[index][XHCI_RING_SIZE - 1].parameter_high = 0;
    command_rings[index][XHCI_RING_SIZE - 1].status = 0;
    command_rings[index][XHCI_RING_SIZE - 1].control =
        (6u << 10) | (1u << 1) | 1u;

    __asm__ volatile ("" : : : "memory");
    mmio_write(controller->operational + XHCI_DCBAAP,
               (uint32_t)&dcbaa[index][0]);
    mmio_write(controller->operational + XHCI_DCBAAP + 4, 0);
    mmio_write(controller->operational + XHCI_CRCR,
               (uint32_t)&command_rings[index][0] | 1u);
    mmio_write(controller->operational + XHCI_CRCR + 4, 0);
    mmio_write(controller->operational + XHCI_CONFIG, controller->slots);

    mmio_write(controller->runtime + 0x28, 1);
    mmio_write(controller->runtime + 0x30, (uint32_t)&erst[index]);
    mmio_write(controller->runtime + 0x34, 0);
    mmio_write(controller->runtime + 0x38,
               (uint32_t)&event_rings[index][0]);
    mmio_write(controller->runtime + 0x3C, 0);

    mmio_write(controller->operational + XHCI_USBCMD, XHCI_CMD_RUN);
    timeout = 1000000;
    while (timeout-- &&
           (mmio_read(controller->operational + XHCI_USBSTS) &
            XHCI_STS_HALTED))
        ;
    if (mmio_read(controller->operational + XHCI_USBSTS) &
        XHCI_STS_HALTED)
        return 0;

    controller->present = 1;
    controller_count++;
    for (uint16_t port = 0; port < controller->ports; port++)
    {
        uint32_t portsc = controller->operational +
            XHCI_PORTSC_BASE + (uint32_t)port * 0x10u;
        uint32_t status = mmio_read(portsc);
        status = (status & XHCI_PORTSC_RW_MASK) |
            (status & (XHCI_PORTSC_CONNECT_CHANGE |
                       XHCI_PORTSC_RESET_CHANGE |
                       XHCI_PORTSC_WARM_RESET_CHANGE)) |
            XHCI_PORTSC_POWER;
        mmio_write(portsc, status);
        status = mmio_read(portsc);
        controller->connected[port] =
            (status & XHCI_PORTSC_CONNECTED) != 0;
    }
    wait_for_port_power(controller);
    for (uint16_t port = 0; port < controller->ports; port++)
    {
        uint32_t portsc = controller->operational +
            XHCI_PORTSC_BASE + (uint32_t)port * 0x10u;
        uint32_t status = mmio_read(portsc);
        controller->connected[port] =
            (status & XHCI_PORTSC_CONNECTED) != 0;
        if (controller->connected[port])
        {
            if (reset_xhci_port(controller, (uint8_t)port))
            {
                if (!enumerate_xhci_keyboard(
                        (uint8_t)(controller_count - 1), (uint8_t)port))
                {
                    if (reset_xhci_port(controller, (uint8_t)port))
                        enumerate_xhci_storage(
                            (uint8_t)(controller_count - 1),
                            (uint8_t)port);
                }
            }
        }
    }
    return 1;
}

void usb_xhci_poll(void)
{
    uint8_t index;

    for (index = 0; index < controller_count; index++)
    {
        XhciController* controller = &controllers[index];
        uint16_t port;

        if (!controller->present)
            continue;
        drain_events(controller, index);
        for (port = 0; port < controller->ports; port++)
        {
            uint32_t portsc = controller->operational +
                XHCI_PORTSC_BASE + (uint32_t)port * 0x10u;
            uint32_t status = mmio_read(portsc);
            uint8_t connected =
                (status & XHCI_PORTSC_CONNECTED) != 0;

            if ((status & XHCI_PORTSC_CONNECT_CHANGE) ||
                (status & XHCI_PORTSC_RESET_CHANGE) ||
                (status & XHCI_PORTSC_WARM_RESET_CHANGE))
                mmio_write(portsc,
                    (status & XHCI_PORTSC_RW_MASK) |
                    (status & (XHCI_PORTSC_CONNECT_CHANGE |
                               XHCI_PORTSC_RESET_CHANGE |
                               XHCI_PORTSC_WARM_RESET_CHANGE)));
            if (!connected &&
                ((controller->keyboard_present &&
                  controller->keyboard_port == (uint8_t)(port + 1u)) ||
                 (controller->storage_present &&
                  controller->storage_port == (uint8_t)(port + 1u))))
            {
                release_xhci_slot(controller, index);
            }
            if (connected && !controller->connected[port])
            {
                if (reset_xhci_port(controller, (uint8_t)port))
                {
                    if (!enumerate_xhci_keyboard(index, (uint8_t)port))
                    {
                        if (reset_xhci_port(controller, (uint8_t)port))
                            enumerate_xhci_storage(index, (uint8_t)port);
                    }
                }
            }
            controller->connected[port] = connected;
        }
    }

}

int usb_xhci_storage_sector_count(uint8_t drive, uint64_t* sectors)
{
    uint8_t slot;
    uint8_t controller_index;
    uint8_t port;
    XhciController* controller;

    if (sectors == 0 || drive < XHCI_DRIVE_BASE ||
        drive >= XHCI_DRIVE_BASE + XHCI_DRIVE_COUNT)
        return 0;
    slot = (uint8_t)(drive - XHCI_DRIVE_BASE);
    controller_index = (uint8_t)(slot / XHCI_STORAGE_PORT_COUNT);
    port = (uint8_t)(slot % XHCI_STORAGE_PORT_COUNT);
    if (controller_index >= controller_count)
        return 0;
    controller = &controllers[controller_index];
    if (!controller->storage_present ||
        controller->storage_port != (uint8_t)(port + 1u))
        return 0;
    *sectors = controller->storage_sectors;
    return *sectors != 0;
}

int usb_xhci_storage_read(uint8_t drive, uint64_t lba, uint8_t* buffer)
{
    uint64_t sectors;
    uint8_t slot_id;
    uint8_t controller_index;
    uint8_t port;
    XhciController* controller;
    uint8_t* cdb;

    if (buffer == 0 || !usb_xhci_storage_sector_count(drive, &sectors) ||
        lba >= sectors || lba > 0xFFFFFFFFu)
        return 0;
    controller_index = (uint8_t)((drive - XHCI_DRIVE_BASE) /
        XHCI_STORAGE_PORT_COUNT);
    port = (uint8_t)((drive - XHCI_DRIVE_BASE) %
        XHCI_STORAGE_PORT_COUNT);
    controller = &controllers[controller_index];
    if (controller->storage_port != (uint8_t)(port + 1u))
        return 0;
    slot_id = controller->keyboard_slot;
    cdb = storage_cdb[controller_index];
    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x28;
    xhci_store_be32(cdb + 2, (uint32_t)lba);
    cdb[8] = 1;
    if (!xhci_bot_command(controller, controller_index, slot_id,
            cdb, 10, 1, storage_buffers[controller_index], 512))
        return 0;
    for (uint16_t i = 0; i < 512; i++)
        buffer[i] = storage_buffers[controller_index][i];
    return 1;
}

int usb_xhci_storage_write(
    uint8_t drive, uint64_t lba, const uint8_t* buffer)
{
    uint64_t sectors;
    uint8_t controller_index;
    XhciController* controller;
    uint8_t* cdb;

    if (buffer == 0 || !usb_xhci_storage_sector_count(drive, &sectors) ||
        lba >= sectors || lba > 0xFFFFFFFFu)
        return 0;
    controller_index = (uint8_t)((drive - XHCI_DRIVE_BASE) /
        XHCI_STORAGE_PORT_COUNT);
    controller = &controllers[controller_index];
    for (uint16_t i = 0; i < 512; i++)
        storage_buffers[controller_index][i] = buffer[i];
    cdb = storage_cdb[controller_index];
    for (uint8_t i = 0; i < 16; i++)
        cdb[i] = 0;
    cdb[0] = 0x2A;
    xhci_store_be32(cdb + 2, (uint32_t)lba);
    cdb[8] = 1;
    return xhci_bot_command(controller, controller_index,
        controller->keyboard_slot, cdb, 10, 0,
        storage_buffers[controller_index], 512);
}
