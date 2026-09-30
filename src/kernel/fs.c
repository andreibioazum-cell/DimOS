/*
 * fs.c -- storage.
 *
 * Two things live here:
 *
 * 1. A RAM disk. Four megabytes of memory at 0x500000 that behave like a
 *    disk: reads and writes are plain memory copies with a bounds check, so
 *    there is no port I/O and no BIOS call anywhere near them.
 *
 * 2. The FAT12 boot volume. The bootloader cannot reach above one megabyte
 *    with BIOS disk services, so it preloads the interesting parts of the
 *    volume into low memory before handing over:
 *
 *       0x07C00  boot sector          (1 sector)
 *       0x07E00  both FAT copies     (18 sectors)
 *       0x10000  root directory      (14 sectors, 224 entries)
 *       0x30000  first 256 data sectors (128 KiB, ends at 0x50000)
 *
 *    The file manager reads those windows. Files are shown and read from the
 *    copy in memory, and deleting one only hides it until the next boot, so
 *    DimOS can never damage the disk it started from.
 */

#include "dimos.h"

/* ------------------------------------------------------------------ */
/* RAM disk                                                            */
/* ------------------------------------------------------------------ */

#define RAM_DISK_ADDRESS 0x00500000ull
#define RAM_DISK_SECTOR_BYTES 512u
#define RAM_DISK_SECTORS 8192u /* 8192 * 512 = 4 MiB */

void ram_disk_init(void) {
    memory_zero((void *)RAM_DISK_ADDRESS, RAM_DISK_SECTORS * RAM_DISK_SECTOR_BYTES);
}

u32 ram_disk_sectors(void) {
    return RAM_DISK_SECTORS;
}

u8 ram_disk_read(u32 sector, void *buffer, u32 sector_count) {
    if (sector_count == 0u || sector >= RAM_DISK_SECTORS ||
        sector_count > (RAM_DISK_SECTORS - sector)) {
        return 0u;
    }
    memory_copy(buffer, (const void *)(RAM_DISK_ADDRESS + sector * RAM_DISK_SECTOR_BYTES),
                sector_count * RAM_DISK_SECTOR_BYTES);
    return 1u;
}

u8 ram_disk_write(u32 sector, const void *buffer, u32 sector_count) {
    if (sector_count == 0u || sector >= RAM_DISK_SECTORS ||
        sector_count > (RAM_DISK_SECTORS - sector)) {
        return 0u;
    }
    memory_copy((void *)(RAM_DISK_ADDRESS + sector * RAM_DISK_SECTOR_BYTES), buffer,
                sector_count * RAM_DISK_SECTOR_BYTES);
    return 1u;
}

/* ------------------------------------------------------------------ */
/* The FAT12 boot volume                                               */
/* ------------------------------------------------------------------ */

#define BOOT_SECTOR_WINDOW ((const u8 *)0x00007C00u)
#define FAT_WINDOW ((const u8 *)0x00007E00u)
#define ROOT_WINDOW ((const u8 *)0x00010000u)
#define DATA_WINDOW ((const u8 *)0x00030000u)

/* Offsets inside a 32 byte directory entry. */
#define ENTRY_NAME 0u
#define ENTRY_ATTRIBUTES 11u
#define ENTRY_FIRST_CLUSTER 26u
#define ENTRY_SIZE 28u

#define ATTRIBUTE_VOLUME_LABEL 0x08u
#define ATTRIBUTE_LONG_NAME 0x0Fu
#define ENTRY_FREE 0xE5u
#define ENTRY_LAST 0x00u

/* How much of the data area the bootloader preloads at 0x30000. The
 * window ends exactly at SCRATCH_ADDRESS (0x50000): 256 sectors. It
 * must match DATA_PRELOAD in src/bootloader/boot.asm. */
#define DATA_WINDOW_SECTORS 256u
#define CLUSTER_END 0xFF8u

static u8 hidden_entry[FILE_ENTRY_COUNT];
static u16 visible_slot[FILE_ENTRY_COUNT];
static u16 visible_count;

void file_system_init(void) {
    u16 index;

    for (index = 0u; index < FILE_ENTRY_COUNT; ++index) {
        hidden_entry[index] = 0u;
    }

    /* Remember which directory entries describe a file the user can see. */
    visible_count = 0u;
    for (index = 0u; index < FILE_ENTRY_COUNT; ++index) {
        const u8 *entry = ROOT_WINDOW + (u32)index * 32u;

        if (entry[ENTRY_NAME] == ENTRY_LAST) {
            break; /* no more entries after this one */
        }
        if (entry[ENTRY_NAME] == ENTRY_FREE) {
            continue;
        }
        if ((entry[ENTRY_ATTRIBUTES] & ATTRIBUTE_VOLUME_LABEL) != 0u ||
            entry[ENTRY_ATTRIBUTES] == ATTRIBUTE_LONG_NAME) {
            continue; /* volume label or a long file name helper entry */
        }
        visible_slot[visible_count] = index;
        ++visible_count;
    }
}

static const u8 *directory_entry(u16 index) {
    return ROOT_WINDOW + (u32)index * 32u;
}

static u16 entry_visible(u16 index) {
    u16 slot;

    if (index >= FILE_ENTRY_COUNT || hidden_entry[index] != 0u) {
        return 0u;
    }
    for (slot = 0u; slot < visible_count; ++slot) {
        if (visible_slot[slot] == index) {
            return 1u;
        }
    }
    return 0u;
}

u16 file_system_visible_count(void) {
    u16 count = 0u;
    u16 slot;

    for (slot = 0u; slot < visible_count; ++slot) {
        if (hidden_entry[visible_slot[slot]] == 0u) {
            ++count;
        }
    }
    return count;
}

u16 file_system_visible(u16 slot) {
    u16 seen = 0u;
    u16 index;

    for (index = 0u; index < visible_count; ++index) {
        if (hidden_entry[visible_slot[index]] != 0u) {
            continue;
        }
        if (seen == slot) {
            return visible_slot[index];
        }
        ++seen;
    }
    return FILE_NOT_FOUND;
}

/* "KERNEL  BIN" becomes "KERNEL.BIN" in out. */
void file_system_name(u16 index, char *out) {
    const u8 *entry = directory_entry(index);
    u16 position = 0u;
    u8 column;

    out[0] = '\0';
    for (column = 0u; column < 8u && entry[column] != ' '; ++column) {
        out[position++] = (char)entry[column];
    }
    if (entry[8] != ' ') {
        out[position++] = '.';
        for (column = 8u; column < 11u && entry[column] != ' '; ++column) {
            out[position++] = (char)entry[column];
        }
    }
    out[position] = '\0';
}

u32 file_system_size(u16 index) {
    const u8 *entry = directory_entry(index);

    return (u32)entry[ENTRY_SIZE] |
           ((u32)entry[ENTRY_SIZE + 1u] << 8u) |
           ((u32)entry[ENTRY_SIZE + 2u] << 16u) |
           ((u32)entry[ENTRY_SIZE + 3u] << 24u);
}

/* FAT12 packs two 12 bit values into every three bytes. */
static u16 next_cluster(u16 cluster) {
    const u16 offset = (u16)(cluster + (cluster / 2u));
    const u16 pair = (u16)((u16)FAT_WINDOW[offset] |
                           (u16)((u16)FAT_WINDOW[offset + 1u] << 8u));

    if ((cluster & 1u) != 0u) {
        return (u16)(pair >> 4u);
    }
    return (u16)(pair & 0x0FFFu);
}

u16 file_system_find(const char *name) {
    u16 slot;

    for (slot = 0u; slot < visible_count; ++slot) {
        char candidate[13];

        if (hidden_entry[visible_slot[slot]] != 0u) {
            continue;
        }
        file_system_name(visible_slot[slot], candidate);
        if (text_equal_ignore_case(candidate, name) != 0u) {
            return visible_slot[slot];
        }
    }
    return FILE_NOT_FOUND;
}

/* Copies up to length bytes of the file, starting at offset, into buffer and
 * returns how many bytes were copied. Only the clusters the bootloader
 * preloaded are reachable, which is every file a 1.44 MB boot disk holds in
 * practice. */
u32 file_system_read(u16 index, u32 offset, void *buffer, u32 length) {
    const u8 *entry = directory_entry(index);
    u16 cluster = (u16)((u16)entry[ENTRY_FIRST_CLUSTER] |
                        (u16)((u16)entry[ENTRY_FIRST_CLUSTER + 1u] << 8u));
    u32 size = file_system_size(index);
    u8 *target = (u8 *)buffer;
    u32 done = 0u;

    if (offset >= size) {
        return 0u;
    }
    if (length > (size - offset)) {
        length = size - offset;
    }

    /* Walk to the cluster that holds the first wanted byte. */
    while (offset >= 512u && cluster >= 2u && cluster < CLUSTER_END) {
        offset -= 512u;
        cluster = next_cluster(cluster);
    }

    while (done < length && cluster >= 2u && cluster < CLUSTER_END) {
        const u32 cluster_start = (u32)(cluster - 2u) * 512u;
        u32 available = 512u - offset;
        u32 take = length - done;

        if (cluster_start + 512u > DATA_WINDOW_SECTORS * 512u) {
            break; /* past the part of the disk the bootloader preloaded */
        }
        if (take > available) {
            take = available;
        }
        memory_copy(target + done, DATA_WINDOW + cluster_start + offset, take);
        done += take;
        offset = 0u;
        cluster = next_cluster(cluster);
    }
    return done;
}

u8 file_system_is_protected(u16 index) {
    char name[13];

    file_system_name(index, name);
    return text_equal_ignore_case(name, "KERNEL.BIN");
}

u8 file_system_delete(u16 index) {
    if (entry_visible(index) == 0u) {
        return 0u;
    }
    if (file_system_is_protected(index) != 0u) {
        return 0u;
    }
    hidden_entry[index] = 1u;
    return 1u;
}

/* The boot sector is still readable, which the "about" window uses to show
 * what the disk actually is. */
u32 file_system_volume_sectors(void) {
    return (u32)BOOT_SECTOR_WINDOW[19] |
           ((u32)BOOT_SECTOR_WINDOW[20] << 8u);
}

u16 file_system_sector_bytes(void) {
    return (u16)((u16)BOOT_SECTOR_WINDOW[11] |
                 (u16)((u16)BOOT_SECTOR_WINDOW[12] << 8u));
}
