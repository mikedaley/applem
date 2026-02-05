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
         PHX              ;Save X on stack (65C02)
         JSR  DELAY       ;Wait a bit
         PLX              ;Restore X from stack (65C02)
         INX              ;Increment index
         BNE  LOOP        ;Continue if X hasn't wrapped

**********************************
* End of program
**********************************
DONE     RTS              ;Return to caller

**********************************
* Delay routine - nested loop
* Adjust WAIT value for longer/shorter delay
**********************************
WAIT     EQU  $60         ;Delay multiplier (~100ms)

DELAY    LDY  #WAIT       ;Outer loop counter
:OUTER   LDX  #$FF        ;Inner loop counter
:INNER   DEX              ;Decrement inner
         BNE  :INNER      ;Loop until X=0
         DEY              ;Decrement outer
         BNE  :OUTER      ;Loop until Y=0
         RTS

**********************************
* Message data
* Using " delimiter sets high bit automatically
**********************************
MESSAGE  ASC  "HELLO APPLE IIE!"
         DFB  $8D         ;Carriage return (with high bit)
         DFB  $00         ;Null terminator
