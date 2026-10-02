OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

KERNEL_LOAD_ADDRESS = 0x00020000;

/* The BIOS loader follows KERNEL.BIN's FAT chain into 0x20000. Long mode does
 * not change this compact boot-time window; BSS and the 64-bit stack remain
 * NOLOAD at 1 MiB. */
KERNEL_LOADER_LIMIT = 64512;

PHDRS
{
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
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

    . = 0x00100000;
    .bss ALIGN(16) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    /* Kept outside __bss_start..__bss_end: kernel_main clears BSS while it is
     * already executing on this stack. */
    .stack ALIGN(16) (NOLOAD) :
    {
        *(.stack)
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
       "KERNEL.BIN exceeds the BIOS loader window")
ASSERT(__bss_start >= 0x00100000 && __kernel_memory_end < 0x00200000,
       "kernel BSS and 64-bit stack no longer fit below 2 MiB")
