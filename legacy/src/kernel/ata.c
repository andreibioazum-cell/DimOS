/*
 * ata.c -- the disk.
 *
 * DimOS used to see the disk only through the bootloader: the BIOS read the
 * first files into low memory and the kernel could do nothing but look at
 * them. That is not an operating system, so this file talks to the IDE
 * controller itself. It issues the same commands Linux does -- IDENTIFY,
 * READ SECTORS, WRITE SECTORS, FLUSH CACHE -- over plain programmed I/O,
 * which needs no DMA engine, no interrupts and no driver framework: just
 * five ports and a poll loop.
 *
 * Only the primary channel, master drive, is used, and only LBA28
 * addressing (disks up to 128 GiB). Every other disk a PC may have is
 * simply not looked at; the first one that answers is the volume DimOS
 * stores its files on.
 */

#include "dimos.h"

#define ATA_PRIMARY_DATA 0x01F0u
#define ATA_PRIMARY_ERROR 0x01F1u
#define ATA_PRIMARY_COUNT 0x01F2u
#define ATA_PRIMARY_LBA_LOW 0x01F3u
#define ATA_PRIMARY_LBA_MID 0x01F4u
#define ATA_PRIMARY_LBA_HIGH 0x01F5u
#define ATA_PRIMARY_DRIVE 0x01F6u
#define ATA_PRIMARY_COMMAND 0x01F7u
#define ATA_PRIMARY_STATUS 0x01F7u
#define ATA_PRIMARY_CONTROL 0x03F6u
#define ATA_ALTERNATE_STATUS 0x03F6u

#define ATA_STATUS_ERROR 0x01u
#define ATA_STATUS_DRIVE_READY 0x40u
#define ATA_STATUS_BUSY 0x80u

#define ATA_COMMAND_READ 0x20u
#define ATA_COMMAND_WRITE 0x30u
#define ATA_COMMAND_FLUSH 0xE7u
#define ATA_COMMAND_IDENTIFY 0xECu

#define ATA_POLL_LIMIT 400000u

static u8 disk_present;
static u8 disk_is_slave;
static u32 disk_sectors;
static char disk_model[41];
static u16 block[256];

/* Four reads of the alternate status port is the traditional 400 ns wait
 * after a drive-select, and it is all the delay an IDE bus needs. */
static void bus_delay(void) {
    u8 index;

    for (index = 0u; index < 4u; ++index) {
        (void)port_read_byte(ATA_ALTERNATE_STATUS);
    }
}

static u8 wait_ready(void) {
    u32 guard;

    for (guard = 0u; guard < ATA_POLL_LIMIT; ++guard) {
        const u8 status = port_read_byte(ATA_PRIMARY_STATUS);

        if ((status & ATA_STATUS_BUSY) == 0u) {
            if ((status & ATA_STATUS_ERROR) != 0u) {
                return 0u;
            }
            return 1u;
        }
    }
    return 0u;
}

static u8 wait_for_data(void) {
    u32 guard;

    for (guard = 0u; guard < ATA_POLL_LIMIT; ++guard) {
        const u8 status = port_read_byte(ATA_PRIMARY_STATUS);

        if ((status & ATA_STATUS_BUSY) != 0u) {
            continue;
        }
        if ((status & ATA_STATUS_ERROR) != 0u) {
            return 0u;
        }
        if ((status & 0x08u) != 0u) { /* DRQ: a sector is waiting */
            return 1u;
        }
    }
    return 0u;
}

static void select_drive(u8 slave) {
    port_write_byte(ATA_PRIMARY_DRIVE,
                    (u8)(0xE0u | (u8)(slave != 0u ? 0x10u : 0x00u)));
    bus_delay();
}

static void identify_string(char *out, u16 first_word) {
    u16 index;

    for (index = 0u; index < 20u; ++index) {
        const u16 word = block[first_word + index];

        out[index * 2u] = (char)(word >> 8u);
        out[index * 2u + 1u] = (char)(word & 0xFFu);
    }
    out[40] = '\0';

    /* The model field is space padded; trim both ends. */
    index = 40u;
    while (index > 0u && out[index - 1u] == ' ') {
        out[--index] = '\0';
    }
}

static u8 identify(u8 slave) {
    u16 index;

    select_drive(slave);
    port_write_byte(ATA_PRIMARY_COUNT, 0u);
    port_write_byte(ATA_PRIMARY_LBA_LOW, 0u);
    port_write_byte(ATA_PRIMARY_LBA_MID, 0u);
    port_write_byte(ATA_PRIMARY_LBA_HIGH, 0u);
    port_write_byte(ATA_PRIMARY_COMMAND, ATA_COMMAND_IDENTIFY);

    /* A floating bus reads back as zero on a machine with no drive. */
    if (port_read_byte(ATA_PRIMARY_STATUS) == 0u) {
        return 0u;
    }
    if (wait_ready() == 0u || wait_for_data() == 0u) {
        return 0u;
    }
    for (index = 0u; index < 256u; ++index) {
        block[index] = port_read_word(ATA_PRIMARY_DATA);
    }

    disk_sectors = (u32)block[60] | ((u32)block[61] << 16u);
    if (disk_sectors == 0u) {
        return 0u;
    }
    identify_string(disk_model, 27u);
    if (disk_model[0] == '\0') {
        text_copy(disk_model, "IDE disk", 41u);
    }
    return 1u;
}

u8 ata_init(void) {
    disk_present = 0u;
    disk_is_slave = 0u;
    disk_sectors = 0u;
    disk_model[0] = '\0';

    if (identify(0u) != 0u) {
        disk_present = 1u;
        return 1u;
    }
    if (identify(1u) != 0u) {
        disk_present = 1u;
        disk_is_slave = 1u;
        return 1u;
    }
    return 0u;
}

u8 ata_present(void) {
    return disk_present;
}

u32 ata_sectors(void) {
    return disk_sectors;
}

const char *ata_model(void) {
    return disk_model;
}

u8 ata_read(u32 lba, void *buffer, u16 count) {
    u16 done;
    u8 *target = (u8 *)buffer;

    if (disk_present == 0u || count == 0u) {
        return 0u;
    }
    if (lba >= disk_sectors || (u32)count > (disk_sectors - lba)) {
        return 0u;
    }

    for (done = 0u; done < count; ++done) {
        const u32 current = lba + done;
        u16 word;

        if (wait_ready() == 0u) {
            return 0u;
        }
        select_drive(disk_is_slave);
        port_write_byte(ATA_PRIMARY_COUNT, 1u);
        port_write_byte(ATA_PRIMARY_LBA_LOW, (u8)(current & 0xFFu));
        port_write_byte(ATA_PRIMARY_LBA_MID, (u8)((current >> 8u) & 0xFFu));
        port_write_byte(ATA_PRIMARY_LBA_HIGH, (u8)((current >> 16u) & 0xFFu));
        port_write_byte(ATA_PRIMARY_DRIVE,
                        (u8)(0xE0u | (u8)(disk_is_slave != 0u ? 0x10u : 0x00u) |
                             (u8)((current >> 24u) & 0x0Fu)));
        port_write_byte(ATA_PRIMARY_COMMAND, ATA_COMMAND_READ);

        if (wait_for_data() == 0u) {
            return 0u;
        }
        for (word = 0u; word < 256u; ++word) {
            const u16 value = port_read_word(ATA_PRIMARY_DATA);

            target[word * 2u] = (u8)(value & 0xFFu);
            target[word * 2u + 1u] = (u8)(value >> 8u);
        }
        target += 512u;
    }
    return 1u;
}

u8 ata_write(u32 lba, const void *buffer, u16 count) {
    u16 done;
    const u8 *source = (const u8 *)buffer;

    if (disk_present == 0u || count == 0u) {
        return 0u;
    }
    if (lba >= disk_sectors || (u32)count > (disk_sectors - lba)) {
        return 0u;
    }

    for (done = 0u; done < count; ++done) {
        const u32 current = lba + done;
        u16 word;

        if (wait_ready() == 0u) {
            return 0u;
        }
        select_drive(disk_is_slave);
        port_write_byte(ATA_PRIMARY_COUNT, 1u);
        port_write_byte(ATA_PRIMARY_LBA_LOW, (u8)(current & 0xFFu));
        port_write_byte(ATA_PRIMARY_LBA_MID, (u8)((current >> 8u) & 0xFFu));
        port_write_byte(ATA_PRIMARY_LBA_HIGH, (u8)((current >> 16u) & 0xFFu));
        port_write_byte(ATA_PRIMARY_DRIVE,
                        (u8)(0xE0u | (u8)(disk_is_slave != 0u ? 0x10u : 0x00u) |
                             (u8)((current >> 24u) & 0x0Fu)));
        port_write_byte(ATA_PRIMARY_COMMAND, ATA_COMMAND_WRITE);

        if (wait_for_data() == 0u) {
            return 0u;
        }
        for (word = 0u; word < 256u; ++word) {
            const u16 value = (u16)((u16)source[word * 2u] |
                                    (u16)((u16)source[word * 2u + 1u] << 8u));

            port_write_word(ATA_PRIMARY_DATA, value);
        }
        source += 512u;

        if (wait_ready() == 0u) {
            return 0u;
        }
    }
    return 1u;
}

/* Tell the drive to empty its own write cache, so the next boot really sees
 * the bytes. Cheap and harmless on drives that do not cache. */
u8 ata_flush(void) {
    if (disk_present == 0u) {
        return 0u;
    }
    port_write_byte(ATA_PRIMARY_COMMAND, ATA_COMMAND_FLUSH);
    return wait_ready();
}
