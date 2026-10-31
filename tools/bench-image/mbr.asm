; A classic CHS master boot record, for a 386 BIOS without INT 13h
; extensions: moves itself to 0000:0600, finds the active partition,
; loads its boot sector to 0000:7C00 by the entry's start CHS, and jumps
; to it with DL = 80h and DS:SI = the partition entry.
; Only the first 440 bytes are code; the partition table is kept as is.
        bits 16
        org 0x600

start:
        cli
        xor ax, ax
        mov ss, ax
        mov sp, 0x7c00
        mov ds, ax
        mov es, ax
        sti
        cld
        mov si, 0x7c00
        mov di, 0x600
        mov cx, 256
        rep movsw
        jmp 0:relocated

relocated:
        mov si, table
        mov cx, 4
.find:
        cmp byte [si], 0x80
        je .found
        add si, 16
        loop .find
        mov si, msg_none
        jmp fail

.found:
        mov bp, si               ; the entry, kept for the boot sector
        mov di, 5                ; tries
.read:
        mov bx, 0x7c00
        mov ax, 0x0201           ; read one sector
        mov dx, [bp]             ; DL = the boot flag (80h, the first hard disk), DH = head:
                                 ; old BIOSes don't always pass the boot drive in DL
        mov cx, [bp + 2]         ; sector (bits 0-5) and cylinder
        int 0x13
        jnc .loaded
        xor ax, ax               ; reset the disk and try again
        int 0x13
        dec di
        jnz .read
        mov si, msg_read
        jmp fail

.loaded:
        cmp word [0x7dfe], 0xaa55
        je .boot
        mov si, msg_os
        jmp fail
.boot:
        mov si, bp
        jmp 0:0x7c00

fail:                            ; print DS:SI, then stop
        lodsb
        test al, al
        jz .halt
        mov ah, 0x0e
        mov bx, 7
        int 0x10
        jmp fail
.halt:
        hlt
        jmp .halt

msg_none: db "No active partition", 0
msg_read: db "Error loading operating system", 0
msg_os:   db "Missing operating system", 0

        times 446 - ($ - $$) db 0
table:
