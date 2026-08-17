; ==================================================================
; DimOS -- minimal command line and Snake
; The bootloader loads this flat 16-bit kernel at 2000:0000.
; ==================================================================

[BITS 16]
[ORG 0x0000]
[CPU 8086]

section .text

start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE
    sti
    cld

    call clear_screen
    mov si, welcome_message
    call print_string

shell_loop:
    mov si, prompt
    call print_string

    mov di, command_buffer
    mov cx, COMMAND_MAX_LENGTH
    call read_line

    mov si, command_buffer
    call uppercase_string
    cmp byte [command_buffer], 0
    je shell_loop

    mov si, command_buffer
    mov di, snake_command
    call strings_equal
    jc run_snake

    mov si, unknown_command_message
    call print_string
    jmp shell_loop

run_snake:
    call snake_game
    call clear_screen
    mov si, returned_message
    call print_string
    jmp shell_loop

; ------------------------------------------------------------------
; BIOS text helpers
; ------------------------------------------------------------------

clear_screen:
    mov ax, 0x0003
    int 0x10
    ret

; DS:SI -> zero-terminated string
print_string:
    push ax
    push bx
.print_next:
    lodsb
    test al, al
    jz .done
    call print_char
    jmp .print_next
.done:
    pop bx
    pop ax
    ret

; AL -> character
print_char:
    push ax
    push bx
    mov ah, 0x0E
    xor bh, bh
    mov bl, 0x07
    int 0x10
    pop bx
    pop ax
    ret

print_newline:
    push ax
    mov al, 13
    call print_char
    mov al, 10
    call print_char
    pop ax
    ret

; DH = row, DL = column
set_cursor:
    push ax
    push bx
    mov ah, 0x02
    xor bh, bh
    int 0x10
    pop bx
    pop ax
    ret

; AX -> unsigned decimal
print_unsigned:
    push ax
    push bx
    push cx
    push dx

    xor cx, cx
    mov bx, 10
    test ax, ax
    jnz .collect_digits

    mov al, '0'
    call print_char
    jmp .done

.collect_digits:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .collect_digits

.print_digits:
    pop dx
    mov al, dl
    add al, '0'
    call print_char
    loop .print_digits

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Reads at most CX characters into DS:DI, echoes them, and adds NUL.
read_line:
    push ax
    push bx
    push cx
    push dx
    push di

    xor bx, bx
.read_key:
    xor ah, ah
    int 0x16

    cmp al, 13
    je .finish
    cmp al, 8
    je .backspace
    cmp al, 32
    jb .read_key
    cmp al, 126
    ja .read_key
    cmp bx, cx
    jae .read_key

    stosb
    inc bx
    call print_char
    jmp .read_key

.backspace:
    test bx, bx
    jz .read_key
    dec bx
    dec di
    mov al, 8
    call print_char
    mov al, ' '
    call print_char
    mov al, 8
    call print_char
    jmp .read_key

.finish:
    mov byte [di], 0
    call print_newline

    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Converts DS:SI to uppercase in place.
uppercase_string:
    push ax
    push si
.next:
    mov al, [si]
    test al, al
    jz .done
    cmp al, 'a'
    jb .skip
    cmp al, 'z'
    ja .skip
    sub byte [si], 32
.skip:
    inc si
    jmp .next
.done:
    pop si
    pop ax
    ret

; DS:SI and DS:DI are equal -> CF=1, otherwise CF=0.
strings_equal:
    push ax
    push si
    push di
.compare:
    mov al, [si]
    cmp al, [di]
    jne .different
    test al, al
    jz .equal
    inc si
    inc di
    jmp .compare
.equal:
    stc
    jmp .done
.different:
    clc
.done:
    pop di
    pop si
    pop ax
    ret

%include "src/kernel/snake.asm"

section .data

COMMAND_MAX_LENGTH equ 31

welcome_message db 'DimOS command line', 13, 10
                db 'Type SNAKE to start the game.', 13, 10, 13, 10, 0
returned_message db 'Back at the command line.', 13, 10
                 db 'Type SNAKE to play again.', 13, 10, 13, 10, 0
unknown_command_message db 'Unknown command. The only command is SNAKE.', 13, 10, 0
prompt db '> ', 0
snake_command db 'SNAKE', 0

command_buffer times COMMAND_MAX_LENGTH + 1 db 0

kernel_end:
