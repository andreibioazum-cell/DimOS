OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(kernel_entry)

KERNEL_LOAD_ADDRESS = 0x00020000;
KERNEL_LOADER_LIMIT = 43008;

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
ASSERT(__kernel_memory_end < 0x00030000,
       "kernel and BSS no longer fit in the 2000h segment window")
