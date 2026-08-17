; ==================================================================
; DimOS kernel entry and x86 port-I/O primitives
;
; The bootloader enters here in 16-bit real mode at 2000:0000. Only the
; operations that require assembly live in this file: protected-mode setup,
; BSS initialization, and IN/OUT instructions. Kernel policy is in kernel.c.
; ==================================================================

[CPU 386]

section .entry

CODE_SELECTOR equ 0x08
DATA_SELECTOR equ 0x10

[BITS 16]
global kernel_entry
extern kernel_main
extern __bss_start
extern __bss_end

kernel_entry:
    cli
    cld

    ; The binary is loaded at segment 2000h and starts at offset zero.
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE

    ; This operand is a real-mode offset. The GDT itself contains linked
    ; physical addresses because protected-mode segments have base zero.
    lgdt [gdt_descriptor - kernel_entry]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE_SELECTOR:protected_mode_entry

[BITS 32]
protected_mode_entry:
    mov ax, DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x00090000
    xor ebp, ebp

    ; A raw binary does not carry its NOLOAD .bss bytes, so clear them here.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

; cdecl: u8 io_in8(u16 port)
global io_in8
io_in8:
    mov edx, [esp + 4]
    xor eax, eax
    in al, dx
    ret

; cdecl: void io_out8(u16 port, u8 value)
global io_out8
io_out8:
    mov edx, [esp + 4]
    mov eax, [esp + 8]
    out dx, al
    ret

align 8
gdt_start:
    dq 0x0000000000000000       ; null descriptor
    dq 0x00CF9A000000FFFF       ; flat 32-bit code, base 0, limit 4 GiB
    dq 0x00CF92000000FFFF       ; flat 32-bit data, base 0, limit 4 GiB
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

section .note.GNU-stack noalloc noexec nowrite progbits
