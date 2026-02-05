**********************************
*                                *
*     HELLO WORLD - APPLE IIE    *
*     65C02 ASSEMBLY EXAMPLE     *
*                                *
**********************************

* Apple IIe ROM routines
COUT     EQU  $FDED       ;Character output routine

         ORG  $0800       ;Standard BASIC program area

**********************************
* Main program entry point
**********************************
START    LDX  #$00        ;Initialize index to 0

**********************************
* Print loop - one char at a time
**********************************
LOOP     LDA  MESSAGE,X   ;Load character at MESSAGE+X
         BEQ  DONE        ;If zero (end of string), we're done
         JSR  COUT        ;Print the character
         INX              ;Increment index
         BNE  LOOP        ;Continue if X hasn't wrapped

**********************************
* End of program
**********************************
DONE     RTS              ;Return to caller

**********************************
* Message data - high bit set for Apple II
**********************************
MESSAGE  DFB  $C8         ;H
         DFB  $C5         ;E
         DFB  $CC         ;L
         DFB  $CC         ;L
         DFB  $CF         ;O
         DFB  $A0         ;(space)
         DFB  $C1         ;A
         DFB  $D0         ;P
         DFB  $D0         ;P
         DFB  $CC         ;L
         DFB  $C5         ;E
         DFB  $A0         ;(space)
         DFB  $C9         ;I
         DFB  $C9         ;I
         DFB  $C5         ;E
         DFB  $A1         ;!
         DFB  $8D         ;Carriage return
         DFB  $00         ;Null terminator
