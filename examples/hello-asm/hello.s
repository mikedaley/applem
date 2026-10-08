; hello.s - A first program: clear the screen, print a line, return to BASIC
;
; Assembled with ca65 into a raw binary at $0803. ApplEm reads where it loads
; from the debug file the linker writes (build/hello.dbg), puts it there and
; calls it; the RTS at the end returns to Applesoft's ] prompt.

        .setcpu "65C02"

HOME    = $FC58                 ; Monitor: clear the screen, cursor home
COUT    = $FDED                 ; Monitor: print the character in A

        .code
main:   jsr HOME
        ldx #0
next:   lda message,x           ; the next character
        beq done                ; a zero ends the message
        ora #$80                ; the Monitor prints characters with bit 7 set
        jsr COUT
        inx
        bne next
done:   rts

        .rodata
message:
        .byte "HELLO FROM CA65!", $0D, 0
