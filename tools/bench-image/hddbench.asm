; HDDBENCH.COM: HDD Synth Pro's benchmark and stability test for DOS, run
; from the benchmark image (build_image.py, README.md next to this file).
;
; Everything goes through the BIOS (INT 13h, CHS), so every machine is
; measured the same way, and the same operations run in the same order on
; each: the random reads use a fixed seed. The image's layout is in its
; sector 1 (the header, written by build_image.py): a read-only pattern area,
; whose every sector the builder filled with pattern(LBA), and a scratch area
; for the write tests. The FAT partition is never touched except to append
; the results to C:\BENCH.LOG.
;
; One benchmark pass, then (unless /NOSOAK) a soak that repeats the checks
; until a key is pressed. /SOAK starts with the soak.
;
; The pattern, for the sector at LBA L (lo, hi: its 16-bit halves) and a
; seed S (0 in the pattern area; the pass number in the scratch area):
;   word 0: lo ^ S, word 1: hi ^ S,
;   word i >= 2: (lo + i * 9E37h) ^ (hi + i) ^ S, 16-bit arithmetic.
;
; Needs a 386 or later (32-bit registers in real mode). Assemble with
; nasm -f bin -o HDDBENCH.COM hddbench.asm.

        cpu 386
        bits 16
        org 0x100

VERSION         equ 1
HEADER_LBA      equ 1
SEQ_READ_MAX    equ 32768           ; sectors timed reading (16 MB)
SEQ_WRITE_MAX   equ 16384           ; sectors timed writing (8 MB)
RANDOM_READS    equ 500
MAX_REPORTS     equ 8               ; bad sectors printed per pass
LOG_SIZE        equ 4096

start:
        call    check_386
        mov     dx, msg_banner
        call    puts

        ; Options: /SOAK, /NOSOAK
        mov     si, 0x81
.opts:  lodsb
        cmp     al, 13
        je      .opts_done
        cmp     al, '/'
        jne     .opts
        mov     ax, [si]
        and     ax, 0xDFDF              ; upper case
        cmp     ax, 'SO'
        jne     .not_soak
        mov     byte [opt_soak_first], 1
        jmp     .opts
.not_soak:
        cmp     ax, 'NO'
        jne     .opts
        mov     byte [opt_no_soak], 1
        jmp     .opts
.opts_done:

        ; The buffer: one track (at most 63 sectors, 32,256 bytes), in a
        ; segment 32 KB aligned above the program, so it never crosses a
        ; 64 KB boundary (some BIOSes refuse that, for DMA's sake).
        mov     ax, cs
        add     ax, 0x1000 + 0x7FF
        and     ax, 0xF800
        mov     [buf_seg], ax

        call    get_geometry
        call    read_header
        call    print_setup

        cmp     byte [opt_soak_first], 0
        jne     .soak
        call    bench_pass
        cmp     byte [opt_no_soak], 0
        jne     .exit
        mov     dx, msg_soak_prompt
        call    puts
        mov     cx, 182                 ; 10 s of 18.2 ticks
        call    wait_key
        jc      .exit                   ; a key: back to DOS
.soak:  call    soak
.exit:  call    flush_log
        mov     ax, 0x4C00
        int     0x21

; ---- Setup ---------------------------------------------------------------

check_386:
        pushf
        pop     ax
        and     ax, 0x0FFF
        push    ax
        popf
        pushf
        pop     ax
        and     ax, 0xF000
        cmp     ax, 0xF000
        je      .old                    ; 8086/8088: bits 12-15 stay set
        or      ax, 0x7000
        push    ax
        popf
        pushf
        pop     ax
        test    ax, 0x7000
        jz      .old                    ; 286: they stay clear in real mode
        ret
.old:   mov     dx, msg_need_386
        mov     ah, 9
        int     0x21
        mov     ax, 0x4C01
        int     0x21

get_geometry:
        mov     ah, 0x08
        mov     dl, 0x80
        push    es
        int     0x13
        pop     es
        jc      .fail
        movzx   ax, dh
        inc     ax
        mov     [heads], ax
        mov     al, cl
        and     ax, 0x3F
        mov     [spt], ax
        movzx   bx, ch
        movzx   ax, cl
        shl     ax, 2
        and     ax, 0x300
        or      ax, bx
        inc     ax
        mov     [cyls], ax
        mov     ax, [heads]
        mul     word [spt]
        mov     [track_cyl], ax         ; sectors per cylinder (under 64K)
        ; INT 13h extensions: reported, not used
        mov     ah, 0x41
        mov     bx, 0x55AA
        mov     dl, 0x80
        int     0x13
        jc      .no_ext
        cmp     bx, 0xAA55
        jne     .no_ext
        mov     byte [has_ext], 1
.no_ext:
        ret
.fail:  mov     dx, msg_no_disk
        jmp     fatal

read_header:
        mov     eax, HEADER_LBA
        mov     cx, 0x0201
        call    disk_io
        jc      .bad
        push    ds
        mov     ds, [buf_seg]
        xor     si, si
        mov     di, hdr_magic
        mov     cx, 8
        push    es
        push    cs
        pop     es
        repe    cmpsb
        pop     es
        pop     ds
        jne     .bad
        push    ds
        mov     ds, [buf_seg]
        mov     eax, [10]
        mov     ebx, [14]
        mov     ecx, [18]
        mov     edx, [22]
        mov     esi, [26]
        pop     ds
        mov     [disk_sectors], eax
        mov     [pattern_lba], ebx
        mov     [pattern_count], ecx
        mov     [scratch_lba], edx
        mov     [scratch_count], esi
        ret
.bad:   mov     dx, msg_no_header
        jmp     fatal

fatal:  call    puts
        call    flush_log
        mov     ax, 0x4C02
        int     0x21

print_setup:
        call    print_date
        mov     dx, msg_geometry
        call    puts
        movzx   eax, word [cyls]
        call    putu
        mov     dx, msg_h
        call    puts
        movzx   eax, word [heads]
        call    putu
        mov     dx, msg_s
        call    puts
        movzx   eax, word [spt]
        call    putu
        mov     dx, msg_ext_no
        cmp     byte [has_ext], 0
        je      .ext
        mov     dx, msg_ext_yes
.ext:   call    puts
        mov     dx, msg_layout
        call    puts
        mov     eax, [disk_sectors]
        call    putu
        mov     dx, msg_layout2
        call    puts
        mov     eax, [pattern_count]
        call    putu
        mov     dx, msg_layout3
        call    puts
        mov     eax, [scratch_count]
        call    putu
        mov     dx, msg_crlf
        call    puts
        ret

; ---- The benchmark pass ----------------------------------------------------

bench_pass:
        mov     dword [bad_sectors], 0
        mov     dword [io_errors], 0
        mov     dword [retries], 0
        mov     byte [reports], 0

        ; Sequential read, timed, not checked (checking costs CPU time)
        mov     dx, msg_seq_read
        call    puts
        mov     eax, [pattern_count]
        cmp     eax, SEQ_READ_MAX
        jbe     .r1
        mov     eax, SEQ_READ_MAX
.r1:    mov     [run_count], eax
        mov     eax, [pattern_lba]
        mov     [run_lba], eax
        mov     byte [run_op], OP_READ
        call    timed_run
        call    print_rate

        ; Random single-sector reads over the whole disk, timed
        mov     dx, msg_random
        call    puts
        mov     dword [rand_state], 12345
        call    get_ticks
        mov     [t_start], eax
        mov     cx, RANDOM_READS
.rr:    push    cx
        call    rand
        xor     edx, edx
        div     dword [disk_sectors]
        mov     eax, edx
        mov     cx, 0x0201
        call    disk_io_counted
        pop     cx
        loop    .rr
        call    elapsed_ms
        push    eax
        mov     eax, RANDOM_READS
        call    putu
        mov     dx, msg_reads_in
        call    puts
        pop     eax
        push    eax
        call    put_seconds
        mov     dx, msg_ms_each
        call    puts
        pop     eax                     ; ms for all: hundredths of ms each
        mov     ebx, 100
        mul     ebx
        mov     ebx, RANDOM_READS
        div     ebx
        call    put_hundredths
        mov     dx, msg_ms_crlf
        call    puts

        ; Sequential write to the scratch area, timed, with this pass's seed
        mov     dx, msg_seq_write
        call    puts
        mov     eax, [scratch_count]
        cmp     eax, SEQ_WRITE_MAX
        jbe     .w1
        mov     eax, SEQ_WRITE_MAX
.w1:    mov     [run_count], eax
        mov     [write_count], eax
        mov     eax, [scratch_lba]
        mov     [run_lba], eax
        call    next_seed
        mov     byte [run_op], OP_WRITE
        call    timed_run
        call    print_rate

        ; Check what was written, then the whole pattern area
        mov     word [run_label], msg_check_scratch
        mov     eax, [write_count]
        mov     [run_count], eax
        mov     eax, [scratch_lba]
        mov     [run_lba], eax
        mov     byte [run_op], OP_VERIFY
        call    counted_run

        mov     word [run_label], msg_check_pattern
        mov     eax, [pattern_count]
        mov     [run_count], eax
        mov     eax, [pattern_lba]
        mov     [run_lba], eax
        mov     word [seed], 0
        mov     byte [run_op], OP_VERIFY
        call    counted_run

        call    print_verdict
        call    flush_log
        ret

; A run timed: sectors per second from run_count and the time taken.
timed_run:
        call    get_ticks
        mov     [t_start], eax
        call    run
        call    elapsed_ms
        mov     [t_ms], eax
        ret

; A checking run, reported as "<run_label>N sectors, B bad" after any BAD lines.
counted_run:
        mov     eax, [bad_sectors]
        mov     [bad_before], eax
        call    run
        mov     dx, [run_label]
        call    puts
        mov     eax, [run_count]
        call    putu
        mov     dx, msg_sectors_comma
        call    puts
        mov     eax, [bad_sectors]
        sub     eax, [bad_before]
        call    putu
        mov     dx, msg_bad_crlf
        call    puts
        ret

; "N KB in S.ss s = R.rr MB/s" from run_count and t_ms.
print_rate:
        mov     eax, [run_count]
        shr     eax, 1
        call    putu
        mov     dx, msg_kb_in
        call    puts
        mov     eax, [t_ms]
        call    put_seconds
        mov     dx, msg_equals
        call    puts
        ; MB/s in hundredths: sectors * 512 / (ms * 10), MB = 1,000,000 bytes
        mov     eax, [run_count]
        shl     eax, 9
        mov     ebx, [t_ms]
        imul    ebx, ebx, 10
        or      ebx, ebx
        jnz     .ok
        inc     ebx
.ok:    xor     edx, edx
        div     ebx
        call    put_hundredths
        mov     dx, msg_mbs
        call    puts
        ret

print_verdict:
        mov     dx, msg_bios_errors
        call    puts
        mov     eax, [io_errors]
        call    putu
        mov     dx, msg_retries
        call    puts
        mov     eax, [retries]
        call    putu
        mov     dx, msg_close_crlf
        call    puts
        mov     dx, msg_pass
        mov     eax, [bad_sectors]
        or      eax, [io_errors]
        jz      .p
        mov     dx, msg_fail
.p:     call    puts
        ret

; ---- The soak --------------------------------------------------------------

soak:
        mov     byte [in_soak], 1
        mov     dx, msg_soak_start
        call    puts
        call    flush_log
        mov     dword [soak_pass], 0
        mov     dword [soak_bad], 0
        mov     dword [soak_errors], 0
        call    get_ticks
        mov     [soak_start], eax
.pass:  inc     dword [soak_pass]
        mov     dword [bad_sectors], 0
        mov     dword [io_errors], 0
        mov     byte [reports], 0
        mov     byte [stop], 0

        ; The pattern area, then the scratch area written and checked with a
        ; new seed, then random reads in the pattern area, each checked
        mov     eax, [pattern_count]
        mov     [run_count], eax
        mov     eax, [pattern_lba]
        mov     [run_lba], eax
        mov     word [seed], 0
        mov     byte [run_op], OP_VERIFY
        call    run
        cmp     byte [stop], 0
        jne     .end
        call    next_seed
        mov     eax, [scratch_count]
        mov     [run_count], eax
        mov     eax, [scratch_lba]
        mov     [run_lba], eax
        mov     byte [run_op], OP_WRITE
        call    run
        cmp     byte [stop], 0
        jne     .end
        mov     byte [run_op], OP_VERIFY
        call    run
        cmp     byte [stop], 0
        jne     .end
        mov     word [seed], 0
        mov     cx, RANDOM_READS
.rv:    push    cx
        call    rand
        xor     edx, edx
        div     dword [pattern_count]
        mov     eax, edx
        add     eax, [pattern_lba]
        mov     [cur_lba], eax
        mov     cx, 0x0201
        call    disk_io_counted
        jc      .rv_next
        push    es
        mov     es, [buf_seg]
        xor     si, si
        call    verify_sector
        pop     es
.rv_next:
        pop     cx
        loop    .rv

        ; "Soak pass N: B bad, E BIOS errors (total B, E), M min"
        mov     eax, [bad_sectors]
        add     [soak_bad], eax
        mov     eax, [io_errors]
        add     [soak_errors], eax
        mov     dx, msg_soak_pass
        call    puts
        mov     eax, [soak_pass]
        call    putu
        mov     dx, msg_colon
        call    puts
        mov     eax, [bad_sectors]
        call    putu
        mov     dx, msg_soak_bad
        call    puts
        mov     eax, [io_errors]
        call    putu
        mov     dx, msg_soak_total
        call    puts
        mov     eax, [soak_bad]
        call    putu
        mov     dx, msg_comma
        call    puts
        mov     eax, [soak_errors]
        call    putu
        mov     dx, msg_soak_after
        call    puts
        mov     eax, [soak_start]
        mov     [t_start], eax
        call    elapsed_ms
        xor     edx, edx
        mov     ebx, 60000
        div     ebx
        call    putu
        mov     dx, msg_min
        call    puts
        call    flush_log
        call    key_pressed
        jnc     .pass
.end:   mov     dx, msg_soak_stopped
        call    puts
        ret

; ---- Runs over the disk -----------------------------------------------------

OP_READ         equ 0
OP_WRITE        equ 1
OP_VERIFY       equ 2

; run_count sectors from run_lba, a track (or what's left of one) at a time:
; read them (OP_READ), write the pattern with `seed` (OP_WRITE), or read and
; check them against it (OP_VERIFY). In the soak a key sets `stop`.
run:
        mov     eax, [run_lba]
        mov     [cur_lba], eax
        mov     eax, [run_count]
        mov     [left], eax
.next:  cmp     dword [left], 0
        je      .done
        ; This chunk: to the end of the track, at most what's left
        mov     eax, [cur_lba]
        xor     edx, edx
        movzx   ebx, word [spt]
        div     ebx
        mov     eax, ebx
        sub     eax, edx
        cmp     eax, [left]
        jbe     .n
        mov     eax, [left]
.n:     mov     [chunk], ax
        cmp     byte [run_op], OP_WRITE
        jne     .io
        ; Fill the buffer with the chunk's pattern
        push    es
        mov     es, [buf_seg]
        xor     di, di
        mov     eax, [cur_lba]
        push    eax
        mov     cx, [chunk]
.fill:  call    fill_sector
        inc     dword [cur_lba]
        loop    .fill
        pop     eax
        mov     [cur_lba], eax
        pop     es
.io:    mov     eax, [cur_lba]
        mov     cl, [chunk]
        mov     ch, 0x02
        cmp     byte [run_op], OP_WRITE
        jne     .rd
        mov     ch, 0x03
.rd:    call    disk_io_counted
        jc      .failed
        cmp     byte [run_op], OP_VERIFY
        jne     .advance
        push    es
        mov     es, [buf_seg]
        xor     si, si
        mov     eax, [cur_lba]
        push    eax
        mov     cx, [chunk]
.chk:   push    cx
        call    verify_sector
        pop     cx
        inc     dword [cur_lba]
        loop    .chk
        pop     eax
        mov     [cur_lba], eax
        pop     es
        jmp     .advance
.failed:
        movzx   eax, word [chunk]       ; the whole chunk counts as bad
        add     [bad_sectors], eax
.advance:
        movzx   eax, word [chunk]
        add     [cur_lba], eax
        sub     [left], eax
        cmp     byte [in_soak], 0
        je      .next
        call    key_pressed
        jnc     .next
        mov     byte [stop], 1
.done:  ret

; One sector's pattern into es:di (advanced by 512), for cur_lba and seed.
fill_sector:
        mov     ax, [cur_lba]
        xor     ax, [seed]
        stosw
        mov     ax, [cur_lba + 2]
        xor     ax, [seed]
        stosw
        mov     bx, [cur_lba]
        add     bx, (2 * 0x9E37) & 0xFFFF
        mov     dx, [cur_lba + 2]
        add     dx, 2
        push    cx
        mov     cx, 254
.w:     mov     ax, bx
        xor     ax, dx
        xor     ax, [seed]
        stosw
        add     bx, 0x9E37
        inc     dx
        loop    .w
        pop     cx
        ret

; Checks the sector at es:si (advanced by 512) against cur_lba's pattern;
; a bad one is counted and, the first few times a pass, printed.
verify_sector:
        mov     word [bad_words], 0
        xor     cx, cx
        mov     ax, [cur_lba]           ; word 0
        xor     ax, [seed]
        call    .check
        mov     ax, [cur_lba + 2]       ; word 1
        xor     ax, [seed]
        call    .check
        mov     bx, [cur_lba]           ; words 2-255, as fill_sector makes them
        add     bx, (2 * 0x9E37) & 0xFFFF
        mov     dx, [cur_lba + 2]
        add     dx, 2
.w:     mov     ax, bx
        xor     ax, dx
        xor     ax, [seed]
        call    .check
        add     bx, 0x9E37
        inc     dx
        cmp     cx, 256
        jb      .w
        jmp     .report
.check:                                 ; word CX at es:si should be AX; CX, SI step on
        mov     di, [es:si]
        add     si, 2
        cmp     di, ax
        je      .same
        cmp     word [bad_words], 0
        jne     .more
        mov     [first_bad], cx
        mov     [first_seen], di
        mov     [first_want], ax
.more:  inc     word [bad_words]
.same:  inc     cx
        ret
.report:
        cmp     word [bad_words], 0
        je      .ok
        inc     dword [bad_sectors]
        cmp     byte [reports], MAX_REPORTS
        jae     .ok
        inc     byte [reports]
        push    si
        mov     dx, msg_bad_lba
        call    puts
        mov     eax, [cur_lba]
        call    putu
        mov     dx, msg_word
        call    puts
        movzx   eax, word [first_bad]
        call    putu
        mov     dx, msg_seen
        call    puts
        mov     ax, [first_seen]
        call    puthex
        mov     dx, msg_want
        call    puts
        mov     ax, [first_want]
        call    puthex
        mov     dx, msg_bad_words
        call    puts
        movzx   eax, word [bad_words]
        call    putu
        mov     dx, msg_crlf
        call    puts
        pop     si
.ok:    ret

next_seed:
        inc     word [pass_seed]
        mov     ax, [pass_seed]
        mov     [seed], ax
        ret

; ---- The BIOS ----------------------------------------------------------------

; INT 13h on the first hard disk: CH = 02h (read) or 03h (write), EAX's LBA
; (bits 0-23 of it; the image is 128 MB), CL sectors, to or from buf_seg:0.
; Three tries, with a reset between; CF set if all failed. Counts both.
disk_io_counted:
        call    disk_io
        jnc     .ok
        inc     dword [io_errors]
        stc
.ok:    ret

disk_io:
        mov     [io_op], ch
        mov     [io_count], cl
        ; LBA to CHS with the BIOS's geometry
        and     eax, 0x00FFFFFF
        xor     edx, edx
        movzx   ebx, word [track_cyl]
        div     ebx                     ; eax = cylinder, edx = within it
        mov     [io_cyl], ax
        mov     ax, dx
        xor     dx, dx
        div     word [spt]              ; ax = head, dx = sector - 1
        mov     [io_head], al
        inc     dl
        mov     [io_sector], dl
        mov     byte [io_tries], 3
.try:   mov     ax, [io_cyl]
        mov     ch, al
        mov     cl, ah
        shl     cl, 6
        or      cl, [io_sector]
        mov     dh, [io_head]
        mov     dl, 0x80
        mov     al, [io_count]
        mov     ah, [io_op]
        push    es
        mov     es, [buf_seg]
        xor     bx, bx
        int     0x13
        pop     es
        jnc     .ok
        dec     byte [io_tries]
        jz      .fail
        inc     dword [retries]
        xor     ah, ah                  ; reset the disk, and again
        mov     dl, 0x80
        int     0x13
        jmp     .try
.fail:  stc
        ret
.ok:    clc
        ret

; ---- Time, keys, random numbers ------------------------------------------------

get_ticks:                              ; EAX = BIOS ticks since midnight
        push    fs
        mov     ax, 0x40
        mov     fs, ax
        mov     eax, [fs:0x6C]
        pop     fs
        ret

elapsed_ms:                             ; EAX = ms since t_start
        call    get_ticks
        sub     eax, [t_start]
        jnc     .ok
        add     eax, 0x1800B0           ; past midnight
.ok:    mov     ebx, 54925              ; a tick is 54.925 ms
        mul     ebx
        mov     ebx, 1000
        div     ebx
        ret

key_pressed:                            ; CF set (and the key eaten) if one was pressed
        mov     ah, 0x01
        int     0x16
        jz      .none
        xor     ah, ah
        int     0x16
        stc
        ret
.none:  clc
        ret

wait_key:                               ; up to CX ticks; CF set if a key came
        movzx   ecx, cx
        call    get_ticks
        mov     [t_start], eax
.w:     call    key_pressed
        jc      .key
        call    get_ticks
        sub     eax, [t_start]
        cmp     eax, ecx
        jb      .w
        clc
        ret
.key:   stc
        ret

rand:                                   ; EAX = the next of a fixed sequence
        mov     eax, [rand_state]
        imul    eax, eax, 1103515245
        add     eax, 12345
        mov     [rand_state], eax
        shr     eax, 8
        ret

; ---- Output: the screen, and a copy for C:\BENCH.LOG -------------------------

puts:                                   ; the $-terminated string at DX
        push    si
        mov     si, dx
.c:     lodsb
        cmp     al, '$'
        je      .d
        call    putc
        jmp     .c
.d:     pop     si
        ret

putc:
        push    ax
        push    dx
        push    bx
        mov     bx, [log_len]
        cmp     bx, LOG_SIZE
        jae     .screen
        mov     [log_buf + bx], al
        inc     word [log_len]
.screen:
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        pop     bx
        pop     dx
        pop     ax
        ret

putu:                                   ; EAX, unsigned decimal
        push    ebx
        push    ecx
        push    edx
        mov     ebx, 10
        xor     cx, cx
.d:     xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        or      eax, eax
        jnz     .d
.p:     pop     ax
        add     al, '0'
        call    putc
        loop    .p
        pop     edx
        pop     ecx
        pop     ebx
        ret

put_hundredths:                         ; EAX hundredths as "N.nn"
        xor     edx, edx
        mov     ebx, 100
        div     ebx
        push    dx
        call    putu
        mov     al, '.'
        call    putc
        pop     ax
        aam                             ; AH = tens, AL = units
        add     ax, '00'
        xchg    al, ah
        call    putc
        mov     al, ah
        call    putc
        ret

put_seconds:                            ; EAX ms as "S.ss s"
        xor     edx, edx
        mov     ebx, 10
        div     ebx
        call    put_hundredths
        mov     al, ' '
        call    putc
        mov     al, 's'
        call    putc
        ret

puthex:                                 ; AX as four hex digits
        mov     cx, 4
.h:     rol     ax, 4
        push    ax
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .p
        add     al, 'a' - '9' - 1
.p:     call    putc
        pop     ax
        loop    .h
        ret

print_date:                             ; "2026-10-31 07:45"
        mov     ah, 0x2A
        int     0x21                    ; CX year, DH month, DL day
        push    dx
        movzx   eax, cx
        call    putu
        pop     dx
        push    dx
        mov     al, '-'
        call    putc
        movzx   ax, dh
        call    put2
        mov     al, '-'
        call    putc
        pop     dx
        movzx   ax, dl
        call    put2
        mov     ah, 0x2C
        int     0x21                    ; CH hour, CL minute
        mov     al, ' '
        call    putc
        movzx   ax, ch
        push    cx
        call    put2
        mov     al, ':'
        call    putc
        pop     cx
        movzx   ax, cl
        call    put2
        mov     dx, msg_crlf
        call    puts
        ret

put2:                                   ; AL (0-99) as two digits
        aam
        add     ax, '00'
        xchg    al, ah
        call    putc
        mov     al, ah
        call    putc
        ret

; Appends what was printed since the last flush to C:\BENCH.LOG.
flush_log:
        cmp     word [log_len], 0
        je      .done
        mov     ax, 0x3D01              ; open for writing
        mov     dx, log_name
        int     0x21
        jnc     .open
        mov     ah, 0x3C                ; or create it
        xor     cx, cx
        int     0x21
        jc      .done
.open:  mov     bx, ax
        mov     ax, 0x4202              ; to the end
        xor     cx, cx
        xor     dx, dx
        int     0x21
        mov     ah, 0x40
        mov     cx, [log_len]
        mov     dx, log_buf
        int     0x21
        mov     ah, 0x3E
        int     0x21
.done:  mov     word [log_len], 0
        ret

; ---- Data ----------------------------------------------------------------------

msg_banner      db "HDDBENCH 1 - HDD Synth Pro benchmark and stability test", 13, 10, "$"
msg_need_386    db "HDDBENCH needs a 386 or later.", 13, 10, "$"
msg_no_disk     db "No hard disk 80h from the BIOS.", 13, 10, "$"
msg_no_header   db "This disk isn't the HDD Synth Pro benchmark image (no HDDBENCH header in sector 1).", 13, 10, "$"
msg_geometry    db "BIOS geometry: C=$"
msg_h           db " H=$"
msg_s           db " S=$"
msg_ext_no      db ", no INT 13h extensions", 13, 10, "$"
msg_ext_yes     db ", INT 13h extensions", 13, 10, "$"
msg_layout      db "Image: $"
msg_layout2     db " sectors in use, pattern area $"
msg_layout3     db ", scratch area $"
msg_seq_read    db "Sequential read : $"
msg_random      db "Random read     : $"
msg_reads_in    db " reads in $"
msg_ms_each     db ", $"
msg_ms_crlf     db " ms each", 13, 10, "$"
msg_seq_write   db "Sequential write: $"
msg_check_scratch db "Check written   : $"
msg_check_pattern db "Check pattern   : $"
msg_sectors_comma db " sectors, $"
msg_bad_crlf    db " bad", 13, 10, "$"
msg_kb_in       db " KB in $"
msg_equals      db " = $"
msg_mbs         db " MB/s", 13, 10, "$"
msg_bios_errors db "BIOS errors     : $"
msg_retries     db " (retries: $"
msg_pass        db "Result          : PASS", 13, 10, 13, 10, "$"
msg_fail        db "Result          : FAIL", 13, 10, 13, 10, "$"
msg_bad_lba     db "  BAD lba=$"
msg_word        db " word=$"
msg_seen        db " seen=$"
msg_want        db " want=$"
msg_bad_words   db " bad_words=$"
msg_soak_prompt db "Soak test starts in 10 s; press a key for DOS instead.", 13, 10, "$"
msg_soak_start  db "Soak: the pattern area, the scratch area written and checked, 500 random reads; a key stops it.", 13, 10, "$"
msg_soak_pass   db "Soak pass $"
msg_colon       db ": $"
msg_soak_bad    db " bad, $"
msg_soak_total  db " BIOS errors (total $"
msg_comma       db ", $"
msg_soak_after  db "), $"
msg_min         db " min", 13, 10, "$"
msg_soak_stopped db "Soak stopped.", 13, 10, 13, 10, "$"
msg_close_crlf  db ")", 13, 10, "$"
msg_crlf        db 13, 10, "$"
hdr_magic       db "HDDBENCH"
log_name        db "C:\BENCH.LOG", 0

opt_soak_first  db 0
opt_no_soak     db 0
has_ext         db 0
in_soak         db 0                    ; the soak: a key between chunks stops it
stop            db 0
reports         db 0
run_op          db 0
io_op           db 0
io_count        db 0
io_head         db 0
io_sector       db 0
io_tries        db 0
align 4
buf_seg         dw 0
run_label       dw 0
heads           dw 0
spt             dw 0
cyls            dw 0
track_cyl       dw 0
chunk           dw 0
seed            dw 0
pass_seed       dw 0
bad_words       dw 0
first_bad       dw 0
first_seen      dw 0
first_want      dw 0
io_cyl          dw 0
log_len         dw 0
disk_sectors    dd 0
pattern_lba     dd 0
pattern_count   dd 0
scratch_lba     dd 0
scratch_count   dd 0
run_lba         dd 0
run_count       dd 0
write_count     dd 0
cur_lba         dd 0
left            dd 0
t_start         dd 0
t_ms            dd 0
bad_sectors     dd 0
bad_before      dd 0
io_errors       dd 0
retries         dd 0
rand_state      dd 0
soak_pass       dd 0
soak_bad        dd 0
soak_errors     dd 0
soak_start      dd 0
log_buf         times LOG_SIZE db 0
