"""The 8x8 font the test rig copies into the simulated video BIOS ROM.

On a real machine kernel.asm asks the video BIOS for its own 8x8 font
(service 1130h, subfunction 02h) and stores the address in
bios_font_address. The sandbox has no BIOS, so this table is written to
0xE000 instead and bios_font_address is pointed at it.

The glyph shapes are the classic public domain IBM VGA 8x8 set:
each entry is one character, eight rows, top row first, one byte per row,
written here as sixteen hex digits.
"""

ZERO = "0000000000000000"

# Glyphs 0x20 .. 0x7E. Everything else stays blank.
GLYPHS = {
    0x20: ZERO,
    0x21: "183C3C1818001800",
    0x22: "3636000000000000",
    0x23: "36367F367F363600",
    0x24: "0C3E031E301F0C00",
    0x25: "006333180C666300",
    0x26: "1C361C6E3B336E00",
    0x27: "0606030000000000",
    0x28: "180C0606060C1800",
    0x29: "060C1818180C0600",
    0x2A: "00663CFF3C660000",
    0x2B: "000C0C3F0C0C0000",
    0x2C: "00000000000C0C06",
    0x2D: "0000003F00000000",
    0x2E: "00000000000C0C00",
    0x2F: "6030180C06030100",
    0x30: "3E63737B6F673E00",
    0x31: "0C0E0C0C0C0C3F00",
    0x32: "1E33301C06333F00",
    0x33: "1E33301C30331E00",
    0x34: "383C36337F307800",
    0x35: "3F031F3030331E00",
    0x36: "1C06031F33331E00",
    0x37: "3F3330180C0C0C00",
    0x38: "1E33331E33331E00",
    0x39: "1E33333E30180E00",
    0x3A: "000C0C00000C0C00",
    0x3B: "000C0C00000C0C06",
    0x3C: "180C0603060C1800",
    0x3D: "00003F00003F0000",
    0x3E: "060C1830180C0600",
    0x3F: "1E3330180C000C00",
    0x40: "3E637B7B7B031E00",
    0x41: "0C1E33333F333300",
    0x42: "3F66663E66663F00",
    0x43: "3C66030303663C00",
    0x44: "1F36666666361F00",
    0x45: "7F46161E16467F00",
    0x46: "7F46161E16060F00",
    0x47: "3C66030373667C00",
    0x48: "3333333F33333300",
    0x49: "1E0C0C0C0C0C1E00",
    0x4A: "7830303033331E00",
    0x4B: "6766361E36666700",
    0x4C: "0F06060646667F00",
    0x4D: "63777F7F6B636300",
    0x4E: "63676F7B73636300",
    0x4F: "1C36636363361C00",
    0x50: "3F66663E06060F00",
    0x51: "1E3333333B1E3800",
    0x52: "3F66663E36666700",
    0x53: "1E33070E38331E00",
    0x54: "3F2D0C0C0C0C1E00",
    0x55: "3333333333333F00",
    0x56: "33333333331E0C00",
    0x57: "6363636B7F776300",
    0x58: "6363361C1C366300",
    0x59: "3333331E0C0C1E00",
    0x5A: "7F6331184C667F00",
    0x5B: "1E06060606061E00",
    0x5C: "03060C1830604000",
    0x5D: "1E18181818181E00",
    0x5E: "081C366300000000",
    0x5F: "00000000000000FF",
    0x60: "0C0C180000000000",
    0x61: "00001E303E336E00",
    0x62: "0706063E66663B00",
    0x63: "00001E3303331E00",
    0x64: "3830303E33336E00",
    0x65: "00001E333F031E00",
    0x66: "1C36060F06060F00",
    0x67: "00006E33333E301F",
    0x68: "0706366E66666700",
    0x69: "0C000E0C0C0C1E00",
    0x6A: "300030303033331E",
    0x6B: "070666361E366700",
    0x6C: "0E0C0C0C0C0C1E00",
    0x6D: "0000337F7F6B6300",
    0x6E: "00001F3333333300",
    0x6F: "00001E3333331E00",
    0x70: "00003B66663E060F",
    0x71: "00006E33333E3078",
    0x72: "00003B6E66060F00",
    0x73: "00003E031E301F00",
    0x74: "080C3E0C0C2C1800",
    0x75: "0000333333336E00",
    0x76: "00003333331E0C00",
    0x77: "0000636B7F7F3600",
    0x78: "000063361C366300",
    0x79: "00003333333E301F",
    0x7A: "00003F190C263F00",
    0x7B: "380C0C070C0C3800",
    0x7C: "1818180018181800",
    0x7D: "070C0C380C0C0700",
    0x7E: "6E3B000000000000",
}


def _mirror(byte):
    """Flip one row of pixels left-for-right.

    The GLYPHS table stores each row with bit 0 as the leftmost pixel (the
    public-domain font8x8 convention), but the video BIOS ROM font that the
    kernel copies at boot stores bit 7 as the leftmost pixel. Mirroring every
    row makes this fixture byte-identical in shape to what int 10h would have
    returned on real hardware."""
    out = 0
    for bit in range(8):
        if byte & (1 << bit):
            out |= 0x80 >> bit
    return out


def font_bytes():
    """128 glyphs of eight bytes, ready to be written to 0xE000."""
    table = bytearray(128 * 8)
    for code, pattern in GLYPHS.items():
        raw = bytes.fromhex(pattern)
        assert len(raw) == 8, hex(code)
        table[code * 8:code * 8 + 8] = bytes(_mirror(b) for b in raw)
    return bytes(table)
