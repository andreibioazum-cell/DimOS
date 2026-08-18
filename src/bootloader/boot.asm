; ==================================================================
; DimOS -- FAT12 bootloader (exactly 512 bytes)
; Copyright (C) 2025 PRoX2011
;
; Loads KERNEL.BIN to 2000:0000 and preloads the FAT, root directory
; and the first data sectors for the kernel file manager.
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

; Fixed 1.44M FAT12 layout (must match the BPB and image_inspector):
;   reserved=1, 2 FATs * 9, root=224 entries -> data LBA 33
ROOT_LBA      equ 19
ROOT_SECTS    equ 14
FAT_LBA       equ 1
FAT_SECTS     equ 18
DATA_LBA      equ 33
DATA_PRELOAD  equ 64
ROOT_ENTRIES  equ 224

main:
        cli
        cld
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

        ; Root directory -> 1000:0000
        mov ax, ROOT_LBA
        mov cx, ROOT_SECTS
        mov bx, 0x1000
        mov es, bx
        xor bx, bx
        call ReadSectors

        ; ES still addresses the root directory.
        mov cx, ROOT_ENTRIES
        xor di, di
.find:
        push cx
        push di
        mov cx, 11
        mov si, ImageName
        repe cmpsb
        pop di
        pop cx
        je .found
        add di, 32
        loop .find
        mov si, msgKernel
        jmp fail

.found:
        ; Cluster is in the directory entry at ES:DI, not DS:DI.
        ; mkfs.vfat writes a volume label first, so DI is rarely 0.
        mov ax, [es:di + 26]
        mov [cluster], ax

        ; Both FATs -> 07C0:0200 (physical 0x7E00)
        mov ax, 0x07C0
        mov es, ax
        mov bx, 0x0200
        mov ax, FAT_LBA
        mov cx, FAT_SECTS
        call ReadSectors

        ; First 64 data sectors -> 3000:0000 (TYPE/DIR window)
        mov ax, 0x3000
        mov es, ax
        xor bx, bx
        mov ax, DATA_LBA
        mov cx, DATA_PRELOAD
        call ReadSectors

        ; KERNEL.BIN -> 2000:0000
        mov ax, 0x2000
        mov es, ax
        xor bx, bx

.load:
        mov ax, [cluster]
        add ax, DATA_LBA - 2        ; LBA = (cluster - 2) * spc + 33
        mov cx, 1
        call ReadSectors

        mov ax, [cluster]
        mov si, ax
        shr si, 1
        add si, ax                  ; cluster * 3 / 2
        mov dx, [si + 0x0200]
        test al, 1
        jz .even
        mov cl, 4
        shr dx, cl
        jmp .next
.even:
        and dx, 0x0FFF
.next:
        mov [cluster], dx
        cmp dx, 0x0FF0
        jb .load

        jmp 0x2000:0x0000

fail_disk:
        mov si, msgDisk
fail:
        lodsb
        or al, al
        jz .wait
        mov ah, 0x0E
        mov bx, 0x0007
        int 0x10
        jmp fail
.wait:
        xor ah, ah
        int 0x16
        int 0x19

; AX = LBA, CX = count, ES:BX = buffer. Advances ES:BX past the read.
ReadSectors:
.main:
        mov di, 5
.retry:
        push ax
        push bx
        push cx

        mov [dap + 4], bx
        mov [dap + 6], es
        mov [dap + 8], ax
        mov si, dap
        mov dl, [bsDriveNumber]
        mov ah, 0x42
        int 0x13
        jnc .ok

        ; CHS fallback. AX still holds the LBA (int 13h AH=42h
        ; overwrites AX, so restore it first). pop does not change CF.
        pop cx
        pop bx
        pop ax
        push ax
        push bx
        push cx
        xor dx, dx
        mov cx, 18
        div cx                      ; AX=temp, DX=sector-1
        inc dx
        mov cl, dl                  ; sector
        mov dh, al
        and dh, 1                   ; head  (2-sided floppy)
        shr ax, 1
        mov ch, al                  ; cylinder
        mov dl, [bsDriveNumber]
        mov ax, 0x0201
        int 0x13
        jnc .ok

        xor ax, ax
        mov dl, [bsDriveNumber]
        int 0x13
        pop cx
        pop bx
        pop ax
        dec di
        jnz .retry
        jmp fail_disk

.ok:
        pop cx
        pop bx
        pop ax
        add bx, 512
        jnc .adv
        mov dx, es
        add dh, 0x10
        mov es, dx
.adv:
        inc ax
        loop .main
        ret

cluster     dw 0

dap:
        db 16, 0
        dw 1
        dw 0, 0
        dd 0, 0

ImageName   db "KERNEL  BIN"
msgDisk     db "Disk error", 0
msgKernel   db "No KERNEL.BIN", 0

%if ($ - $$) > 510
        %error "boot sector exceeds 510 bytes"
%endif
        TIMES 510-($-$$) DB 0
        DW 0xAA55
