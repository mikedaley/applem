; demo.s - Something for the profiler to find
;
; Colour bars that scroll across the lo-res screen, drawn by a program whose
; routines cost very different amounts, so the Profiler has something to rank:
;
;   plot          one pixel; called 1,920 times a pass. The heaviest routine.
;   draw_bars     the loops around plot, and one multiply per column.
;   clear_screen  wipes the screen first, which is pure waste: draw_bars
;                 covers every pixel anyway. The profiler shows what it costs.
;   fib           Fibonacci(12), recursively: 465 calls of itself a pass, to
;                 show recursion counted once in a routine's total.
;   multiply      8 by 8 bits, shift and add: 40 small calls a pass.
;   wait_vbl      polls $C019 for the vertical blank: a busy wait, which shows
;                 as hot lines that do no work.
;
; Runs for ever; Control-Reset stops it.

        .setcpu "65C02"

; Zero page the monitor and Applesoft leave alone
ptr     = $FA           ; two bytes: the row being drawn
colour  = $FC
temp    = $FD
mul_lo  = $FE
mul_b   = $FF
col     = $EB
yy      = $EC
frame   = $ED
fibtmp  = $EE
base    = $EF

; Soft switches
TXTCLR  = $C050         ; graphics
MIXCLR  = $C052         ; full screen
LOWSCR  = $C054         ; page 1
LORES   = $C056
CLR80VID = $C00C
RDVBLBAR = $C019        ; bit 7 low during vertical blank on a //e

        .code

.proc main
        sta TXTCLR
        sta MIXCLR
        sta LOWSCR
        sta LORES
        sta CLR80VID
        stz frame
loop:
        jsr clear_screen
        jsr draw_bars
        lda #12
        jsr fib
        jsr wait_vbl
        inc frame
        bra loop
.endproc

; Every pixel black, row by row (the screen holes are left alone).
.proc clear_screen
        ldx #23
row:
        lda rowlo,x
        sta ptr
        lda rowhi,x
        sta ptr+1
        lda #0
        ldy #39
byte:
        sta (ptr),y
        dey
        bpl byte
        dex
        bpl row
        rts
.endproc

; Forty columns of 48 pixels, each column's colours shifted by the frame.
.proc draw_bars
        stz col
column:
        lda col
        ldx frame
        jsr multiply            ; col * frame
        lda mul_lo
        lsr a
        lsr a
        lsr a
        sta base
        stz yy
pixel:
        lda yy
        lsr a
        lsr a
        lsr a                   ; a band every eight pixels
        clc
        adc base
        ldx col
        ldy yy
        jsr plot
        inc yy
        lda yy
        cmp #48
        bne pixel
        inc col
        lda col
        cmp #40
        bne column
        rts
.endproc

; A pixel: A = colour (low nibble), X = column 0-39, Y = line 0-47.
; Two lines share a byte, the even one in the low nibble.
.proc plot
        and #$0F
        sta colour
        tya
        lsr a                   ; the text row; carry set for an odd line
        tay
        lda rowlo,y
        sta ptr
        lda rowhi,y
        sta ptr+1
        txa
        tay
        bcs odd
        lda (ptr),y
        and #$F0
        ora colour
        sta (ptr),y
        rts
odd:
        lda colour
        asl a
        asl a
        asl a
        asl a
        sta temp
        lda (ptr),y
        and #$0F
        ora temp
        sta (ptr),y
        rts
.endproc

; A * X, shift and add. The high byte in A, the low in mul_lo.
.proc multiply
        sta mul_lo
        stx mul_b
        lda #0
        ldx #8
        lsr mul_lo
step:
        bcc skip
        clc
        adc mul_b
skip:
        ror a
        ror mul_lo
        dex
        bne step
        rts
.endproc

; Fibonacci(A), modulo 256, the slow way.
.proc fib
        cmp #2
        bcs recurse
        rts                     ; fib(0) = 0, fib(1) = 1
recurse:
        pha                     ; n
        dec a
        jsr fib                 ; fib(n - 1)
        plx                     ; n
        pha                     ; fib(n - 1)
        txa
        dec a
        dec a
        jsr fib                 ; fib(n - 2)
        sta fibtmp
        pla
        clc
        adc fibtmp
        rts
.endproc

; Wait for the start of the next vertical blank.
.proc wait_vbl
inblank:
        bit RDVBLBAR
        bpl inblank             ; still in the last one
drawing:
        bit RDVBLBAR
        bmi drawing             ; the picture is being drawn
        rts
.endproc

        .rodata

; Where each of the 24 text rows starts.
rowlo:
        .repeat 24, r
        .byte <($0400 + (r .mod 8) * $80 + (r / 8) * $28)
        .endrepeat
rowhi:
        .repeat 24, r
        .byte >($0400 + (r .mod 8) * $80 + (r / 8) * $28)
        .endrepeat
