; ==================================================================
; DimOS real-mode entry and 64-bit long-mode kernel
;
; BIOS starts us in 16-bit real mode. This file obtains the BIOS font, selects
; the VBE framebuffer, enters a temporary 32-bit protected mode, identity-maps
; the complete first 4 GiB with 2 MiB pages, enables IA-32e long mode and calls
; the x86-64 C kernel. Physical devices remain directly addressable.
; ==================================================================

[CPU x64]

section .entry

KERNEL_SEGMENT equ 0x2000
KERNEL_BASE equ 0x00020000

CODE32_SELECTOR equ 0x08
DATA_SELECTOR equ 0x10
CODE64_SELECTOR equ 0x18

PML4_ADDRESS equ 0x00090000
PDPT_ADDRESS equ 0x00091000
PAGE_DIRECTORY_ADDRESS equ 0x00092000
PAGE_TABLE_BYTES equ 0x00006000

VBE_FALLBACK_MODE equ 0x0111          ; 640 x 480 x 16 RGB565
VBE_MODE_LINEAR_BIT equ 0x4000
VBE_REQUIRED_ATTRIBUTES equ 0x0091    ; supported + graphics + linear FB
%ifdef DIMOS_EMULATOR
; This is the *physical* VBE mode for the browser package.  It must match
; the complete display, rather than merely be a small viewport inside a
; 1920x1080 mode: v86 exposes the unused area as the blue host background.
; 640x480 is available on conservative VBE BIOSes and cuts the browser's
; framebuffer traffic to less than fifteen percent of the PC profile.
VBE_TARGET_WIDTH equ 640
VBE_TARGET_HEIGHT equ 480
%else
VBE_TARGET_WIDTH equ 1920
VBE_TARGET_HEIGHT equ 1080
%endif

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
    a32 mov [bios_font_address - KERNEL_BASE], eax

    ; Ask VBE for its complete mode list instead of relying on a vendor mode
    ; number. Mode IDs for 1920x1080 are deliberately not standardized; the
    ; dimensions, direct-colour masks and physical framebuffer are.
    mov ax, KERNEL_SEGMENT
    mov es, ax
    mov edi, vbe_controller_info - KERNEL_BASE
    mov dword [es:di], 0x32454256    ; "VBE2" requests the VBE 2.0 block
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .try_vbe_fallback
    mov ax, KERNEL_SEGMENT
    mov es, ax
    mov edi, vbe_controller_info - KERNEL_BASE
    cmp dword [es:di], 0x41534556    ; BIOS answered "VESA"
    jne .try_vbe_fallback
    cmp word [es:di + 4], 0x0200
    jb .try_vbe_fallback

    mov si, [es:di + 14]            ; far pointer to zero-terminated mode IDs
    mov ax, [es:di + 16]
    mov fs, ax

.find_hd_mode:
    mov cx, [fs:si]
    add si, 2
    cmp cx, 0xFFFF
    je .try_vbe_fallback
    push fs
    push si
    push cx
    mov ax, KERNEL_SEGMENT
    mov es, ax
    mov edi, vbe_mode_info - KERNEL_BASE
    mov ax, 0x4F01
    int 0x10
    pop cx
    pop si
    pop fs
    cmp ax, 0x004F
    jne .find_hd_mode
    mov ax, [es:di + 0]
    and ax, VBE_REQUIRED_ATTRIBUTES
    cmp ax, VBE_REQUIRED_ATTRIBUTES
    jne .find_hd_mode
    cmp word [es:di + 18], VBE_TARGET_WIDTH
    jne .find_hd_mode
    cmp word [es:di + 20], VBE_TARGET_HEIGHT
    jne .find_hd_mode
    cmp byte [es:di + 25], 32
    jne .find_hd_mode
    cmp byte [es:di + 27], 6        ; direct-colour memory model
    jne .find_hd_mode
    cmp byte [es:di + 31], 8        ; XRGB8888 channel masks
    jne .find_hd_mode
    cmp byte [es:di + 32], 16
    jne .find_hd_mode
    cmp byte [es:di + 33], 8
    jne .find_hd_mode
    cmp byte [es:di + 34], 8
    jne .find_hd_mode
    cmp byte [es:di + 35], 8
    jne .find_hd_mode
    cmp byte [es:di + 36], 0
    jne .find_hd_mode
    cmp dword [es:di + 40], 0
    je .find_hd_mode

    mov bx, cx
    or bx, VBE_MODE_LINEAR_BIT
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .try_vbe_fallback
    jmp .store_vbe_mode

.try_vbe_fallback:
    ; Old adapters may not expose full HD. Keep a conservative standardized
    ; RGB565 mode before dropping all the way back to indexed VGA mode 13h.
    mov ax, KERNEL_SEGMENT
    mov es, ax
    mov edi, vbe_mode_info - KERNEL_BASE
    mov ax, 0x4F01
    mov cx, VBE_FALLBACK_MODE
    int 0x10
    cmp ax, 0x004F
    jne .use_vga
    mov ax, [es:di + 0]
    and ax, VBE_REQUIRED_ATTRIBUTES
    cmp ax, VBE_REQUIRED_ATTRIBUTES
    jne .use_vga
    cmp word [es:di + 18], 640
    jne .use_vga
    cmp word [es:di + 20], 480
    jne .use_vga
    cmp byte [es:di + 25], 16
    jne .use_vga
    cmp byte [es:di + 27], 6
    jne .use_vga
    cmp byte [es:di + 31], 5
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
    mov bx, VBE_FALLBACK_MODE | VBE_MODE_LINEAR_BIT
    int 0x10
    cmp ax, 0x004F
    jne .use_vga

.store_vbe_mode:
    mov ax, KERNEL_SEGMENT
    mov ds, ax
    mov es, ax
    mov edi, vbe_mode_info - KERNEL_BASE
    a32 mov byte [video_backend - KERNEL_BASE], 1
    mov eax, [es:di + 40]
    a32 mov [video_framebuffer_address - KERNEL_BASE], eax
    mov ax, [es:di + 16]
    a32 mov [video_pitch - KERNEL_BASE], ax
    mov ax, [es:di + 18]
    a32 mov [video_width - KERNEL_BASE], ax
    mov ax, [es:di + 20]
    a32 mov [video_height - KERNEL_BASE], ax
    mov al, [es:di + 25]
    a32 mov [video_bits_per_pixel - KERNEL_BASE], al
    jmp .video_ready

.use_vga:
    mov ax, 0x0013
    int 0x10
    mov ax, KERNEL_SEGMENT
    mov ds, ax
    a32 mov byte [video_backend - KERNEL_BASE], 0
    a32 mov dword [video_framebuffer_address - KERNEL_BASE], 0x000A0000
    a32 mov word [video_pitch - KERNEL_BASE], 320
    a32 mov word [video_width - KERNEL_BASE], 320
    a32 mov word [video_height - KERNEL_BASE], 200
    a32 mov byte [video_bits_per_pixel - KERNEL_BASE], 8

.video_ready:
    ; Fast A20 gate. Protected mode can now address the RAM disk, font and
    ; back buffers above one megabyte without wrapping into low memory.
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    a32 lgdt [gdt_descriptor - KERNEL_BASE]
    mov eax, cr0
    or eax, 0x00000001                ; CR0.PE
    mov cr0, eax
    jmp dword CODE32_SELECTOR:protected_mode_entry

[BITS 32]
protected_mode_entry:
    mov ax, DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x0008F000
    xor ebp, ebp
    cld

    ; Six pages describe an identity mapping of 0..4 GiB: one PML4, one PDPT
    ; and four page directories containing 2048 two-megabyte mappings.
    mov edi, PML4_ADDRESS
    xor eax, eax
    mov ecx, PAGE_TABLE_BYTES / 4
    rep stosd
    mov dword [PML4_ADDRESS], PDPT_ADDRESS | 0x03

    mov edi, PDPT_ADDRESS
    mov eax, PAGE_DIRECTORY_ADDRESS | 0x03
    mov ecx, 4
.make_pdpt:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x1000
    add edi, 8
    loop .make_pdpt

    mov edi, PAGE_DIRECTORY_ADDRESS
    mov eax, 0x00000083                 ; present, writable, 2 MiB page
    mov ecx, 2048
.make_pages:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x00200000
    add edi, 8
    loop .make_pages

    mov eax, cr4
    or eax, 1 << 5                      ; CR4.PAE
    mov cr4, eax
    mov eax, PML4_ADDRESS
    mov cr3, eax
    mov ecx, 0xC0000080                 ; IA32_EFER
    rdmsr
    or eax, 1 << 8                      ; EFER.LME
    wrmsr
    mov eax, cr0
    or eax, 1 << 31                     ; CR0.PG (PE is already set)
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
    mov rsp, kernel_stack_top
    xor ebp, ebp
    call kernel_main

.hang64:
    cli
    hlt
    jmp .hang64

; ------------------------------------------------------------------
; Hardware port access, using the x86-64 System V ABI.
; ------------------------------------------------------------------

global port_read_byte
port_read_byte:
    mov edx, edi                        ; first argument: port
    xor eax, eax
    in al, dx
    ret

global port_write_byte
port_write_byte:
    mov edx, edi                        ; first argument: port
    mov eax, esi                        ; second argument: value
    out dx, al
    ret

; ------------------------------------------------------------------
; GDT: transition code, shared data and 64-bit kernel code.
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
    dw 0x0000
    dw 0x0000
    db 0x00
    db 0x9A
    db 0x20                            ; L=1, D=0
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
vbe_controller_info:
    times 512 db 0

align 16
vbe_mode_info:
    times 256 db 0

section .stack nobits alloc write align=16
kernel_stack_bottom:
    resb 16384
kernel_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
