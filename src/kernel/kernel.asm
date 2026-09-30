; ==================================================================
; DimOS x86-64 entry
;
; BIOS starts us in 16-bit real mode. This file does the jobs freestanding C
; cannot do: obtain a BIOS font, select the best hardware framebuffer, enable
; A20, construct identity-mapped page tables, enter IA-32e long mode, and
; provide the IN/OUT port helpers used by the C kernel.
;
; Preferred video path: VBE 2.0 mode 111h, 640x480 RGB565 linear framebuffer.
; Compatibility path: VGA mode 13h, 320x200 indexed framebuffer at A0000h.
; The bootloader jumps here at 2000:0000.
; ==================================================================

[CPU x86-64]

section .entry

KERNEL_SEGMENT equ 0x2000
KERNEL_BASE equ 0x00020000

CODE32_SELECTOR equ 0x08
DATA_SELECTOR equ 0x10
CODE64_SELECTOR equ 0x18

PML4_ADDRESS equ 0x00001000
PDPT_ADDRESS equ 0x00002000
PD0_ADDRESS equ 0x00003000
PAGE_TABLE_BYTES equ 0x00006000       ; PML4 + PDPT + four page directories
PAGE_LARGE_PRESENT_WRITE equ 0x00000083

VBE_MODE equ 0x0111                   ; 640 x 480 x 16, RGB565 on VBE 2.0+
VBE_MODE_LINEAR equ (0x4000 | VBE_MODE)
VBE_ATTRIBUTE_LINEAR equ 0x0080

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

    ; Ask the video BIOS for its built-in 8x8 glyph table before leaving real
    ; mode. gfx.c uses this only when no FONT.TTF is present on the boot disk.
    mov ax, 0x1130
    mov bh, 0x02
    int 0x10
    xor eax, eax
    mov ax, es
    shl eax, 4
    movzx ebx, bp
    add eax, ebx
    mov ax, KERNEL_SEGMENT
    mov ds, ax
    mov [dword bios_font_address - KERNEL_BASE], eax

    ; Probe a standard VBE 2.0 linear mode. The mode information block gives
    ; us the physical framebuffer address and the hardware pitch; no address
    ; is guessed. If any requirement is missing, old VGA mode 13h is selected.
    mov ax, KERNEL_SEGMENT
    mov es, ax
    mov di, vbe_mode_info - KERNEL_BASE
    mov ax, 0x4F01
    mov cx, VBE_MODE
    int 0x10
    cmp ax, 0x004F
    jne .use_vga
    test word [es:di + 0], VBE_ATTRIBUTE_LINEAR
    jz .use_vga
    cmp word [es:di + 18], 640
    jne .use_vga
    cmp word [es:di + 20], 480
    jne .use_vga
    cmp byte [es:di + 25], 16
    jne .use_vga
    cmp byte [es:di + 27], 6         ; direct-colour memory model
    jne .use_vga
    cmp byte [es:di + 31], 5         ; RGB565 channel masks
    jne .use_vga
    cmp byte [es:di + 32], 11
    jne .use_vga
    cmp byte [es:di + 33], 6
    jne .use_vga
    cmp byte [es:di + 34], 5
    jne .use_vga
    cmp byte [es:di + 35], 5
    jne .use_vga
    cmp byte [es:di + 36], 0
    jne .use_vga
    cmp dword [es:di + 40], 0
    je .use_vga

    mov ax, 0x4F02
    mov bx, VBE_MODE_LINEAR
    int 0x10
    cmp ax, 0x004F
    jne .use_vga

    mov ax, KERNEL_SEGMENT
    mov ds, ax
    mov es, ax
    mov di, vbe_mode_info - KERNEL_BASE
    mov byte [dword video_backend - KERNEL_BASE], 1
    mov eax, [es:di + 40]
    mov [dword video_framebuffer_address - KERNEL_BASE], eax
    mov ax, [es:di + 16]
    mov [dword video_pitch - KERNEL_BASE], ax
    mov ax, [es:di + 18]
    mov [dword video_width - KERNEL_BASE], ax
    mov ax, [es:di + 20]
    mov [dword video_height - KERNEL_BASE], ax
    mov al, [es:di + 25]
    mov [dword video_bits_per_pixel - KERNEL_BASE], al
    jmp .video_ready

.use_vga:
    mov ax, 0x0013
    int 0x10
    mov ax, KERNEL_SEGMENT
    mov ds, ax
    mov byte [dword video_backend - KERNEL_BASE], 0
    mov dword [dword video_framebuffer_address - KERNEL_BASE], 0x000A0000
    mov word [dword video_pitch - KERNEL_BASE], 320
    mov word [dword video_width - KERNEL_BASE], 320
    mov word [dword video_height - KERNEL_BASE], 200
    mov byte [dword video_bits_per_pixel - KERNEL_BASE], 8

.video_ready:
    ; Fast A20 gate. Long mode can now address the RAM disk and font workspace
    ; above one megabyte without wrapping back into conventional memory.
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    ; Refuse to execute the long-mode transition on a CPU that does not expose
    ; the architectural x86-64 bit. Every supported DimOS target has CPUID.
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .unsupported
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)
    jz .unsupported

    lgdt [dword gdt_descriptor - KERNEL_BASE]
    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax
    jmp dword CODE32_SELECTOR:protected_mode_entry

.unsupported:
    ; A visible red screen is preferable to silently triple-faulting on an old
    ; 32-bit processor. No BIOS calls are needed after this point.
    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov al, 4
    mov cx, 32000
    rep stosw
.hang16:
    hlt
    jmp .hang16

[BITS 32]
protected_mode_entry:
    mov ax, DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x00090000

    ; Six pages identity-map the complete low 4 GiB with 2 MiB pages. Mapping
    ; the whole 32-bit physical range matters because PCI/VBE framebuffers are
    ; commonly placed near E0000000h. The tables occupy 1000h..6FFFh, memory
    ; no longer needed by BIOS after the switch.
    mov edi, PML4_ADDRESS
    xor eax, eax
    mov ecx, PAGE_TABLE_BYTES / 4
    rep stosd

    mov dword [PML4_ADDRESS], PDPT_ADDRESS | 0x03
    mov dword [PDPT_ADDRESS + 0], (PD0_ADDRESS + 0x0000) | 0x03
    mov dword [PDPT_ADDRESS + 8], (PD0_ADDRESS + 0x1000) | 0x03
    mov dword [PDPT_ADDRESS + 16], (PD0_ADDRESS + 0x2000) | 0x03
    mov dword [PDPT_ADDRESS + 24], (PD0_ADDRESS + 0x3000) | 0x03

    mov edi, PD0_ADDRESS
    mov eax, PAGE_LARGE_PRESENT_WRITE
    mov ecx, 2048                     ; 2048 * 2 MiB = 4 GiB
.map_large_page:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x00200000
    add edi, 8
    loop .map_large_page

    mov eax, cr4
    or eax, (1 << 5)                  ; CR4.PAE
    mov cr4, eax

    mov eax, PML4_ADDRESS
    mov cr3, eax

    mov ecx, 0xC0000080               ; IA32_EFER
    rdmsr
    or eax, (1 << 8)                  ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, 0x80000000                ; CR0.PG
    mov cr0, eax
    jmp CODE64_SELECTOR:long_mode_entry

[BITS 64]
long_mode_entry:
    mov ax, DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x0000000000090000
    and rsp, -16
    xor ebp, ebp

    call kernel_main

.hang64:
    cli
    hlt
    jmp .hang64

; ------------------------------------------------------------------
; Hardware port access, using the x86-64 System V calling convention.
; ------------------------------------------------------------------

global port_read_byte
port_read_byte:
    mov edx, edi                       ; first argument: port
    xor eax, eax
    in al, dx
    ret

global port_write_byte
port_write_byte:
    mov edx, edi                       ; first argument: port
    mov eax, esi                       ; second argument: value
    out dx, al
    ret

; ------------------------------------------------------------------
; GDT: temporary 32-bit code, flat data, then a 64-bit code segment.
; ------------------------------------------------------------------

align 8
gdt_start:
    dq 0

gdt_code32:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00

gdt_code64:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xAF                           ; G=1, L=1, D=0
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

section .data
align 16

global bios_font_address
bios_font_address:
    dd 0

global video_backend
global video_framebuffer_address
global video_pitch
global video_width
global video_height
global video_bits_per_pixel
video_backend:
    db 0
video_framebuffer_address:
    dd 0x000A0000
video_pitch:
    dw 320
video_width:
    dw 320
video_height:
    dw 200
video_bits_per_pixel:
    db 8

align 16
vbe_mode_info:
    times 256 db 0

section .note.GNU-stack noalloc noexec nowrite progbits
