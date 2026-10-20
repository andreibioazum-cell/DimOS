/*
 * volume.c -- a FAT12 file system the kernel can write.
 *
 * The boot loader only shows DimOS a frozen copy of the boot disk, so the
 * file manager could list and read files but never save anything. Now that
 * ata.c can read and write sectors, this file turns the disk into a real
 * volume: it parses the BPB, follows cluster chains, creates and deletes
 * directory entries, allocates clusters and keeps both FAT copies in sync.
 *
 * FAT12 is deliberately the format. It is what the boot media already uses,
 * it is understood by every other system (mtools, Linux, Windows), and even
 * a 32 MB disk needs only a few FAT sectors -- cheap enough to rewrite on a
 * machine with no cache and no interrupts.
 *
 * The geometry DimOS writes when it formats a disk is the classic floppy
 * one: 512-byte sectors, 224 root entries, two FATs, and one sector per
 * cluster on small disks, doubling until the cluster count fits FAT12.
 */

#include "dimos.h"

#define SECTOR_BYTES 512u
#define ENTRY_BYTES 32u
#define ROOT_ENTRIES 224u
#define FAT_COPIES 2u
#define FAT12_LIMIT 4084u
#define CLUSTER_FREE 0x000u
#define CLUSTER_END 0xFFFu
#define NO_CLUSTER 0xFFFFu
#define ENTRY_FREE 0xE5u
#define ENTRY_LAST 0x00u
#define ATTRIBUTE_ARCHIVE 0x20u
#define ATTRIBUTE_DIRECTORY 0x10u
#define ATTRIBUTE_VOLUME 0x08u
#define ATTRIBUTE_LONG_NAME 0x0Fu
#define NO_SECTOR 0xFFFFFFFFu

typedef struct {
    u8 present;
    u8 sectors_per_cluster;
    u8 fat_count;
    u16 reserved_sectors;
    u16 root_entries;
    u16 fat_sectors;
    u32 total_sectors;
    u32 root_start;
    u32 root_sectors;
    u32 data_start;
    u16 cluster_count;
} Volume;

static Volume volume;

/* One data sector, one FAT sector and one directory sector. Keeping the
 * FAT sector mounted makes a chain walk cost a read per sector rather than
 * per cluster, and every write is flushed back to both copies at once. */
static u8 data_sector[SECTOR_BYTES];
static u8 fat_sector[SECTOR_BYTES];
static u8 root_sector[SECTOR_BYTES];
static u32 fat_sector_lba = NO_SECTOR;
static u8 fat_sector_dirty;
static u32 root_sector_lba = NO_SECTOR;

/* ------------------------------------------------------------------ */
/* Sector access                                                       */
/* ------------------------------------------------------------------ */

static u8 read_sector(u32 lba, void *buffer) {
    return ata_read(lba, buffer, 1u);
}

static u8 write_sector(u32 lba, const void *buffer) {
    return ata_write(lba, buffer, 1u);
}

static u32 cluster_lba(u16 cluster) {
    return volume.data_start +
           (u32)(cluster - 2u) * (u32)volume.sectors_per_cluster;
}

static u16 cluster_bytes(void) {
    return (u16)((u16)volume.sectors_per_cluster * SECTOR_BYTES);
}

/* ------------------------------------------------------------------ */
/* The allocation table                                                */
/* ------------------------------------------------------------------ */

static u8 fat_flush(void) {
    u8 copy;

    if (fat_sector_dirty == 0u) {
        return 1u;
    }
    for (copy = 0u; copy < volume.fat_count; ++copy) {
        const u32 target = fat_sector_lba +
                           (u32)copy * (u32)volume.fat_sectors;

        if (write_sector(target, fat_sector) == 0u) {
            return 0u;
        }
    }
    fat_sector_dirty = 0u;
    return 1u;
}

static u8 fat_byte_read(u32 index, u8 *out) {
    const u32 lba = (u32)volume.reserved_sectors + index / SECTOR_BYTES;

    if (fat_sector_lba != lba) {
        if (fat_flush() == 0u) {
            return 0u;
        }
        if (read_sector(lba, fat_sector) == 0u) {
            fat_sector_lba = NO_SECTOR;
            return 0u;
        }
        fat_sector_lba = lba;
    }
    *out = fat_sector[index % SECTOR_BYTES];
    return 1u;
}

static u8 fat_byte_write(u32 index, u8 value) {
    const u32 lba = (u32)volume.reserved_sectors + index / SECTOR_BYTES;

    if (fat_sector_lba != lba) {
        if (fat_flush() == 0u) {
            return 0u;
        }
        if (read_sector(lba, fat_sector) == 0u) {
            fat_sector_lba = NO_SECTOR;
            return 0u;
        }
        fat_sector_lba = lba;
    }
    if (fat_sector[index % SECTOR_BYTES] != value) {
        fat_sector[index % SECTOR_BYTES] = value;
        fat_sector_dirty = 1u;
    }
    return 1u;
}

/* FAT12 packs two 12-bit entries into three bytes, so an odd cluster starts
 * halfway through a byte -- and a badly placed one straddles a sector. */
static u16 fat_read(u16 cluster) {
    const u32 first = (u32)cluster * 3u / 2u;
    u8 low;
    u8 high;

    if (fat_byte_read(first, &low) == 0u ||
        fat_byte_read(first + 1u, &high) == 0u) {
        return CLUSTER_END;
    }
    if ((cluster & 1u) != 0u) {
        return (u16)(((u16)high << 4u) | ((u16)low >> 4u));
    }
    return (u16)((u16)low | ((u16)(high & 0x0Fu) << 8u));
}

static u8 fat_write(u16 cluster, u16 value) {
    const u32 first = (u32)cluster * 3u / 2u;
    u8 low;
    u8 high;

    if (fat_byte_read(first, &low) == 0u ||
        fat_byte_read(first + 1u, &high) == 0u) {
        return 0u;
    }

    if ((cluster & 1u) != 0u) {
        low = (u8)((low & 0x0Fu) | (u8)((value << 4u) & 0xF0u));
        high = (u8)(value >> 4u);
    } else {
        low = (u8)(value & 0xFFu);
        high = (u8)((high & 0xF0u) | (u8)((value >> 8u) & 0x0Fu));
    }
    if (fat_byte_write(first, low) == 0u ||
        fat_byte_write(first + 1u, high) == 0u) {
        return 0u;
    }
    return 1u;
}

static void free_chain(u16 cluster) {
    u16 guard = 0u;

    while (cluster >= 2u && cluster < 0xFF0u && guard < 4096u) {
        const u16 next = fat_read(cluster);

        fat_write(cluster, CLUSTER_FREE);
        cluster = next;
        ++guard;
    }
}

/* Marks `count` free clusters as one chain and returns its first cluster,
 * or NO_CLUSTER when the disk is full. */
static u16 allocate_chain(u16 count) {
    u16 first = NO_CLUSTER;
    u16 previous = NO_CLUSTER;
    u16 cluster;
    u16 found = 0u;

    if (count == 0u) {
        return NO_CLUSTER;
    }
    for (cluster = 2u; cluster < (u16)(volume.cluster_count + 2u) && found < count;
         ++cluster) {
        if (fat_read(cluster) != CLUSTER_FREE) {
            continue;
        }
        if (fat_write(cluster, CLUSTER_END) == 0u) {
            break;
        }
        if (previous != NO_CLUSTER) {
            fat_write(previous, cluster);
        } else {
            first = cluster;
        }
        previous = cluster;
        ++found;
    }

    if (found < count) {
        if (first != NO_CLUSTER) {
            free_chain(first);
        }
        return NO_CLUSTER;
    }
    return first;
}

/* ------------------------------------------------------------------ */
/* Directory entries                                                   */
/* ------------------------------------------------------------------ */

static u8 entry_is_file(const u8 *entry) {
    if (entry[0] == ENTRY_LAST || entry[0] == ENTRY_FREE) {
        return 0u;
    }
    if (entry[11] == ATTRIBUTE_LONG_NAME) {
        return 0u;
    }
    if ((entry[11] & (u8)(ATTRIBUTE_VOLUME | ATTRIBUTE_DIRECTORY)) != 0u) {
        return 0u;
    }
    return 1u;
}

static u16 entry_cluster(const u8 *entry) {
    return (u16)((u16)entry[26] | (u16)((u16)entry[27] << 8u));
}

static u32 entry_size(const u8 *entry) {
    return (u32)entry[28] | ((u32)entry[29] << 8u) |
           ((u32)entry[30] << 16u) | ((u32)entry[31] << 24u);
}

static void entry_name(const u8 *entry, char *out) {
    u16 position = 0u;
    u8 column;

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

/* Reads the directory sector that holds an entry, keeping it mounted while
 * a scan walks all sixteen slots it contains. */
static const u8 *root_entry(u16 index) {
    const u32 lba = volume.root_start + (u32)index / 16u;

    if (root_sector_lba != lba) {
        if (read_sector(lba, root_sector) == 0u) {
            root_sector_lba = NO_SECTOR;
            return (const u8 *)0;
        }
        root_sector_lba = lba;
    }
    return root_sector + ((u32)index % 16u) * ENTRY_BYTES;
}

static u8 root_entry_update(u16 index, const u8 *entry) {
    const u32 lba = volume.root_start + (u32)index / 16u;

    if (root_sector_lba != lba) {
        if (read_sector(lba, root_sector) == 0u) {
            root_sector_lba = NO_SECTOR;
            return 0u;
        }
        root_sector_lba = lba;
    }
    memory_copy(root_sector + ((u32)index % 16u) * ENTRY_BYTES, entry,
                ENTRY_BYTES);
    root_sector_lba = NO_SECTOR; /* the cached copy is now stale elsewhere */
    return write_sector(lba, root_sector);
}

/* Finds the directory slot holding `name`, or NO_CLUSTER. `free_slot`
 * receives the first reusable entry, so creating a file does not have to
 * scan the root directory twice. */
static u16 find_entry(const char *name, u16 *free_slot) {
    u16 index;
    u16 reusable = NO_CLUSTER;

    for (index = 0u; index < volume.root_entries; ++index) {
        const u8 *entry = root_entry(index);

        if (entry == (const u8 *)0) {
            break;
        }
        if (entry[0] == ENTRY_LAST) {
            if (reusable == NO_CLUSTER) {
                reusable = index;
            }
            break;
        }
        if (entry[0] == ENTRY_FREE) {
            if (reusable == NO_CLUSTER) {
                reusable = index;
            }
            continue;
        }
        if (entry_is_file(entry) != 0u) {
            char candidate[13];

            entry_name(entry, candidate);
            if (text_equal_ignore_case(candidate, name) != 0u) {
                if (free_slot != (u16 *)0) {
                    *free_slot = index;
                }
                return index;
            }
        }
    }

    if (free_slot != (u16 *)0) {
        *free_slot = reusable;
    }
    return NO_CLUSTER;
}

static u8 name_character_ok(u8 character, u8 first) {
    const u8 upper = character_to_upper(character);

    if ((upper >= 'A' && upper <= 'Z') || (upper >= '0' && upper <= '9')) {
        return 1u;
    }
    if (first != 0u) {
        return 0u;
    }
    switch (upper) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '-': case '@': case '^': case '_': case '`': case '~':
        case '(': case ')': case '{': case '}':
            return 1u;
        default:
            return 0u;
    }
}

/* "notes.txt" -> "NOTES   TXT". Returns 0 for a name FAT12 cannot hold. */
static u8 encode_name(const char *name, u8 *out) {
    u16 position = 0u;
    u8 base = 0u;
    u8 extension = 0u;
    u8 seen_dot = 0u;
    u8 column;

    for (column = 0u; column < 11u; ++column) {
        out[column] = ' ';
    }

    while (name[position] != '\0') {
        const u8 character = (u8)name[position];

        if (character == '.') {
            if (seen_dot != 0u) {
                return 0u;
            }
            seen_dot = 1u;
            ++position;
            continue;
        }
        if (name_character_ok(character,
                              (u8)(base == 0u && seen_dot == 0u)) == 0u) {
            return 0u;
        }
        if (seen_dot == 0u) {
            if (base >= 8u) {
                return 0u;
            }
            out[base++] = character_to_upper(character);
        } else {
            if (extension >= 3u) {
                return 0u;
            }
            out[8u + extension] = character_to_upper(character);
            ++extension;
        }
        ++position;
    }
    return (u8)(base != 0u || extension != 0u);
}

/* ------------------------------------------------------------------ */
/* Mount and format                                                    */
/* ------------------------------------------------------------------ */

static u16 read_word(const u8 *buffer, u16 offset) {
    return (u16)((u16)buffer[offset] | (u16)((u16)buffer[offset + 1u] << 8u));
}

static u32 read_long(const u8 *buffer, u16 offset) {
    return (u32)buffer[offset] | ((u32)buffer[offset + 1u] << 8u) |
           ((u32)buffer[offset + 2u] << 16u) | ((u32)buffer[offset + 3u] << 24u);
}

static void write_word(u8 *buffer, u16 offset, u16 value) {
    buffer[offset] = (u8)(value & 0xFFu);
    buffer[offset + 1u] = (u8)(value >> 8u);
}

static void write_long(u8 *buffer, u16 offset, u32 value) {
    buffer[offset] = (u8)(value & 0xFFu);
    buffer[offset + 1u] = (u8)((value >> 8u) & 0xFFu);
    buffer[offset + 2u] = (u8)((value >> 16u) & 0xFFu);
    buffer[offset + 3u] = (u8)((value >> 24u) & 0xFFu);
}

/* FAT name fields are fixed width and space padded, never terminated. */
static void write_padded(u8 *buffer, u16 offset, const char *text, u16 width) {
    u16 index = 0u;

    while (index < width && text[index] != '\0') {
        buffer[offset + index] = (u8)text[index];
        ++index;
    }
    while (index < width) {
        buffer[offset + index] = ' ';
        ++index;
    }
}

static void volume_reset(void) {
    memory_zero(&volume, (u32)sizeof(volume));
    fat_sector_lba = NO_SECTOR;
    root_sector_lba = NO_SECTOR;
    fat_sector_dirty = 0u;
}

u8 volume_init(void) {
    const u8 *boot = data_sector;
    u16 bytes_per_sector;
    u32 total;

    volume_reset();
    if (ata_present() == 0u && ata_init() == 0u) {
        return 0u;
    }
    if (read_sector(0u, data_sector) == 0u) {
        return 0u;
    }
    if (boot[510] != 0x55u || boot[511] != 0xAAu) {
        return 0u;
    }

    bytes_per_sector = read_word(boot, 11u);
    if (bytes_per_sector != SECTOR_BYTES) {
        return 0u; /* DimOS only speaks 512-byte sectors */
    }

    volume.sectors_per_cluster = boot[13];
    volume.reserved_sectors = read_word(boot, 14u);
    volume.fat_count = boot[16];
    volume.root_entries = read_word(boot, 17u);
    total = read_word(boot, 19u);
    if (total == 0u) {
        total = read_long(boot, 32u);
    }
    volume.fat_sectors = read_word(boot, 22u);
    if (volume.fat_sectors == 0u) {
        return 0u; /* FAT32 is out of scope */
    }

    if (volume.sectors_per_cluster == 0u || volume.reserved_sectors == 0u ||
        volume.fat_count == 0u || volume.root_entries == 0u || total == 0u) {
        return 0u;
    }
    if (total > ata_sectors()) {
        return 0u;
    }

    volume.total_sectors = total;
    volume.root_start = (u32)volume.reserved_sectors +
                        (u32)volume.fat_count * (u32)volume.fat_sectors;
    volume.root_sectors = ((u32)volume.root_entries * ENTRY_BYTES +
                           SECTOR_BYTES - 1u) / SECTOR_BYTES;
    volume.data_start = volume.root_start + volume.root_sectors;
    if (volume.data_start >= total) {
        return 0u;
    }
    volume.cluster_count =
        (u16)((total - volume.data_start) / volume.sectors_per_cluster);
    if (volume.cluster_count < 1u) {
        return 0u;
    }

    /* A cluster count at or above 4085 would make this FAT16, which this
     * driver does not implement. */
    volume.present = (u8)(volume.cluster_count < FAT12_LIMIT);
    return volume.present;
}

/* Writes a fresh, empty FAT12 volume over the start of the disk. */
u8 volume_format(void) {
    u32 total;
    u16 root_sectors;
    u16 fat_sectors = 1u;
    u8 spc = 1u;
    u32 clusters = 0u;
    u8 attempt;
    u16 index;

    if (ata_present() == 0u) {
        return 0u;
    }

    total = ata_sectors();
    if (total > 0xFFFFu) {
        total = 0xFFFFu;
    }
    if (total < 256u) {
        return 0u;
    }

    root_sectors = (u16)(((u32)ROOT_ENTRIES * ENTRY_BYTES + SECTOR_BYTES - 1u) /
                         SECTOR_BYTES);

    for (attempt = 0u; attempt < 8u; ++attempt) {
        u8 round;

        for (round = 0u; round < 8u; ++round) {
            u32 next;

            clusters = (total - 1u - root_sectors) / spc;
            fat_sectors = (u16)(((clusters + 2u) * 3u / 2u + SECTOR_BYTES - 1u) /
                                SECTOR_BYTES);
            if (fat_sectors == 0u) {
                fat_sectors = 1u;
            }
            if (total <= 1u + (u32)FAT_COPIES * fat_sectors + root_sectors) {
                return 0u;
            }
            next = (total - 1u - (u32)FAT_COPIES * fat_sectors - root_sectors) /
                   spc;
            if (next == clusters) {
                break;
            }
            clusters = next;
        }
        if (clusters > 16u && clusters <= FAT12_LIMIT) {
            break;
        }
        if (spc >= 64u) {
            return 0u;
        }
        spc = (u8)(spc * 2u);
    }
    if (clusters == 0u || clusters > FAT12_LIMIT) {
        return 0u;
    }

    volume_reset();

    /* Boot sector and BIOS parameter block. */
    memory_zero(data_sector, SECTOR_BYTES);
    data_sector[0] = 0xEBu;
    data_sector[1] = 0x3Cu;
    data_sector[2] = 0x90u;
    write_padded(data_sector, 3u, "DIMOS14", 8u);
    write_word(data_sector, 11u, SECTOR_BYTES);
    data_sector[13] = spc;
    write_word(data_sector, 14u, 1u);
    data_sector[16] = FAT_COPIES;
    write_word(data_sector, 17u, ROOT_ENTRIES);
    write_word(data_sector, 19u, (u16)total);
    data_sector[21] = 0xF0u;
    write_word(data_sector, 22u, fat_sectors);
    write_word(data_sector, 24u, 18u);
    write_word(data_sector, 26u, 2u);
    write_long(data_sector, 28u, 0u);
    write_long(data_sector, 32u, 0u);
    data_sector[36] = 0x00u;
    data_sector[38] = 0x29u;
    write_long(data_sector, 39u, 0x44494D4Fu);
    write_padded(data_sector, 43u, "DIMOS 14", 11u);
    write_padded(data_sector, 54u, "FAT12", 8u);
    data_sector[510] = 0x55u;
    data_sector[511] = 0xAAu;
    if (write_sector(0u, data_sector) == 0u) {
        return 0u;
    }

    /* Both allocation tables: entry 0 carries the media byte, entry 1 is
     * the end-of-chain marker, everything after that is free. */
    for (index = 0u; index < fat_sectors; ++index) {
        u8 copy;

        memory_zero(data_sector, SECTOR_BYTES);
        if (index == 0u) {
            data_sector[0] = 0xF0u;
            data_sector[1] = 0xFFu;
            data_sector[2] = 0xFFu;
        }
        for (copy = 0u; copy < FAT_COPIES; ++copy) {
            const u32 target = 1u + (u32)copy * fat_sectors + index;

            if (write_sector(target, data_sector) == 0u) {
                return 0u;
            }
        }
    }

    /* An empty root directory. */
    memory_zero(data_sector, SECTOR_BYTES);
    {
        const u32 root_start = 1u + (u32)FAT_COPIES * fat_sectors;

        for (index = 0u; index < root_sectors; ++index) {
            if (write_sector(root_start + index, data_sector) == 0u) {
                return 0u;
            }
        }
    }

    ata_flush();
    return volume_init();
}

u8 volume_present(void) {
    return volume.present;
}

u32 volume_sectors(void) {
    return volume.present != 0u ? volume.total_sectors : 0u;
}

u32 volume_total_bytes(void) {
    return volume.present != 0u ? volume.total_sectors * SECTOR_BYTES : 0u;
}

u32 volume_free_bytes(void) {
    u32 free_clusters = 0u;
    u16 cluster;

    if (volume.present == 0u) {
        return 0u;
    }
    for (cluster = 2u; cluster < (u16)(volume.cluster_count + 2u); ++cluster) {
        if (fat_read(cluster) == CLUSTER_FREE) {
            ++free_clusters;
        }
    }
    fat_flush();
    return free_clusters * (u32)cluster_bytes();
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

u16 volume_file_count(void) {
    u16 index;
    u16 count = 0u;

    if (volume.present == 0u) {
        return 0u;
    }
    for (index = 0u; index < volume.root_entries; ++index) {
        const u8 *entry = root_entry(index);

        if (entry == (const u8 *)0 || entry[0] == ENTRY_LAST) {
            break;
        }
        if (entry_is_file(entry) != 0u) {
            ++count;
        }
    }
    return count;
}

u8 volume_file(u16 slot, char *name, u32 *size) {
    u16 index;
    u16 seen = 0u;

    if (volume.present == 0u) {
        return 0u;
    }
    for (index = 0u; index < volume.root_entries; ++index) {
        const u8 *entry = root_entry(index);

        if (entry == (const u8 *)0 || entry[0] == ENTRY_LAST) {
            return 0u;
        }
        if (entry_is_file(entry) == 0u) {
            continue;
        }
        if (seen == slot) {
            entry_name(entry, name);
            if (size != (u32 *)0) {
                *size = entry_size(entry);
            }
            return 1u;
        }
        ++seen;
    }
    return 0u;
}

u32 volume_read_file(const char *name, void *buffer, u32 capacity) {
    u16 index;
    u16 free_slot;
    const u8 *entry;
    u16 cluster;
    u32 size;
    u32 done = 0u;
    u8 *target = (u8 *)buffer;

    if (volume.present == 0u) {
        return 0u;
    }
    index = find_entry(name, &free_slot);
    if (index == NO_CLUSTER) {
        return 0u;
    }
    entry = root_entry(index);
    if (entry == (const u8 *)0) {
        return 0u;
    }
    cluster = entry_cluster(entry);
    size = entry_size(entry);
    if (size > capacity) {
        size = capacity;
    }

    while (done < size && cluster >= 2u && cluster < 0xFF0u) {
        u16 step;

        for (step = 0u; step < volume.sectors_per_cluster && done < size; ++step) {
            u32 take = size - done;

            if (read_sector(cluster_lba(cluster) + step, data_sector) == 0u) {
                return done;
            }
            if (take > SECTOR_BYTES) {
                take = SECTOR_BYTES;
            }
            memory_copy(target + done, data_sector, take);
            done += take;
        }
        cluster = fat_read(cluster);
    }
    return done;
}

u8 volume_write_file(const char *name, const void *data, u32 length) {
    u8 encoded[11];
    u16 index;
    u16 free_slot = NO_CLUSTER;
    u16 first = NO_CLUSTER;
    u16 cluster;
    u16 needed;
    u32 remaining = length;
    const u8 *source = (const u8 *)data;
    u8 entry[ENTRY_BYTES];

    if (volume.present == 0u || encode_name(name, encoded) == 0u) {
        return 0u;
    }

    index = find_entry(name, &free_slot);
    if (index == NO_CLUSTER && free_slot == NO_CLUSTER) {
        return 0u; /* the root directory is full */
    }

    /* Replace: drop the old chain before allocating, so a file can grow
     * into the space it used to occupy. */
    if (index != NO_CLUSTER) {
        const u8 *existing = root_entry(index);

        if (existing == (const u8 *)0) {
            return 0u;
        }
        free_chain(entry_cluster(existing));
    }

    needed = (u16)((length + (u32)cluster_bytes() - 1u) / (u32)cluster_bytes());
    if (needed != 0u) {
        first = allocate_chain(needed);
        if (first == NO_CLUSTER) {
            return 0u;
        }

        cluster = first;
        while (cluster >= 2u && cluster < 0xFF0u && remaining != 0u) {
            u16 step;

            for (step = 0u; step < volume.sectors_per_cluster; ++step) {
                u32 take = remaining;

                memory_zero(data_sector, SECTOR_BYTES);
                if (take > SECTOR_BYTES) {
                    take = SECTOR_BYTES;
                }
                if (take != 0u) {
                    memory_copy(data_sector, source, take);
                    source += take;
                    remaining -= take;
                }
                if (write_sector(cluster_lba(cluster) + step, data_sector) == 0u) {
                    return 0u;
                }
                if (remaining == 0u) {
                    break;
                }
            }
            cluster = fat_read(cluster);
        }
    }

    /* Create or update the directory entry. */
    if (index == NO_CLUSTER) {
        index = free_slot;
    }
    memory_zero(entry, ENTRY_BYTES);
    memory_copy(entry, encoded, 11u);
    entry[11] = ATTRIBUTE_ARCHIVE;
    entry[26] = (u8)(first & 0xFFu);
    entry[27] = (u8)(first >> 8u);
    write_long(entry, 28u, length);
    if (root_entry_update(index, entry) == 0u) {
        return 0u;
    }

    if (fat_flush() == 0u) {
        return 0u;
    }
    ata_flush();
    return 1u;
}

u8 volume_delete_file(const char *name) {
    u8 encoded[11];
    u16 index;
    u16 free_slot;
    const u8 *existing;
    u8 entry[ENTRY_BYTES];

    if (volume.present == 0u || encode_name(name, encoded) == 0u) {
        return 0u;
    }
    index = find_entry(name, &free_slot);
    if (index == NO_CLUSTER) {
        return 0u;
    }
    existing = root_entry(index);
    if (existing == (const u8 *)0) {
        return 0u;
    }
    memory_copy(entry, existing, ENTRY_BYTES);
    free_chain(entry_cluster(entry));
    entry[0] = ENTRY_FREE;
    if (root_entry_update(index, entry) == 0u) {
        return 0u;
    }
    if (fat_flush() == 0u) {
        return 0u;
    }
    ata_flush();
    return 1u;
}
