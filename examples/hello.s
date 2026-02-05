**********************************
*                                *
*     HELLO WORLD - APPLE IIE    *
*     65C02 ASSEMBLY EXAMPLE     *
*                                *
**********************************

* Apple IIe ROM routines
COUT     EQU  $FDED       ;Character output routine
CROUT    EQU  $FD8E       ;Carriage return

* Zero page locations for pointer
PTR      EQU  $06         ;String pointer (2 bytes)

         ORG  $0800       ;Standard BASIC program area

**********************************
* Main program entry point
**********************************
START    LDA  #<MESSAGE   ;Load low byte of message address
         STA  PTR         ;Store in zero page pointer
         LDA  #>MESSAGE   ;Load high byte of message address
         STA  PTR+1       ;Store in zero page pointer+1

         LDY  #$00        ;Initialize index to 0

**********************************
* Print loop - one char at a time
**********************************
LOOP     LDA  (PTR),Y     ;Load character at pointer+Y
         BEQ  DONE        ;If zero (end of string), we're done
         ORA  #$80        ;Set high bit for Apple II display
         JSR  COUT        ;Print the character
         INY              ;Increment index
         BNE  LOOP        ;Continue if Y hasn't wrapped (max 256 chars)

**********************************
* End of program
**********************************
DONE     JSR  CROUT       ;Print carriage return
         RTS              ;Return to caller

**********************************
* Message data (null-terminated)
**********************************
MESSAGE  ASC  "HELLO, APPLE IIE!"
         DFB  $00         ;Null terminator
