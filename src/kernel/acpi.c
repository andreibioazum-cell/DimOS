#include "dimos.h"

#ifdef DIMOS_SANDBOX
u8 acpi_power_off(void) {
    return 0u;
}
#else

#define ACPI_HEADER_BYTES 36u
#define ACPI_SMI_ENABLE_LIMIT 100000u
#define ACPI_PM1_CNT_SCI_EN 0x0001u
#define ACPI_PM1_CNT_SLP_TYP_MASK 0x1C00u
#define ACPI_PM1_CNT_SLP_EN 0x2000u

static u8 physical_byte(u32 address) {
    return *(const volatile u8 *)(u32)address;
}

static u16 physical_word(u32 address) {
    return (u16)((u16)physical_byte(address) |
                  ((u16)physical_byte(address + 1u) << 8u));
}

static u32 physical_dword(u32 address) {
    return (u32)physical_byte(address) |
           ((u32)physical_byte(address + 1u) << 8u) |
           ((u32)physical_byte(address + 2u) << 16u) |
           ((u32)physical_byte(address + 3u) << 24u);
}

static u8 signature_is(u32 address, const char signature[4]) {
    u8 index;

    for (index = 0u; index < 4u; ++index) {
        if (physical_byte(address + index) != (u8)signature[index]) {
            return 0u;
        }
    }
    return 1u;
}

static u8 checksum_is_zero(u32 address, u32 length) {
    u32 index;
    u8 sum = 0u;

    for (index = 0u; index < length; ++index) {
        sum = (u8)(sum + physical_byte(address + index));
    }
    return (u8)(sum == 0u);
}

static u8 rsdp_at(u32 address) {
    return (u8)(signature_is(address, "RSD ") != 0u &&
                physical_byte(address + 4u) == (u8)'P' &&
                physical_byte(address + 5u) == (u8)'T' &&
                physical_byte(address + 6u) == (u8)'R' &&
                physical_byte(address + 7u) == (u8)' ' &&
                checksum_is_zero(address, 20u) != 0u);
}

static u32 find_rsdp_in_range(u32 start, u32 end) {
    u32 address;

    for (address = start; address + 20u <= end; address += 16u) {
        if (rsdp_at(address) != 0u) {
            return address;
        }
    }
    return 0u;
}

static u32 find_rsdp(void) {
    const u32 ebda_segment = bios_read_word(0x40Eu);
    const u32 ebda = ebda_segment << 4u;
    u32 address = 0u;

    if (ebda >= 0x80000u && ebda < 0xA0000u) {
        address = find_rsdp_in_range(ebda, ebda + 1024u);
    }
    if (address == 0u) {
        address = find_rsdp_in_range(0xE0000u, 0x100000u);
    }
    return address;
}

static u8 table_is_valid(u32 address, const char signature[4]) {
    u32 length;

    if (address == 0u || address > 0xFFFFF000u ||
        signature_is(address, signature) == 0u) {
        return 0u;
    }
    length = physical_dword(address + 4u);
    if (length < ACPI_HEADER_BYTES || length > 0x00100000u ||
        address > 0xFFFFFFFFu - length) {
        return 0u;
    }
    return checksum_is_zero(address, length);
}

static u32 find_fadt(u32 rsdp) {
    const u32 root = physical_dword(rsdp + 16u);
    u32 length;
    u32 offset;

    if (table_is_valid(root, "RSDT") == 0u) {
        return 0u;
    }
    length = physical_dword(root + 4u);
    if (length < ACPI_HEADER_BYTES) {
        return 0u;
    }
    for (offset = ACPI_HEADER_BYTES; offset + 4u <= length; offset += 4u) {
        const u32 table = physical_dword(root + offset);

        if (table_is_valid(table, "FACP") != 0u) {
            return table;
        }
    }
    return 0u;
}

static u8 aml_integer(u32 *cursor, u32 limit, u32 *value) {
    const u8 opcode = physical_byte(*cursor);

    ++*cursor;
    if (opcode == 0x00u) {
        *value = 0u;
        return 1u;
    }
    if (opcode == 0x01u) {
        *value = 1u;
        return 1u;
    }
    if (opcode == 0xFFu) {
        *value = 0xFFFFFFFFu;
        return 1u;
    }
    if (opcode == 0x0Au && *cursor + 1u <= limit) {
        *value = physical_byte(*cursor);
        ++*cursor;
        return 1u;
    }
    if (opcode == 0x0Bu && *cursor + 2u <= limit) {
        *value = physical_word(*cursor);
        *cursor += 2u;
        return 1u;
    }
    if (opcode == 0x0Cu && *cursor + 4u <= limit) {
        *value = physical_dword(*cursor);
        *cursor += 4u;
        return 1u;
    }
    if (opcode == 0x0Eu && *cursor + 8u <= limit) {
        if (physical_dword(*cursor + 4u) != 0u) {
            return 0u;
        }
        *value = physical_dword(*cursor);
        *cursor += 8u;
        return 1u;
    }
    return 0u;
}

static u8 aml_package_length(u32 address, u32 limit, u32 *length,
                             u8 *encoded_bytes) {
    const u8 first = physical_byte(address);
    const u8 follow = (u8)(first >> 6u);
    u8 index;

    if ((u32)address + 1u + follow > limit || follow > 3u) {
        return 0u;
    }
    if (follow == 0u) {
        *length = (u32)(first & 0x3Fu);
        *encoded_bytes = 1u;
        return 1u;
    }
    *length = (u32)(first & 0x0Fu);
    for (index = 0u; index < follow; ++index) {
        *length |= (u32)physical_byte(address + 1u + index)
                   << (4u + (u32)index * 8u);
    }
    *encoded_bytes = (u8)(follow + 1u);
    return 1u;
}

static u8 find_s5_types(u32 dsdt, u32 *sleep_a, u32 *sleep_b) {
    const u32 length = physical_dword(dsdt + 4u);
    const u32 end = dsdt + length;
    u32 offset;

    for (offset = ACPI_HEADER_BYTES; offset + 8u < length; ++offset) {
        u32 name;
        u32 package_opcode;
        u32 package_length;
        u32 package_end;
        u32 cursor;
        u32 value_a;
        u32 value_b;
        u8 encoded_bytes;
        u8 elements;

        if (physical_byte(dsdt + offset) != 0x08u) {
            continue;
        }
        name = dsdt + offset + 1u;
        if (physical_byte(name) == 0x5Cu) {
            ++name;
        }
        if (physical_byte(name) != (u8)'_' ||
            physical_byte(name + 1u) != (u8)'S' ||
            physical_byte(name + 2u) != (u8)'5' ||
            physical_byte(name + 3u) != (u8)'_') {
            continue;
        }
        package_opcode = name + 4u;
        if (physical_byte(package_opcode) != 0x12u || package_opcode + 2u >= end ||
            aml_package_length(package_opcode + 1u, end, &package_length,
                               &encoded_bytes) == 0u) {
            continue;
        }
        package_end = package_opcode + 1u + package_length;
        cursor = package_opcode + 1u + encoded_bytes;
        if (package_length < encoded_bytes || package_end > end ||
            cursor >= package_end) {
            continue;
        }
        elements = physical_byte(cursor);
        ++cursor;
        if (elements < 2u ||
            aml_integer(&cursor, package_end, &value_a) == 0u ||
            aml_integer(&cursor, package_end, &value_b) == 0u ||
            value_a > 7u || value_b > 7u) {
            continue;
        }
        *sleep_a = value_a;
        *sleep_b = value_b;
        return 1u;
    }
    return 0u;
}

static u16 acpi_control_port(u32 fadt, u32 table_length, u32 legacy_offset,
                             u32 extended_offset) {
    u32 address;

    if (legacy_offset + 4u <= table_length) {
        address = physical_dword(fadt + legacy_offset);
        if (address != 0u && address <= 0xFFFFu) {
            return (u16)address;
        }
    }
    if (extended_offset + 12u <= table_length &&
        physical_byte(fadt + extended_offset) == 1u &&
        physical_byte(fadt + extended_offset + 1u) >= 16u) {
        address = physical_dword(fadt + extended_offset + 4u);
        if (physical_dword(fadt + extended_offset + 8u) == 0u &&
            address != 0u && address <= 0xFFFFu) {
            return (u16)address;
        }
    }
    return 0u;
}

u8 acpi_power_off(void) {
    const u32 rsdp = find_rsdp();
    const u32 fadt = (rsdp != 0u) ? find_fadt(rsdp) : 0u;
    u32 fadt_length;
    u32 dsdt;
    u32 sleep_a;
    u32 sleep_b;
    u32 guard;
    u16 pm1a;
    u16 pm1b;
    u16 control;
    u32 smi_command;
    u8 acpi_enable;

    if (fadt == 0u) {
        return 0u;
    }
    fadt_length = physical_dword(fadt + 4u);
    if (fadt_length < 90u) {
        return 0u;
    }
    dsdt = physical_dword(fadt + 40u);
    if (dsdt == 0u && fadt_length >= 148u &&
        physical_dword(fadt + 144u) == 0u) {
        dsdt = physical_dword(fadt + 140u);
    }
    if (table_is_valid(dsdt, "DSDT") == 0u ||
        find_s5_types(dsdt, &sleep_a, &sleep_b) == 0u) {
        return 0u;
    }

    pm1a = acpi_control_port(fadt, fadt_length, 64u, 172u);
    pm1b = acpi_control_port(fadt, fadt_length, 68u, 184u);
    if (pm1a == 0u || port_read_word(pm1a) == 0xFFFFu) {
        return 0u;
    }

    smi_command = physical_dword(fadt + 48u);
    acpi_enable = physical_byte(fadt + 52u);
    if ((port_read_word(pm1a) & ACPI_PM1_CNT_SCI_EN) == 0u &&
        smi_command != 0u && smi_command <= 0xFFFFu && acpi_enable != 0u) {
        port_write_byte((u16)smi_command, acpi_enable);
        for (guard = 0u; guard < ACPI_SMI_ENABLE_LIMIT; ++guard) {
            if ((port_read_word(pm1a) & ACPI_PM1_CNT_SCI_EN) != 0u) {
                break;
            }
        }
        if (guard == ACPI_SMI_ENABLE_LIMIT) {
            return 0u;
        }
    }

    control = port_read_word(pm1a);
    control = (u16)((control & (u16)~(ACPI_PM1_CNT_SLP_TYP_MASK |
                                      ACPI_PM1_CNT_SLP_EN)) |
                    (u16)(sleep_a << 10u) | ACPI_PM1_CNT_SLP_EN);
    if (pm1b != 0u && port_read_word(pm1b) != 0xFFFFu) {
        u16 control_b = port_read_word(pm1b);

        control_b = (u16)((control_b & (u16)~(ACPI_PM1_CNT_SLP_TYP_MASK |
                                               ACPI_PM1_CNT_SLP_EN)) |
                          (u16)(sleep_b << 10u) | ACPI_PM1_CNT_SLP_EN);
        port_write_word(pm1b, control_b);
    }
    port_write_word(pm1a, control);
    return 1u;
}
#endif
