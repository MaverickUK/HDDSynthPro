; CDCHECK.COM: checks the CD-ROM build's test disc (host/hddproto/test_iso.py)
; on DOS. Run it from the disc (D:, then CDCHECK): it reads every file the
; disc should hold, through DOS and the CD-ROM driver, and compares each
; one's size and CRC-32 with the table test_iso.py builds into it
; (files.inc). Files whose ISO 9660 names aren't 8.3 are left out (hdiutil
; keeps long ones; the Mac's check covers them by their Joliet names). DOS needs no other tools, so a boot floppy with a CD-ROM
; driver and MSCDEX is enough.
;
; CDCHECK /R repeats the check until a key is pressed, as a soak.
; The exit code (ERRORLEVEL) is 1 if any file was missing or wrong.
;
; Needs a 386 or later (32-bit registers in real mode). test_iso.py
; assembles it: nasm -f bin -o CDCHECK.COM cdcheck.asm, with files.inc
; next to it.

        cpu 386
        bits 16
        org 0x100

BUFSZ   equ 32768

start:
        mov     dx, msg_banner
        call    puts
        mov     ax, skipped_files       ; from files.inc
        test    ax, ax
        jz      .tables
        call    putdec
        mov     dx, msg_skipped
        call    puts
.tables:
        call    make_crc_table

        ; Option: /R (repeat)
        mov     si, 0x81
.opt:   lodsb
        cmp     al, 13
        je      .pass
        cmp     al, '/'
        jne     .opt
        lodsb
        or      al, 0x20
        cmp     al, 'r'
        jne     .opt
        mov     byte [repeat], 1
        jmp     .opt

.pass:  inc     word [passes]
        mov     word [bad], 0
        mov     word [checked], 0
        mov     si, files
.file:  cmp     byte [si], 0
        je      .summary
        call    check_file
        jmp     .file

.summary:
        mov     dx, msg_pass
        call    puts
        mov     ax, [passes]
        call    putdec
        mov     dx, msg_colon
        call    puts
        mov     ax, [checked]
        call    putdec
        mov     dx, msg_files
        call    puts
        mov     ax, [bad]
        test    ax, ax
        jnz     .some_bad
        mov     dx, msg_all_ok
        call    puts
        jmp     .after
.some_bad:
        mov     byte [failed], 1
        call    putdec
        mov     dx, msg_n_bad
        call    puts
.after:
        cmp     byte [repeat], 0
        je      .exit
        mov     ah, 0x0b                ; a key pressed?
        int     0x21
        test    al, al
        jz      .pass
        mov     ah, 0x08                ; take it
        int     0x21
.exit:  mov     al, [failed]
        mov     ah, 0x4c
        int     0x21

; Checks the table entry at SI (path, 0, dword size, dword CRC-32) and
; leaves SI on the next entry. In repeat passes it prints only problems.
check_file:
        mov     [entry], si
        cmp     word [passes], 1
        jne     .quiet
        call    putz
.quiet: mov     dx, [entry]
        mov     ax, 0x3d00              ; open, read only
        int     0x21
        jc      .missing
        mov     [handle], ax
        mov     dword [crc], 0xffffffff
        mov     dword [count], 0
.read:  mov     bx, [handle]
        mov     ah, 0x3f
        mov     cx, BUFSZ
        mov     dx, buf
        int     0x21
        jc      .read_error
        test    ax, ax
        jz      .eof
        movzx   ecx, ax
        add     [count], ecx
        push    si
        mov     si, buf
        mov     cx, ax
        mov     edx, [crc]
.crc:   lodsb                           ; crc = table[(crc ^ b) & ff] ^ (crc >> 8)
        xor     al, dl
        movzx   bx, al
        shl     bx, 2
        shr     edx, 8
        xor     edx, [crc_table + bx]
        loop    .crc
        mov     [crc], edx
        pop     si
        jmp     .read

.eof:   call    close
        call    skip_name               ; SI: the entry's size and CRC
        inc     word [checked]
        mov     eax, [count]
        cmp     eax, [si]
        jne     .wrong_size
        mov     eax, [crc]
        not     eax
        cmp     eax, [si + 4]
        jne     .wrong_data
        cmp     word [passes], 1
        jne     .next
        mov     dx, msg_ok
        jmp     .report

.missing:
        mov     dx, msg_missing
        jmp     .problem_no_file
.read_error:
        call    close
        mov     dx, msg_read_error
.problem_no_file:
        push    dx
        call    skip_name
        inc     word [checked]
        pop     dx
        jmp     .problem
.wrong_size:
        mov     dx, msg_wrong_size
        jmp     .problem
.wrong_data:
        mov     dx, msg_wrong_data
.problem:
        inc     word [bad]
        cmp     word [passes], 1
        je      .report
        push    dx                      ; repeat passes: name the file first
        push    si
        mov     si, [entry]
        call    putz
        pop     si
        pop     dx
.report:
        call    puts
.next:  add     si, 8
        ret

; SI: from the entry's path to just past its 0.
skip_name:
        mov     si, [entry]
.s:     lodsb
        test    al, al
        jnz     .s
        ret

close:
        mov     bx, [handle]
        mov     ah, 0x3e
        int     0x21
        ret

make_crc_table:
        mov     di, crc_table
        xor     cx, cx
.entry: movzx   eax, cx
        mov     bx, 8
.bit:   shr     eax, 1
        jnc     .no_xor
        xor     eax, 0xedb88320
.no_xor:
        dec     bx
        jnz     .bit
        stosd
        inc     cx
        cmp     cx, 256
        jb      .entry
        ret

; Prints the $-terminated string at DX.
puts:   mov     ah, 0x09
        int     0x21
        ret

; Prints the 0-terminated string at SI (SI unchanged).
putz:   push    si
.c:     lodsb
        test    al, al
        jz      .done
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     .c
.done:  pop     si
        ret

; Prints AX in decimal.
putdec: mov     bx, 10
        xor     cx, cx
.div:   xor     dx, dx
        div     bx
        push    dx
        inc     cx
        test    ax, ax
        jnz     .div
.out:   pop     dx
        add     dl, '0'
        mov     ah, 0x02
        int     0x21
        loop    .out
        ret

msg_banner      db "CDCHECK: the HDDSynthPro test disc, every file's size and CRC-32", 13, 10, "$"
msg_skipped     db " file(s) with long names left out: DOS can't open them", 13, 10, "$"
msg_ok          db " ok", 13, 10, "$"
msg_missing     db " MISSING", 13, 10, "$"
msg_read_error  db " READ ERROR", 13, 10, "$"
msg_wrong_size  db " WRONG SIZE", 13, 10, "$"
msg_wrong_data  db " WRONG DATA", 13, 10, "$"
msg_pass        db "Pass $"
msg_colon       db ": $"
msg_files       db " files, $"
msg_all_ok      db "all ok", 13, 10, "$"
msg_n_bad       db " BAD", 13, 10, "$"

repeat          db 0
failed          db 0
passes          dw 0

files:
%include "files.inc"
        db      0

section .bss
bad             resw 1
checked         resw 1
entry           resw 1
handle          resw 1
crc             resd 1
count           resd 1
crc_table       resd 256
buf             resb BUFSZ
