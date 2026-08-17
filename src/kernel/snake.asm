; ==================================================================
; Integrated Snake game for the minimal DimOS kernel.
; ==================================================================

SNAKE_MAX_LENGTH equ 512
SNAKE_START_ROW  equ 12
SNAKE_START_COL  equ 42
SNAKE_PLAY_TOP   equ 3
SNAKE_PLAY_ROWS  equ 20
SNAKE_PLAY_COLS  equ 78
SNAKE_TICK_DELAY equ 2

snake_game:
.restart:
    call clear_screen

    mov word [snake_length], 5
    mov word [snake_score], 0
    mov byte [snake_direction], 1       ; 0 up, 1 right, 2 down, 3 left

    mov ah, 0
    int 0x1A
    mov [snake_last_tick], dx
    mov [snake_random_state], dx

    call snake_draw_arena

    push ds
    pop es
    mov ax, SNAKE_START_ROW * 80 + SNAKE_START_COL
    mov di, snake_cells
    mov cx, 5
.initialize_body:
    stosw
    dec ax
    loop .initialize_body

    call snake_draw_body
    call snake_place_food

.main_loop:
    call snake_read_control
    cmp al, 1
    je .exit
    cmp al, 2
    je .restart

    mov ah, 0
    int 0x1A
    mov ax, dx
    sub ax, [snake_last_tick]
    cmp ax, SNAKE_TICK_DELAY
    jb .main_loop
    mov [snake_last_tick], dx

    call snake_move
    jc .game_over
    jmp .main_loop

.game_over:
    mov dh, 12
    mov dl, 25
    call set_cursor
    mov si, snake_game_over_message
    call print_string

.wait_after_loss:
    xor ah, ah
    int 0x16
    cmp al, 27
    je .exit
    cmp al, 13
    je .restart
    cmp al, 'n'
    je .restart
    cmp al, 'N'
    je .restart
    jmp .wait_after_loss

.exit:
    ret

; AL=0 continue, AL=1 leave game, AL=2 restart.
snake_read_control:
    mov ah, 0x01
    int 0x16
    jz .none

    xor ah, ah
    int 0x16

    cmp al, 27
    je .exit
    cmp al, 'n'
    je .restart
    cmp al, 'N'
    je .restart

    cmp al, 'w'
    je .up
    cmp al, 'W'
    je .up
    cmp ah, 0x48
    je .up

    cmp al, 'd'
    je .right
    cmp al, 'D'
    je .right
    cmp ah, 0x4D
    je .right

    cmp al, 's'
    je .down
    cmp al, 'S'
    je .down
    cmp ah, 0x50
    je .down

    cmp al, 'a'
    je .left
    cmp al, 'A'
    je .left
    cmp ah, 0x4B
    je .left
    jmp .none

.up:
    cmp byte [snake_direction], 2
    je .none
    mov byte [snake_direction], 0
    jmp .none

.right:
    cmp byte [snake_direction], 3
    je .none
    mov byte [snake_direction], 1
    jmp .none

.down:
    cmp byte [snake_direction], 0
    je .none
    mov byte [snake_direction], 2
    jmp .none

.left:
    cmp byte [snake_direction], 1
    je .none
    mov byte [snake_direction], 3

.none:
    xor al, al
    ret
.exit:
    mov al, 1
    ret
.restart:
    mov al, 2
    ret

; Moves one cell. CF=1 means collision, CF=0 means success.
snake_move:
    mov bx, [snake_cells]
    mov al, [snake_direction]
    test al, al
    jz .move_up
    cmp al, 1
    je .move_right
    cmp al, 2
    je .move_down

.move_left:
    dec bx
    jmp .target_ready
.move_up:
    sub bx, 80
    jmp .target_ready
.move_right:
    inc bx
    jmp .target_ready
.move_down:
    add bx, 80

.target_ready:
    mov [snake_new_head], bx
    call snake_get_cell
    cmp al, '@'
    je .ate_food
    cmp al, ' '
    jne .collision

    mov byte [snake_grew], 0
    jmp .erase_tail

.ate_food:
    mov byte [snake_grew], 1
    cmp word [snake_length], SNAKE_MAX_LENGTH
    jae .collision
    inc word [snake_length]
    add word [snake_score], 10

.erase_tail:
    cmp byte [snake_grew], 1
    je .shift_body

    mov si, [snake_length]
    dec si
    shl si, 1
    mov bx, [snake_cells + si]
    mov dl, ' '
    mov dh, 0x07
    call snake_draw_cell

.shift_body:
    mov cx, [snake_length]
    dec cx
    jcxz .store_head
    mov si, cx
    shl si, 1

.shift_next:
    mov ax, [snake_cells + si - 2]
    mov [snake_cells + si], ax
    sub si, 2
    loop .shift_next

.store_head:
    mov bx, [snake_cells]
    mov dl, 'o'
    mov dh, 0x0A
    call snake_draw_cell

    mov bx, [snake_new_head]
    mov [snake_cells], bx
    mov dl, 'O'
    mov dh, 0x0F
    call snake_draw_cell

    cmp byte [snake_grew], 1
    jne .success
    call snake_draw_score
    call snake_place_food

.success:
    clc
    ret
.collision:
    stc
    ret

snake_draw_arena:
    mov dh, 0
    mov dl, 0
    call set_cursor
    mov si, snake_title
    call print_string

    mov dh, 1
    mov dl, 0
    call set_cursor
    mov si, snake_controls
    call print_string

    call snake_draw_score

    mov bx, 2 * 80
    mov cx, 80
.draw_top_border:
    mov dl, '#'
    mov dh, 0x09
    call snake_draw_cell
    inc bx
    loop .draw_top_border

    mov bx, 23 * 80
    mov cx, 80
.draw_bottom_border:
    mov dl, '#'
    mov dh, 0x09
    call snake_draw_cell
    inc bx
    loop .draw_bottom_border

    mov bx, SNAKE_PLAY_TOP * 80
    mov cx, SNAKE_PLAY_ROWS
.draw_side_borders:
    mov dl, '#'
    mov dh, 0x09
    call snake_draw_cell
    add bx, 79
    call snake_draw_cell
    sub bx, 79
    add bx, 80
    loop .draw_side_borders
    ret

snake_draw_body:
    mov si, snake_cells
    mov cx, [snake_length]
    xor bp, bp
.next_cell:
    mov bx, [si]
    mov dl, 'o'
    mov dh, 0x0A
    test bp, bp
    jnz .draw
    mov dl, 'O'
    mov dh, 0x0F
.draw:
    call snake_draw_cell
    add si, 2
    inc bp
    loop .next_cell
    ret

snake_draw_score:
    mov dh, 0
    mov dl, 65
    call set_cursor
    mov si, snake_score_label
    call print_string
    mov ax, [snake_score]
    call print_unsigned
    ret

snake_place_food:
    push ax
    push bx
    push cx
    push dx
    push si

.try_again:
    call snake_random
    xor dx, dx
    mov cx, SNAKE_PLAY_ROWS
    div cx
    add dx, SNAKE_PLAY_TOP
    mov [snake_food_row], dx

    call snake_random
    xor dx, dx
    mov cx, SNAKE_PLAY_COLS
    div cx
    inc dx
    mov si, dx

    mov ax, [snake_food_row]
    mov bx, 80
    mul bx
    add ax, si
    mov bx, ax

    call snake_get_cell
    cmp al, ' '
    jne .try_again

    mov dl, '@'
    mov dh, 0x0C
    call snake_draw_cell

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Returns a small pseudo-random word in AX.
snake_random:
    push bx
    push dx
    mov ax, [snake_random_state]
    mov bx, 25173
    mul bx
    add ax, 13849
    mov [snake_random_state], ax
    pop dx
    pop bx
    ret

; BX = text cell index, DL = character, DH = VGA attribute.
snake_draw_cell:
    push ax
    push di
    push es
    mov ax, 0xB800
    mov es, ax
    mov di, bx
    shl di, 1
    mov al, dl
    mov ah, dh
    mov [es:di], ax
    pop es
    pop di
    pop ax
    ret

; BX = text cell index, returns character in AL.
snake_get_cell:
    push di
    push es
    mov ax, 0xB800
    mov es, ax
    mov di, bx
    shl di, 1
    mov al, [es:di]
    pop es
    pop di
    ret

snake_title db 'DimOS Snake', 0
snake_controls db 'WASD/arrows: move   N: new game   ESC: command line', 0
snake_score_label db 'Score: ', 0
snake_game_over_message db ' GAME OVER - ENTER/N: retry, ESC: exit ', 0

snake_length       dw 5
snake_score        dw 0
snake_last_tick    dw 0
snake_random_state dw 1
snake_food_row     dw 0
snake_new_head     dw 0
snake_direction    db 1
snake_grew         db 0
snake_cells        times SNAKE_MAX_LENGTH dw 0
