; ==================================================================
; DimOS -- the only assembly in the kernel
;
; Three jobs live here, and each one is something C cannot express:
;
;   1. read and write hardware ports (the IN and OUT instructions);
;   2. talk to the video BIOS while the machine is still in real mode:
;      switch to 320x200 graphics and find the 8x8 font;
;   3. switch the processor from 16 bit real mode to 32 bit protected
;      mode and hand over to kernel_main().
;
; The bootloader jumps here in real mode at 2000:0000.
; ==================================================================

[CPU 386]

section .entry

CODE_SELECTOR equ 0x08      ; the code entry is the second table entry
DATA_SELECTOR equ 0x10      ; the data entry is the third
KERNEL_SEGMENT equ 0x2000   ; where the bootloader loaded the kernel

[BITS 16]
global kernel_entry
extern kernel_main

kernel_entry:
    cli
    cld
    mov ax, KERNEL_SEGMENT
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE

    ; --- where is the 8x8 font? ------------------------------------
    ; Video service 1130h, subfunction 02h, answers with the address of
    ; the 8x8 font for characters 0..127 in ES:BP. Segment and offset
    ; are turned into one flat address and stored for gfx.c, so the
    ; kernel does not have to carry its own copy of the font.
    mov ax, 0x1130
    mov bh, 0x02
    int 0x10
    xor eax, eax
    mov ax, es
    shl eax, 4              ; ES * 16
    movzx ebx, bp           ; + BP
    add eax, ebx
    ; The dword inside the brackets is an address-size prefix: it makes the
    ; assembler emit a 32-bit displacement. A 16-bit displacement would ask
    ; the linker for a 16-bit relocation of an address above 64 KiB, which
    ; overflows; the 32-bit form links everywhere. At run time DS is 0x2000,
    ; so the linear address is 0x20000 plus this offset.
    mov [dword bios_font_address - KERNEL_SEGMENT * 16], eax

    ; --- 320 x 200, 256 colours ------------------------------------
    ; Video service 00h, mode 13h: the classic DOS graphics mode. One
    ; byte per pixel, picture at 0xA0000, palette through ports 3C8h.
    mov ax, 0x0013
    int 0x10

    ; --- into protected mode ---------------------------------------
    ; Same address-size prefix as above: 32-bit displacement, so the linker
    ; never sees a 16-bit relocation it cannot fit.
    lgdt [dword gdt_descriptor - KERNEL_SEGMENT * 16]
    mov eax, cr0
    or eax, 1               ; set PE, the protection enable bit
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
    mov esp, 0x00090000     ; the stack grows down from 576 KiB
    xor ebp, ebp

    call kernel_main        ; never returns

.hang:
    cli
    hlt
    jmp .hang

; ------------------------------------------------------------------
; Reading and writing one byte of a hardware port. These are the two
; instructions C has no spelling for; everything else in the kernel
; reaches hardware through them.
; ------------------------------------------------------------------

; u8 port_read_byte(u16 port)
global port_read_byte
port_read_byte:
    mov edx, [esp + 4]      ; the port number is the first argument
    xor eax, eax
    in al, dx               ; read one byte from that port
    ret

; void port_write_byte(u16 port, u8 value)
global port_write_byte
port_write_byte:
    mov edx, [esp + 4]      ; first argument:  the port number
    mov eax, [esp + 8]      ; second argument: the byte to write
    out dx, al
    ret

; ------------------------------------------------------------------
; The global descriptor table.
;
; Protected mode needs a table that describes the memory segments. The
; simplest useful table is two "flat" segments that both start at
; address zero and cover all 4 GiB - then a pointer in C already is the
; physical address and nothing has to be translated.
;
; One entry is eight bytes. They are written out field by field here,
; because that is what the processor reads:
;
;   dw  limit, bits  0..15
;   dw  base,  bits  0..15
;   db  base,  bits 16..23
;   db  access byte: present, privilege ring, segment type
;   db  flags (4 bits) + limit, bits 16..19
;   db  base,  bits 24..31
; ------------------------------------------------------------------

SEGMENT_LIMIT_LOW   equ 0xFFFF  ; with the flags below this means 4 GiB
SEGMENT_BASE_LOW    equ 0x0000  ; the segment starts at address 0
SEGMENT_BASE_MIDDLE equ 0x00
CODE_ACCESS         equ 0x9A    ; present, ring 0, code, readable
DATA_ACCESS         equ 0x92    ; present, ring 0, data, writable
GRANULARITY_FLAGS   equ 0xCF    ; 32 bit, 4 KiB granularity, limit 0xF
SEGMENT_BASE_HIGH   equ 0x00

align 8
gdt_start:
    dq 0                        ; the first entry is always empty
gdt_code:
    dw SEGMENT_LIMIT_LOW
    dw SEGMENT_BASE_LOW
    db SEGMENT_BASE_MIDDLE
    db CODE_ACCESS
    db GRANULARITY_FLAGS
    db SEGMENT_BASE_HIGH
gdt_data:
    dw SEGMENT_LIMIT_LOW
    dw SEGMENT_BASE_LOW
    db SEGMENT_BASE_MIDDLE
    db DATA_ACCESS
    db GRANULARITY_FLAGS
    db SEGMENT_BASE_HIGH
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; size of the table, in bytes, minus one
    dd gdt_start                ; and where the table is in memory

; ------------------------------------------------------------------
; Filled in by kernel_entry with the address of the font that lives in
; the video BIOS. gfx.c reads it through this name.
; ------------------------------------------------------------------

section .data
global bios_font_address
bios_font_address:
    dd 0

section .note.GNU-stack noalloc noexec nowrite progbits
