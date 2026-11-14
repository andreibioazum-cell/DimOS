OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(kernel_entry)

KERNEL_LOAD_ADDRESS = 0x00020000;

/* The BIOS loader follows KERNEL.BIN's FAT chain into 2000:0000. Its loaded
 * sections must stay below the FAT12 data window at 0x30000. BSS is NOLOAD
 * and lives at 1 MiB, away from the disk cache and graphics buffers. */
KERNEL_LOADER_LIMIT = 64512;

PHDRS
{
    code PT_LOAD FLAGS(5); /* read + execute */
    data PT_LOAD FLAGS(6); /* read + write */
}

SECTIONS
{
    . = KERNEL_LOAD_ADDRESS;
    __kernel_start = .;

    .text ALIGN(16) :
    {
        KEEP(*(.entry))
        *(.text .text.*)
    } :code

    .rodata ALIGN(16) :
    {
        *(.rodata .rodata.*)
    } :code

    .data ALIGN(16) :
    {
        *(.data .data.*)
    } :data

    __kernel_load_end = .;

    /* Protected mode addresses this region directly. Keeping BSS out of the
     * 64 KiB loader window keeps the protected-mode image compact without
     * stealing space from KERNEL.BIN. */
    . = 0x00100000;
    .bss ALIGN(16) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    __kernel_memory_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}

ASSERT(__kernel_load_end - __kernel_start <= KERNEL_LOADER_LIMIT,
       "KERNEL.BIN exceeds the bootloader's load window")
ASSERT(__bss_start >= 0x00100000 && __kernel_memory_end < 0x00200000,
       "kernel BSS no longer fits in its 1 MiB runtime window")
