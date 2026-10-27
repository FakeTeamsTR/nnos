#include "types.h"
#include "ahci.h"

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA    0x0CFC
#define PCI_COMMAND        0x04
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_MASTER 0x0004
#define PCI_CLASS_REVISION 0x08
#define PCI_BAR5           0x24

#define AHCI_CLASS         0x01
#define AHCI_SUBCLASS      0x06
#define AHCI_PROG_IF       0x01
#define AHCI_GHC_AE        (1u << 31)
#define AHCI_CAP2_BOH      (1u << 0)
#define AHCI_BOHC_BOS      (1u << 0)
#define AHCI_BOHC_OOS      (1u << 1)
#define AHCI_BOHC_BB       (1u << 4)
#define AHCI_PORTS         AHCI_PORT_COUNT
#define AHCI_PORT_BASE     0x100
#define AHCI_PORT_SIZE     0x80
#define AHCI_PORT_ST       (1u << 0)
#define AHCI_PORT_FRE      (1u << 4)
#define AHCI_PORT_FR       (1u << 14)
#define AHCI_PORT_CR       (1u << 15)
#define AHCI_PORT_TFES     (1u << 30)
#define AHCI_SIG_ATA       0x00000101u
#define AHCI_SSTS_DET_MASK 0x0Fu
#define AHCI_SSTS_DET_PHY  0x03u
#define AHCI_CMD_IDENTIFY  0xEC
#define AHCI_CMD_READ_DMA  0xC8
#define AHCI_CMD_WRITE_DMA 0xCA
#define AHCI_CMD_READ_EXT  0x25
#define AHCI_CMD_WRITE_EXT 0x35
#define AHCI_CMD_FLUSH     0xE7
#define AHCI_CMD_FLUSH_EXT 0xEA
#define AHCI_COMMAND_TIMEOUT 10000000u

typedef struct __attribute__((packed))
{
    uint8_t command_fis[64];
    uint8_t atapi_command[16];
    uint8_t reserved[48];
    uint32_t data_base;
    uint32_t data_base_upper;
    uint32_t reserved_word;
    uint32_t byte_count;
} AhciCommandTable;

typedef struct __attribute__((packed))
{
    uint8_t command_fis_length;
    uint8_t flags;
    uint16_t prdt_length;
    uint32_t bytes_transferred;
    uint32_t command_table_base;
    uint32_t command_table_base_upper;
    uint32_t reserved[4];
} AhciCommandHeader;

typedef char ahci_command_header_must_be_32[
    sizeof(AhciCommandHeader) == 32 ? 1 : -1
];

typedef struct
{
    volatile uint32_t* registers;
    uint64_t sector_count;
    uint8_t present;
    uint8_t lba48;
} AhciPort;

static AhciPort ports[AHCI_PORTS];
static uint8_t initialized;
static uint8_t controller_found;
static volatile uint32_t* hba_registers;
static uint32_t command_list[AHCI_PORTS][256]
    __attribute__((aligned(1024)));
static uint8_t received_fis[AHCI_PORTS][256]
    __attribute__((aligned(256)));
static uint8_t command_tables[AHCI_PORTS][256]
    __attribute__((aligned(128)));
static uint8_t dma_buffers[AHCI_PORTS][512]
    __attribute__((aligned(2)));

static uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
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
    return inb(PCI_CONFIG_DATA) |
        ((uint32_t)inb(PCI_CONFIG_DATA + 1) << 8) |
        ((uint32_t)inb(PCI_CONFIG_DATA + 2) << 16) |
        ((uint32_t)inb(PCI_CONFIG_DATA + 3) << 24);
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
    __asm__ volatile ("outw %0, %1" : : "a"(value),
                      "Nd"((uint16_t)(PCI_CONFIG_DATA + (offset & 2u))));
}

static int wait_mask(
    volatile uint32_t* reg,
    uint32_t mask,
    uint32_t expected,
    uint32_t timeout)
{
    while (timeout--)
    {
        if ((*reg & mask) == expected)
            return 1;
    }
    return 0;
}

static int stop_port(volatile uint32_t* port)
{
    port[0x18 / 4] &= ~AHCI_PORT_ST;
    if (!wait_mask(
            &port[0x18 / 4], AHCI_PORT_CR, 0, AHCI_COMMAND_TIMEOUT))
        return 0;
    port[0x18 / 4] &= ~AHCI_PORT_FRE;
    return wait_mask(
        &port[0x18 / 4], AHCI_PORT_FR, 0, AHCI_COMMAND_TIMEOUT);
}

static int issue_command(
    uint8_t port_index,
    uint8_t command,
    uint64_t lba,
    uint8_t* buffer)
{
    AhciPort* port = &ports[port_index];
    AhciCommandHeader* header;
    AhciCommandTable* table;
    volatile uint32_t* registers = port->registers;
    uint8_t* fis;
    uint32_t timeout;

    if (!wait_mask(
            &registers[0x20 / 4], 0x88u, 0, AHCI_COMMAND_TIMEOUT))
        return 0;

    header = (AhciCommandHeader*)command_list[port_index];
    table = (AhciCommandTable*)command_tables[port_index];
    header[0].command_fis_length = 5;
    header[0].flags =
        command == AHCI_CMD_WRITE_DMA || command == AHCI_CMD_WRITE_EXT ?
        (1u << 6) : 0;
    header[0].prdt_length = buffer ? 1 : 0;
    header[0].bytes_transferred = 0;
    header[0].command_table_base = (uint32_t)table;
    header[0].command_table_base_upper = 0;

    fis = table->command_fis;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = command;
    fis[3] = 0;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 1u << 6;
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[11] = 0;
    fis[12] = 1;
    fis[13] = 0;
    fis[14] = 0;
    fis[15] = 0;

    table->data_base = buffer ? (uint32_t)buffer : 0;
    table->data_base_upper = 0;
    table->reserved_word = 0;
    table->byte_count = buffer ? (511u | (1u << 31)) : 0;

    registers[0x30 / 4] = 0xFFFFFFFFu;
    registers[0x10 / 4] = 0xFFFFFFFFu;
    __asm__ volatile ("" : : : "memory");
    registers[0x38 / 4] = 1;

    timeout = AHCI_COMMAND_TIMEOUT;
    while (timeout--)
    {
        uint32_t status = registers[0x10 / 4];
        if ((status & AHCI_PORT_TFES) != 0)
            return 0;
        if ((registers[0x38 / 4] & 1u) == 0)
        {
            if (registers[0x20 / 4] & 1u)
                return 0;
            __asm__ volatile ("" : : : "memory");
            return 1;
        }
    }

    return 0;
}

static int init_port(uint8_t port_index, uint32_t port_map)
{
    volatile uint32_t* port;
    uint8_t* identify;
    uint64_t sectors;
    uint32_t signature;
    uint64_t sectors48;

    if (!(port_map & (1u << port_index)))
        return 0;

    port = (volatile uint32_t*)((volatile uint8_t*)hba_registers +
        AHCI_PORT_BASE + (uint32_t)port_index * AHCI_PORT_SIZE);
    if ((port[0x28 / 4] & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PHY)
        return 0;

    signature = port[0x24 / 4];
    if (signature != AHCI_SIG_ATA)
        return 0;

    if (!stop_port(port))
        return 0;
    ports[port_index].registers = port;
    port[0] = (uint32_t)command_list[port_index];
    port[1] = 0;
    port[2] = (uint32_t)received_fis[port_index];
    port[3] = 0;
    port[0x30 / 4] = 0xFFFFFFFFu;
    port[0x10 / 4] = 0xFFFFFFFFu;
    port[0x18 / 4] |= AHCI_PORT_FRE;
    port[0x18 / 4] |= AHCI_PORT_ST;

    identify = dma_buffers[port_index];
    if (!issue_command(port_index, AHCI_CMD_IDENTIFY, 0, identify))
        return 0;

    if (!(identify[99] & 0x02))
        return 0;
    if (((uint16_t)identify[212] |
         ((uint16_t)identify[213] << 8)) & 0x1000u)
    {
        uint16_t sector_info = (uint16_t)identify[212] |
                               ((uint16_t)identify[213] << 8);
        if ((sector_info & 0xC000u) == 0x4000u)
            return 0;
    }

    sectors48 = (uint64_t)identify[200] |
        ((uint64_t)identify[201] << 8) |
        ((uint64_t)identify[202] << 16) |
        ((uint64_t)identify[203] << 24) |
        ((uint64_t)identify[204] << 32) |
        ((uint64_t)identify[205] << 40) |
        ((uint64_t)identify[206] << 48) |
        ((uint64_t)identify[207] << 56);
    sectors48 &= 0x0000FFFFFFFFFFFFull;

    ports[port_index].lba48 =
        (identify[167] & 0xC4u) == 0x44u;
    sectors = ports[port_index].lba48 ?
        sectors48 : 0;

    if (!ports[port_index].lba48)
        sectors = (uint32_t)identify[120] |
                  ((uint32_t)identify[121] << 8) |
                  ((uint32_t)identify[122] << 16) |
                  ((uint32_t)identify[123] << 24);

    if (sectors == 0)
        return 0;

    if (sectors > (1ull << 48))
        return 0;

    ports[port_index].registers = port;
    ports[port_index].sector_count = sectors;
    ports[port_index].present = 1;
    return 1;
}

static int init_controller(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t class_revision)
{
    uint32_t command;
    uint32_t bar5;
    uint32_t bar_high;
    uint32_t capabilities;
    uint32_t port_map;
    uint8_t i;
    uint32_t found = 0;

    if (((class_revision >> 24) & 0xFF) != AHCI_CLASS ||
        ((class_revision >> 16) & 0xFF) != AHCI_SUBCLASS ||
        ((class_revision >> 8) & 0xFF) != AHCI_PROG_IF)
        return 0;

    bar5 = pci_read32(bus, device, function, PCI_BAR5);
    if ((bar5 & 1u) || ((bar5 >> 1) & 3u) == 1u ||
        ((bar5 >> 1) & 3u) == 3u)
        return 0;
    if (((bar5 >> 1) & 3u) == 2)
    {
        bar_high = pci_read32(bus, device, function, PCI_BAR5 + 4);
        if (bar_high != 0)
            return 0;
    }

    bar5 &= ~0x0Fu;
    if (bar5 == 0 || bar5 > 0xFFFFEFFFu)
        return 0;

    command = pci_read32(bus, device, function, PCI_COMMAND);
    pci_write16(
        bus, device, function, PCI_COMMAND,
        (uint16_t)((command & 0xFFFFu) |
                   PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));

    hba_registers = (volatile uint32_t*)bar5;
    capabilities = hba_registers[0x24 / 4];
    if (capabilities & AHCI_CAP2_BOH)
    {
        uint32_t timeout = AHCI_COMMAND_TIMEOUT;
        volatile uint32_t* handoff = &hba_registers[0x28 / 4];
        *handoff |= AHCI_BOHC_OOS;
        while (timeout-- &&
               (*handoff & (AHCI_BOHC_BOS | AHCI_BOHC_BB)))
            ;
        if (*handoff & (AHCI_BOHC_BOS | AHCI_BOHC_BB))
            return 0;
    }

    hba_registers[1] |= AHCI_GHC_AE;
    hba_registers[1] &= ~(1u << 1);
    port_map = hba_registers[3];
    for (i = 0; i < AHCI_PORTS; i++)
        found += init_port(i, port_map) ? 1u : 0u;

    return found != 0;
}

static void initialize(void)
{
    uint16_t bus;
    uint8_t device;
    uint8_t function;

    if (initialized)
        return;
    initialized = 1;

    for (bus = 0; bus < 256 && !controller_found; bus++)
    {
        for (device = 0; device < 32 && !controller_found; device++)
        {
            uint32_t vendor0 = pci_read32((uint8_t)bus, device, 0, 0);
            uint32_t header;
            uint8_t function_count =
                1;

            if ((vendor0 & 0xFFFFu) == 0xFFFFu)
                continue;
            header = pci_read32((uint8_t)bus, device, 0, 0x0C);
            if (header & (1u << 23))
                function_count = 8;

            for (function = 0;
                 function < function_count && !controller_found;
                 function++)
            {
                uint32_t vendor =
                    pci_read32((uint8_t)bus, device, function, 0);
                uint32_t class_revision;

                if ((vendor & 0xFFFFu) == 0xFFFFu)
                    continue;

                class_revision = pci_read32(
                    (uint8_t)bus, device, function, PCI_CLASS_REVISION);
                if (init_controller(
                        (uint8_t)bus, device, function, class_revision))
                    controller_found = 1;
            }
        }
    }
}

int ahci_get_sector_count(uint8_t drive, uint64_t* sector_count)
{
    uint8_t port;

    if (drive < AHCI_DRIVE_BASE ||
        drive >= AHCI_DRIVE_BASE + AHCI_PORTS || sector_count == 0)
        return 0;

    initialize();
    port = (uint8_t)(drive - AHCI_DRIVE_BASE);
    if (!controller_found || !ports[port].present)
        return 0;

    *sector_count = ports[port].sector_count;
    return 1;
}

static int transfer(uint8_t drive, uint64_t lba, uint8_t* buffer, int write)
{
    uint8_t port;
    uint8_t* bounce;
    uint16_t i;

    if (buffer == 0 || drive < AHCI_DRIVE_BASE ||
        drive >= AHCI_DRIVE_BASE + AHCI_PORTS)
        return 0;

    port = (uint8_t)(drive - AHCI_DRIVE_BASE);
    if (!ahci_get_sector_count(drive, &ports[port].sector_count) ||
        lba >= ports[port].sector_count ||
        (!ports[port].lba48 && lba > 0x0FFFFFFFu))
        return 0;

    bounce = dma_buffers[port];
    if (write)
    {
        for (i = 0; i < 512; i++)
            bounce[i] = buffer[i];
    }

    if (!issue_command(
            port,
            ports[port].lba48 ?
                (write ? AHCI_CMD_WRITE_EXT : AHCI_CMD_READ_EXT) :
                (write ? AHCI_CMD_WRITE_DMA : AHCI_CMD_READ_DMA),
            lba,
            bounce))
        return 0;

    if (!write)
    {
        for (i = 0; i < 512; i++)
            buffer[i] = bounce[i];
    }
    else if (!issue_command(
                 port,
                 ports[port].lba48 ? AHCI_CMD_FLUSH_EXT : AHCI_CMD_FLUSH,
                 0,
                 0))
        return 0;

    return 1;
}

int ahci_read_sector(uint8_t drive, uint64_t lba, uint8_t* buffer)
{
    return transfer(drive, lba, buffer, 0);
}

int ahci_write_sector(uint8_t drive, uint64_t lba, const uint8_t* buffer)
{
    return transfer(drive, lba, (uint8_t*)buffer, 1);
}
