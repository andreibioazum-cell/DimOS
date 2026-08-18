; ==================================================================
; DimOS -- FAT12 bootloader
; Copyright (C) 2025 PRoX2011
;
; Loads KERNEL.BIN and jumps to 2000:0000.
; Uses the BIOS drive number in DL and prefers LBA (int 13h AH=42h)
; so floppy, El Torito CD and small HDD images all work in SeaBIOS/v86.
; ==================================================================

[BITS 16]
[ORG 0x0000]
[CPU 8086]

        jmp short main
        nop

bpbOEM               DB "DIMOS   "
bpbBytesPerSector    DW 512
bpbSectorsPerCluster DB 1
bpbReservedSectors   DW 1
bpbNumberOfFATs      DB 2
bpbRootEntries       DW 224
bpbTotalSectors      DW 2880
bpbMedia             DB 0xf0
bpbSectorsPerFAT     DW 9
bpbSectorsPerTrack   DW 18
bpbHeadsPerCylinder  DW 2
bpbHiddenSectors     DD 0
bpbTotalSectorsBig   DD 0
bsDriveNumber        DB 0
bsUnused             DB 0
bsExtBootSignature   DB 0x29
bsSerialNumber       DD 0x00000000
bsVolumeLabel        DB "DIMOS      "
bsFileSystem         DB "FAT12   "

main:
        cli
        mov ax, 0x07C0
        mov ds, ax
        xor ax, ax
        mov ss, ax
        mov sp, 0x7C00
        sti

        ; Never ignore the unit BIOS booted from. Hardcoding 0 makes
        ; SeaBIOS print "could not read the boot disk" on CD/HDD.
        mov [bsDriveNumber], dl

        xor ax, ax
        int 0x13
        call detect_lba

        mov si, msgLoading
        call Print

LOAD_ROOT:
        xor cx, cx
        xor dx, dx
        mov ax, 0x0020
        mul WORD [bpbRootEntries]
        div WORD [bpbBytesPerSector]
        xchg ax, cx
        mov al, BYTE [bpbNumberOfFATs]
        mul WORD [bpbSectorsPerFAT]
        add ax, WORD [bpbReservedSectors]
        mov WORD [datasector], ax
        add WORD [datasector], cx

        push ax
        mov ax, 0x1000
        mov es, ax
        xor bx, bx
        pop ax
        call ReadSectors

        mov cx, WORD [bpbRootEntries]
        xor di, di
.LOOP:
        push cx
        mov cx, 0x000B
        mov si, ImageName
        push di
        rep cmpsb
        pop di
        je LOAD_FAT
        pop cx
        add di, 0x0020
        loop .LOOP
        mov si, msgKernel
        jmp FAIL

LOAD_FAT:
        mov dx, WORD [di + 0x001A]
        mov WORD [cluster], dx
        xor ax, ax
        mov al, BYTE [bpbNumberOfFATs]
        mul WORD [bpbSectorsPerFAT]
        mov cx, ax
        mov ax, 0x07C0
        mov es, ax
        mov bx, 0x0200
        mov ax, WORD [bpbReservedSectors]
        call ReadSectors

        mov ax, WORD [datasector]
        mov cx, 0x0040
        xor bx, bx
        mov dx, 0x3000
        mov es, dx
        call ReadSectors

        mov ax, 0x2000
        mov es, ax
        xor bx, bx
        push bx

LOAD_IMAGE:
        mov ax, WORD [cluster]
        pop bx
        call ClusterLBA
        xor cx, cx
        mov cl, BYTE [bpbSectorsPerCluster]
        call ReadSectors
        push bx
        mov ax, WORD [cluster]
        mov cx, ax
        mov dx, ax
        shr dx, 1
        add cx, dx
        mov bx, 0x0200
        add bx, cx
        mov dx, WORD [bx]
        test ax, 1
        jz .EVEN
        mov cl, 4
        shr dx, cl
        jmp .NEXTCLUS
.EVEN:
        and dx, 0x0FFF
.NEXTCLUS:
        mov WORD [cluster], dx
        cmp dx, 0x0FF0
        jb LOAD_IMAGE
        jmp 0x2000:0x0000

FAIL_DISK:
        mov si, msgDisk
FAIL:
        call Print
        xor ah, ah
        int 0x16
        int 0x19

Print:
        lodsb
        or al, al
        jz .done
        mov ah, 0x0E
        mov bx, 0x0007
        int 0x10
        jmp Print
.done:
        ret

ClusterLBA:
        sub ax, 2
        xor cx, cx
        mov cl, BYTE [bpbSectorsPerCluster]
        mul cx
        add ax, WORD [datasector]
        ret

LBACHS:
        xor dx, dx
        div WORD [bpbSectorsPerTrack]
        inc dl
        mov [absoluteSector], dl
        xor dx, dx
        div WORD [bpbHeadsPerCylinder]
        mov [absoluteHead], dl
        mov [absoluteTrack], al
        ret

; AX = LBA, CX = count, ES:BX = buffer
ReadSectors:
.MAIN:
        mov di, 5
.RETRY:
        push ax
        push bx
        push cx
        cmp BYTE [hasLba], 0
        je .CHS
        call ReadLBA
        jnc .OK
.CHS:
        call ReadCHS
        jnc .OK
        xor ax, ax
        mov dl, [bsDriveNumber]
        int 0x13
        dec di
        pop cx
        pop bx
        pop ax
        jnz .RETRY
        jmp FAIL_DISK
.OK:
        pop cx
        pop bx
        pop ax
        add bx, WORD [bpbBytesPerSector]
        jnc .ADV
        mov dx, es
        add dh, 0x10
        mov es, dx
.ADV:
        inc ax
        loop .MAIN
        ret

ReadLBA:
        mov WORD [dap + 2], 1
        mov [dap + 4], bx
        mov [dap + 6], es
        mov [dap + 8], ax
        xor dx, dx
        mov [dap + 10], dx
        mov [dap + 12], dx
        mov [dap + 14], dx
        mov si, dap
        mov dl, [bsDriveNumber]
        mov ah, 0x42
        int 0x13
        ret

ReadCHS:
        call LBACHS
        mov ah, 0x02
        mov al, 0x01
        mov ch, [absoluteTrack]
        mov cl, [absoluteSector]
        mov dh, [absoluteHead]
        mov dl, [bsDriveNumber]
        int 0x13
        ret

detect_lba:
        mov BYTE [hasLba], 0
        mov ah, 0x41
        mov bx, 0x55AA
        mov dl, [bsDriveNumber]
        int 0x13
        jc .done
        cmp bx, 0xAA55
        jne .done
        test cl, 1
        jz .done
        mov BYTE [hasLba], 1
.done:
        ret

absoluteSector db 0
absoluteHead   db 0
absoluteTrack  db 0
hasLba         db 0
datasector     dw 0
cluster        dw 0

dap:
        db 16, 0
        dw 1
        dw 0, 0
        dd 0, 0

ImageName  db "KERNEL  BIN"
msgLoading db "Loading DimOS...", 13, 10, 0
msgDisk    db "Disk read error", 13, 10, 0
msgKernel  db "No KERNEL.BIN", 13, 10, 0

        TIMES 510-($-$$) DB 0
        DW 0xAA55
