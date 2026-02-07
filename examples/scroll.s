; scroll.s - Smooth horizontal text scroller for Apple IIe
;
; Scrolls a message across the top line of the 40-column text screen.
; The message slides in from the right, crosses the screen, and exits left
; before looping. Press any key to quit.
;
; Assemble with Merlin or ca65:
;   ca65 scroll.s -o scroll.o && ld65 scroll.o -t none -o SCROLL -S 0x0300
;
; Run from the monitor:  300G
; Or BRUN from DOS/ProDOS after saving.

            ORG  $0300

; ── Zero page temporaries ──────────────────────────────────────────────

OFFSET      EQU  $06            ; current scroll offset (2 bytes)
SRCPTR      EQU  $08            ; pointer into message buffer (2 bytes)

; ── Hardware / ROM addresses ───────────────────────────────────────────

TXTLINE0    EQU  $0400          ; screen RAM for text line 0
KBDSTROBE   EQU  $C000          ; keyboard data (bit 7 = key ready)
KBDCLEAR    EQU  $C010          ; clear keyboard strobe
HOME        EQU  $FC58          ; ROM: clear screen and home cursor

; ── Constants ──────────────────────────────────────────────────────────

SCRWIDTH    EQU  40             ; visible columns
SPEED       EQU  $18            ; delay — lower = faster

; ======================================================================
; Entry — jump over data block so BUFLEN is defined before use
; ======================================================================

            JMP  START

; ======================================================================
; Message buffer
;
; Padded with 40 spaces on each side so the text scrolls cleanly on and
; off screen.  Characters have the high bit set for normal Apple II text
; (ASCII + $80).
; ======================================================================

BUFFER
; 40 leading spaces
            HEX  A0A0A0A0A0A0A0A0A0A0  ; 10
            HEX  A0A0A0A0A0A0A0A0A0A0  ; 20
            HEX  A0A0A0A0A0A0A0A0A0A0  ; 30
            HEX  A0A0A0A0A0A0A0A0A0A0  ; 40

; Message: "HELLO FROM THE APPLE //E EMULATOR! "
            HEX  C8C5CCCCCF             ; HELLO
            HEX  A0                     ; (space)
            HEX  C6D2CFCD              ; FROM
            HEX  A0                     ; (space)
            HEX  D4C8C5                 ; THE
            HEX  A0                     ; (space)
            HEX  C1D0D0CCC5            ; APPLE
            HEX  A0AFAFC5              ; (space)//E
            HEX  A0                     ; (space)
            HEX  C5CDD5CCC1D4CFD2      ; EMULATOR
            HEX  A1                     ; !
            HEX  A0A0A0                 ; (trailing spaces before pad)

MSGLEN      EQU  *-BUFFER-40    ; length of message + trailing spaces

; 40 trailing spaces
            HEX  A0A0A0A0A0A0A0A0A0A0  ; 10
            HEX  A0A0A0A0A0A0A0A0A0A0  ; 20
            HEX  A0A0A0A0A0A0A0A0A0A0  ; 30
            HEX  A0A0A0A0A0A0A0A0A0A0  ; 40

BUFEND
BUFLEN      EQU  BUFEND-BUFFER-SCRWIDTH+1  ; number of scroll positions

; ======================================================================
; Main code
; ======================================================================

START       JSR  HOME           ; clear screen

            LDA  #0             ; reset scroll offset
            STA  OFFSET
            STA  OFFSET+1

; ── Main loop ─────────────────────────────────────────────────────────

LOOP        LDA  OFFSET         ; SRCPTR = BUFFER + OFFSET
            CLC
            ADC  #<BUFFER
            STA  SRCPTR
            LDA  OFFSET+1
            ADC  #>BUFFER
            STA  SRCPTR+1

; Copy 40 characters from SRCPTR to screen line 0

            LDY  #SCRWIDTH-1
COPY        LDA  (SRCPTR),Y
            STA  TXTLINE0,Y
            DEY
            BPL  COPY

; Advance the offset; wrap when we reach the end of the padded message

            INC  OFFSET
            BNE  NOWRAP
            INC  OFFSET+1
NOWRAP
            LDA  OFFSET+1       ; compare OFFSET with BUFLEN
            CMP  #>BUFLEN
            BCC  NODELAY        ; high byte less — not at end
            BNE  DORESET        ; high byte greater — past end
            LDA  OFFSET
            CMP  #<BUFLEN
            BCC  NODELAY        ; low byte < BUFLEN — continue
DORESET     LDA  #0             ; wrap offset back to zero
            STA  OFFSET
            STA  OFFSET+1
            JMP  LOOP
NODELAY

; ── Frame delay ───────────────────────────────────────────────────────

            LDX  #SPEED
DELAYOUTER  LDY  #0
DELAYINNER  DEY
            BNE  DELAYINNER
            DEX
            BNE  DELAYOUTER

; ── Check for keypress (quit on any key) ──────────────────────────────

            LDA  KBDSTROBE
            BPL  LOOP           ; no key — keep scrolling
            STA  KBDCLEAR       ; clear strobe
            RTS                 ; return to caller / monitor

