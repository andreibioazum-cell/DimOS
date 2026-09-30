/*
 * image_check.c -- checks the build artifacts before they are published.
 *
 * The same checks the build script used to make, in plain C:
 *   - the boot sector is 512 bytes, starts with the FAT jump and ends
 *     with the BIOS signature;
 *   - the kernel fits the loader window;
 *   - the FAT12 geometry of the floppy image is the one the bootloader
 *     hardcodes;
 *   - KERNEL.BIN is really in the root directory, with the right size;
 *   - the hard disk image is the floppy image plus zero padding;
 *   - the ISO carries an El Torito boot catalog for 1.44M floppy
 *     emulation, which is what v86 and SeaBIOS expect.
 *
 * Usage: image_check BOOT.BIN KERNEL.BIN dimos.img dimos.hdd [dimos.iso]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define BOOT_SECTOR_SIZE 512u
#define FLOPPY_SIZE 1474560u
#define HDD_SIZE 8388608u
/* The bootloader walks the FAT chain into 2000:0000; the preloaded data
 * window at 0x30000 caps the kernel at 126 sectors. */
#define MAXIMUM_KERNEL_SIZE 64512u
#define ISO_SECTOR 2048u

static const char kernel_fat_name[11] = {
    'K', 'E', 'R', 'N', 'E', 'L', ' ', ' ', 'B', 'I', 'N'
};

static int failures;

static void fail(const char *message) {
    printf("  FAILED  %s\n", message);
    ++failures;
}

static void require(int condition, const char *message) {
    if (condition == 0) {
        fail(message);
    }
}

static u8 *read_whole_file(const char *path, size_t *size_out) {
    FILE *handle = fopen(path, "rb");
    u8 *data;
    long size;

    if (handle == NULL) {
        printf("  FAILED  cannot open %s\n", path);
        ++failures;
        return NULL;
    }
    fseek(handle, 0, SEEK_END);
    size = ftell(handle);
    fseek(handle, 0, SEEK_SET);
    if (size < 0) {
        fclose(handle);
        printf("  FAILED  cannot measure %s\n", path);
        ++failures;
        return NULL;
    }
    data = (u8 *)malloc((size_t)size + 1u);
    if (data == NULL) {
        fclose(handle);
        printf("  FAILED  out of memory reading %s\n", path);
        ++failures;
        return NULL;
    }
    if (size > 0 && fread(data, 1u, (size_t)size, handle) != (size_t)size) {
        printf("  FAILED  short read of %s\n", path);
        ++failures;
    }
    fclose(handle);
    *size_out = (size_t)size;
    return data;
}

static u16 read_u16(const u8 *data, size_t offset) {
    return (u16)((u16)data[offset] | (u16)((u16)data[offset + 1u] << 8u));
}

static u32 read_u32(const u8 *data, size_t offset) {
    return (u32)data[offset] |
           ((u32)data[offset + 1u] << 8u) |
           ((u32)data[offset + 2u] << 16u) |
           ((u32)data[offset + 3u] << 24u);
}

static int same(const u8 *a, const u8 *b, size_t length) {
    return memcmp(a, b, length) == 0;
}

static int text_at(const u8 *data, size_t offset, const char *text) {
    return memcmp(data + offset, text, strlen(text)) == 0;
}

/* The FAT boot sector has to start with a short jump over the parameter
 * block, so the block always sits at offset 3. */
static void check_boot_jump(const u8 *sector, const char *name) {
    require(sector[0] == 0xEBu && sector[2] == 0x90u,
            name);
    if (sector[0] != 0xEBu || sector[2] != 0x90u) {
        printf("          %s must start with JMP SHORT + NOP (EB xx 90)\n", name);
    }
}

static void check_bootloader(const u8 *bootloader, size_t size) {
    printf("Boot sector\n");
    require(size == BOOT_SECTOR_SIZE, "BOOT.BIN must be exactly 512 bytes");
    if (size < BOOT_SECTOR_SIZE) {
        return;
    }
    check_boot_jump(bootloader, "BOOT.BIN");
    require(bootloader[510] == 0x55u && bootloader[511] == 0xAAu,
            "BOOT.BIN must end with the BIOS signature 0x55AA");
}

static void check_kernel(size_t size) {
    printf("Kernel\n");
    require(size > 0u, "KERNEL.BIN is empty");
    require(size <= MAXIMUM_KERNEL_SIZE,
            "KERNEL.BIN exceeds the 64512 byte loader window");
    printf("          %u bytes\n", (unsigned)size);
}

static void check_geometry(const u8 *image, size_t size) {
    printf("FAT12 geometry\n");
    require(size == FLOPPY_SIZE, "the floppy image must be 1474560 bytes");
    if (size < BOOT_SECTOR_SIZE) {
        return;
    }
    check_boot_jump(image, "the floppy image");
    require(read_u16(image, 11) == 512u, "bytes per sector must be 512");
    require(image[13] == 1u, "sectors per cluster must be 1");
    require(read_u16(image, 14) == 1u, "the reserved sector count must be 1");
    require(image[16] == 2u, "there must be two FAT copies");
    require(read_u16(image, 17) == 224u, "the root directory must hold 224 entries");
    require(read_u16(image, 19) == 2880u, "the total sector count must be 2880");
    require(image[21] == 0xF0u, "the media descriptor must be 0xF0");
    require(read_u16(image, 22) == 9u, "sectors per FAT must be 9");
    require(image[510] == 0x55u && image[511] == 0xAAu,
            "the floppy image must contain the BIOS signature 0x55AA");
}

static void check_installed_bootloader(const u8 *bootloader, const u8 *image,
                                       size_t size) {
    printf("Installed boot sector\n");
    require(size >= BOOT_SECTOR_SIZE, "the floppy image has no boot sector");
    require(same(bootloader, image, BOOT_SECTOR_SIZE),
            "the floppy boot sector differs from BOOT.BIN");
}

static void check_hard_disk(const u8 *floppy, size_t floppy_size,
                            const u8 *hdd, size_t hdd_size) {
    size_t offset;

    printf("Hard disk image\n");
    require(hdd_size == HDD_SIZE, "the hard disk image must be 8388608 bytes");
    require(hdd_size >= floppy_size,
            "the hard disk image is smaller than the FAT12 volume");
    if (hdd_size < floppy_size) {
        return;
    }
    require(same(floppy, hdd, floppy_size),
            "the hard disk image must start with the exact FAT12 floppy image");
    for (offset = floppy_size; offset < hdd_size; ++offset) {
        if (hdd[offset] != 0u) {
            printf("  FAILED  the padding after the volume must be zero "
                   "(first nonzero byte at %u)\n", (unsigned)offset);
            ++failures;
            return;
        }
    }
}

static void check_kernel_directory_entry(const u8 *image, size_t image_size,
                                         size_t kernel_size) {
    const size_t bytes_per_sector = (size_t)read_u16(image, 11);
    const size_t reserved_sectors = (size_t)read_u16(image, 14);
    const size_t fat_count = (size_t)image[16];
    const size_t root_entries = (size_t)read_u16(image, 17);
    const size_t sectors_per_fat = (size_t)read_u16(image, 22);
    const size_t root_offset =
        (reserved_sectors + fat_count * sectors_per_fat) * bytes_per_sector;
    size_t entry;

    printf("Root directory\n");
    require(root_offset + root_entries * 32u <= image_size,
            "the FAT12 root directory extends beyond the image");
    if (root_offset + root_entries * 32u > image_size) {
        return;
    }

    for (entry = 0u; entry < root_entries; ++entry) {
        const size_t offset = root_offset + entry * 32u;

        if (image[offset] == 0x00u) {
            break; /* no more entries */
        }
        if (image[offset] == 0xE5u || image[offset + 11u] == 0x0Fu) {
            continue; /* deleted, or a long file name helper entry */
        }
        if (memcmp(image + offset, kernel_fat_name, sizeof(kernel_fat_name)) == 0) {
            require(read_u32(image, offset + 28u) == (u32)kernel_size,
                    "the KERNEL.BIN directory size does not match the compiled kernel");
            require(read_u16(image, offset + 26u) >= 2u,
                    "KERNEL.BIN has an invalid first cluster");
            printf("          KERNEL.BIN at cluster %u\n",
                   (unsigned)read_u16(image, offset + 26u));
            return;
        }
    }
    fail("KERNEL.BIN is missing from the FAT12 root directory");
}

static void check_iso(const u8 *iso, size_t size) {
    size_t catalog;

    printf("El Torito catalog\n");
    require(size >= 18u * ISO_SECTOR,
            "the ISO image is too small to contain an El Torito catalog");
    if (size < 18u * ISO_SECTOR) {
        return;
    }
    require(iso[16u * ISO_SECTOR] == 1u && text_at(iso, 16u * ISO_SECTOR + 1u, "CD001"),
            "the ISO primary volume descriptor is missing");
    require(iso[17u * ISO_SECTOR] == 0u && text_at(iso, 17u * ISO_SECTOR + 1u, "CD001"),
            "the ISO El Torito boot record is missing");
    require(text_at(iso, 17u * ISO_SECTOR + 7u, "EL TORITO SPECIFICATION"),
            "the ISO boot record is not marked EL TORITO SPECIFICATION");

    catalog = (size_t)read_u32(iso, 17u * ISO_SECTOR + 71u) * ISO_SECTOR;
    require(catalog + 64u <= size, "the El Torito catalog is outside the ISO");
    if (catalog + 64u > size) {
        return;
    }
    require(iso[catalog] == 0x01u && iso[catalog + 30u] == 0x55u &&
                iso[catalog + 31u] == 0xAAu,
            "the El Torito validation entry is invalid");
    require(iso[catalog + 32u] == 0x88u, "the El Torito default entry is not bootable");
    require(iso[catalog + 33u] == 0x02u,
            "the El Torito media type must be 1.44M floppy emulation");
}

int main(int argc, char **argv) {
    size_t bootloader_size = 0u;
    size_t kernel_size = 0u;
    size_t floppy_size = 0u;
    size_t hdd_size = 0u;
    u8 *bootloader;
    u8 *kernel;
    u8 *floppy;
    u8 *hdd;

    if (argc < 5 || argc > 6) {
        fprintf(stderr,
                "Usage: %s <BOOT.BIN> <KERNEL.BIN> <dimos.img> <dimos.hdd> [dimos.iso]\n",
                argv[0]);
        return 2;
    }

    bootloader = read_whole_file(argv[1], &bootloader_size);
    kernel = read_whole_file(argv[2], &kernel_size);
    floppy = read_whole_file(argv[3], &floppy_size);
    hdd = read_whole_file(argv[4], &hdd_size);
    if (bootloader == NULL || kernel == NULL || floppy == NULL || hdd == NULL) {
        return 1;
    }

    printf("DimOS image check\n");
    check_bootloader(bootloader, bootloader_size);
    check_kernel(kernel_size);
    check_geometry(floppy, floppy_size);
    check_installed_bootloader(bootloader, floppy, floppy_size);
    check_hard_disk(floppy, floppy_size, hdd, hdd_size);
    check_kernel_directory_entry(floppy, floppy_size, kernel_size);

    if (argc == 6) {
        size_t iso_size = 0u;
        u8 *iso = read_whole_file(argv[5], &iso_size);

        if (iso != NULL) {
            check_iso(iso, iso_size);
            free(iso);
        }
    }

    free(bootloader);
    free(kernel);
    free(floppy);
    free(hdd);

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All checks passed\n");
    return 0;
}
