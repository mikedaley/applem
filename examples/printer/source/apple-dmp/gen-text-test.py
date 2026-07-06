#!/usr/bin/env python3
# Generator for the Apple DMP FULL FEATURE TEST (text-test.bas).
#
# The Apple Dot Matrix Printer (1982, rebadged C.Itoh 8510) is the single-
# font monochrome ancestor of the ImageWriters: no NLQ / colour / half-
# height / super-subscript / MouseText, and vs the IW-I also no ESC c soft
# reset, no ESC s spacing mode (ESC 1-6 one-shot gaps instead), no ESC u
# tab-add, no ESC g graphics groups, no semicondensed pitch. Everything it
# CAN do is exercised here, split across two transports:
#
#   COUT blocks (PR#1, Applesoft PRINT)  - pitches, styles, proportional,
#     line spacing, direction, ESC R repeat, reverse-feed stacking.
#   Raw-stream blocks (POKE 49296 $C090) - anything COUT/parallel firmware
#     mangles: CTRL-I tabs (the parallel card command char), ESC D/Z
#     soft-switch NUL params, dot graphics with high-bit data bytes, and
#     the custom-font download whose CTRL-D terminator ProDOS would eat.
#
# Spec source: Apple Dot Matrix Printer User's Manual Part I (Reference).

ESC = 27
CR  = [13, 10]

def txt(s):
    return [ord(c) for c in s]

def title(s):                                   # underlined section title
    return [ESC, 0x58] + txt(s) + [ESC, 0x59] + CR + CR

# --- glyph helper (bit0 = top row), same art as gen-math-sum.py -------------
def art(rows):
    h = len(rows); w = max(len(r) for r in rows)
    cols = []
    for x in range(w):
        b = 0
        for y in range(h):
            if x < len(rows[y]) and rows[y][x] == '#':
                b |= (1 << y)
        cols.append(b)
    return cols

def glyph(key, cols):
    return [key, 0x40 + len(cols)] + list(cols)

INLINE_SIGMA = art(["#######","#......",".#.....","..#....","..#....",".#.....","#......","#######"])
INLINE_INF   = art([".##.##.","#..#..#","#..#..#","#..#..#","#..#..#",".##.##."])
MINI_I  = art([".#.","...","##.",".#.",".#.","###"])
MINI_EQ = art(["....","####","....","####","...."])
MINI_1  = art([".#.","##.",".#.",".#.",".#.","###"])
SIGMA_16 = [
    "############","##..........",".##.........","..##........","...##.......",
    "....##......",".....##.....","......##....","......##....",".....##.....",
    "....##......","...##.......","..##........",".##.........","##..........",
    "############",
]
SIGMA_TOP = art(SIGMA_16[0:8]); SIGMA_BOT = art(SIGMA_16[8:16])
BIG_INF  = art(["..##.....##..",".#..#...#..#.","#....#.#....#","#.....#.....#",
                "#....#.#....#",".#..#...#..#.","..##.....##.."])
FRAC_BAR = art(["","","","############","############"])

C_ISIG, C_IINF, C_STOP, C_SBOT = 92, 123, 91, 93
C_BINF, C_BAR,  C_MI,   C_MEQ, C_M1 = 126, 95, 94, 96, 124

def escT(n): return [ESC, 0x54] + txt("%02d" % n)
def escF(n): return [ESC, 0x46] + txt("%04d" % n)
REV = [ESC, 0x72]; FWD = [ESC, 0x66]; LF = [10]
EOL_CR_ONLY = [ESC, 0x5A] + txt("HR")   # ESC Z H R : end-of-line at CR only
EOL_DEFAULT = [ESC, 0x44] + txt("HR")   # ESC D H R : CR,LF,FF end-of-line

# ============================ raw byte stream ================================
stream = []

# ---- H. TABS + LEFT MARGIN (CTRL-I is the parallel command char -> POKE) ----
stream += title("H. TABS + LEFT MARGIN  (ESC ( ) 0 / CTRL-I / ESC L)")
stream += [ESC, 0x28] + txt("10,24,38.")        # ESC ( set stops 10,24,38
stream += txt("|") + [9] + txt("TAB10") + [9] + txt("TAB24") + [9] + txt("TAB38") + CR
stream += [ESC, 0x29] + txt("24.")              # ESC ) : clear ONLY stop 24
stream += txt("|") + [9] + txt("TAB10") + [9] + txt("TAB38 (24 CLEARED, ESC ))") + CR
stream += [ESC, 0x30]                           # ESC 0 : clear all stops
stream += [ESC, 0x4C] + txt("010")              # ESC L : left margin col 10
stream += txt("LEFT MARGIN 10 (ESC L 010)") + CR
stream += txt("STILL INDENTED") + CR
stream += [ESC, 0x4C] + txt("000")              # margin back to 0
stream += txt("MARGIN BACK TO 0") + CR + CR

# ---- I. SLASHED ZEROS (ESC D/Z NUL params die in COUT) ----------------------
stream += title("I. SLASHED ZEROS  (ESC D/Z SOFT SWITCH, SW2-1)")
stream += [ESC, 0x44, 0x00, 0x01]               # ESC D 00 01 : slashed
stream += txt("SLASHED:   0O0O 100 2048 (zero vs oh)") + CR
stream += [ESC, 0x5A, 0x00, 0x01]               # ESC Z 00 01 : unslashed (default)
stream += txt("UNSLASHED: 0O0O 100 2048 (zero vs oh)") + CR + CR

# ---- J. DOT GRAPHICS (high-bit data bytes -> POKE only) ---------------------
# The DMP has ESC G and ESC V only (Appendix B) - no ESC g groups.
stream += title("J. DOT GRAPHICS  (ESC G / ESC V, PICA 80 DPI)")
heart = [0x06, 0x0F, 0x1F, 0x3E, 0x1F, 0x0F, 0x06, 0x00]
stream += txt("ESC G sprites: ")
stream += [ESC, 0x47] + txt("0032") + heart + heart + heart + heart
stream += CR
stream += txt("ESC V rule:    ")
stream += [ESC, 0x56] + txt("0120") + [0x55]    # 120 columns of dashed rule
stream += CR
stream += txt("ESC G checker: ")
stream += [ESC, 0x47] + txt("0032") + [0xAA, 0x55] * 16
stream += CR + CR

# ---- K. CUSTOM FONT: LATEX MATH GAG (CTRL-D terminator -> POKE only) --------
stream += title("K. CUSTOM FONT  (ESC + / ESC I / ESC ' / ESC $)")
stream += [ESC, 0x4E]                           # ESC N : pica
stream += [ESC, 0x2B]                           # ESC + : 16-col custom cells
stream += [ESC, 0x49]                           # ESC I : download
stream += glyph(C_ISIG, INLINE_SIGMA) + glyph(C_IINF, INLINE_INF)
stream += glyph(C_STOP, SIGMA_TOP)   + glyph(C_SBOT, SIGMA_BOT)
stream += glyph(C_BINF, BIG_INF)     + glyph(C_BAR,  FRAC_BAR)
stream += glyph(C_MI,   MINI_I)      + glyph(C_MEQ,  MINI_EQ) + glyph(C_M1, MINI_1)
stream += [4]                                   # CTRL-D : end download
stream += [ESC, 0x27]                           # ESC ' : custom font on
stream += [ESC, 0x41] + CR                      # 6 lpi + headroom for raised limit
stream += txt("   This sum ") + [C_ISIG]
stream += EOL_CR_ONLY                           # LF = pure feed (no carriage return)
stream += escT(8)  + REV + LF + [C_IINF]        # up 8/144 -> superscript infinity
stream += [8]                                   # CTRL-H : back one cell
stream += escT(20) + FWD + LF + [C_MI, C_MEQ, C_M1]  # down 20/144 -> subscript i=1
stream += escT(12) + REV + LF + FWD             # up 12/144 -> baseline
stream += EOL_DEFAULT                           # factory end-of-line behaviour back
stream += txt(" 1/i looks different than") + [ESC, 0x41] + CR + CR + CR
SX = 120; FX = 142; INFX = SX + 1
stream += escT(16)
stream += escF(INFX) + [C_BINF] + CR
stream += escF(SX) + [C_STOP] + escF(FX + 2) + txt("1") + CR
stream += escF(SX) + [C_SBOT] + escF(FX) + [C_BAR] + escT(24) + CR
stream += escF(SX - 6) + txt("i=1") + escF(FX + 2) + txt("i") + CR
stream += [ESC, 0x24, ESC, 0x41]                # ROM font, 6 lpi
stream += CR + CR + [12]                        # feed out + form feed
stream += [-1]

# ============================ Applesoft program ==============================
prog = r'''10 REM ===== APPLE DMP - FULL FEATURE TEST =====
20 REM SINGLE-FONT 1982 MODEL: NO NLQ/COLOUR/HALF-HEIGHT/
30 REM SCRIPTS/MOUSETEXT (THOSE ARRIVED WITH THE IW-II).
40 REM COUT BLOCKS VIA PR#1; TABS, SOFT SWITCHES, GRAPHICS
50 REM AND CUSTOM FONT NEED RAW 8-BIT BYTES (CTRL-I IS THE
55 REM PARALLEL CARD COMMAND CHAR, CTRL-D DIES IN PRODOS,
60 REM DATA>127 LOSES ITS HIGH BIT) SO THEY POKE $C090.
65 E$ = CHR$(27):Q$ = CHR$(34):D$ = CHR$(4)
70 A = 49296: REM PARALLEL SLOT1 DATA LATCH ($C090)
75 S$ = "Sphinx quartz JUDGE 0123456789"
80 PRINT D$;"PR#1"
90 GOSUB 9000
100 PRINT E$;"!";"APPLE DMP - FULL FEATURE TEST";E$;Q$
110 PRINT : PRINT
120 REM ====== BLOCK A: CHARACTER PITCHES ======
130 T$ = "A. PITCHES  (DEFAULT IS PICA 10 CPI; NO SEMICOND)": GOSUB 9100
140 P$ = "Apple DMP 12345"
150 PRINT E$;"N";"Extended   9 (n): ";E$;"n";P$
160 PRINT E$;"N";"Pica      10 (N): ";E$;"N";P$
170 PRINT E$;"N";"Elite     12 (E): ";E$;"E";P$
190 PRINT E$;"N";"Condensed 15 (q): ";E$;"q";P$
200 PRINT E$;"N";"Ultracond 17 (Q): ";E$;"Q";P$
210 PRINT E$;"N";"Prop-Pica    (p): ";E$;"p";P$
220 PRINT E$;"N";"Prop-Elite   (P): ";E$;"P";P$
230 PRINT E$;"N";
240 REM ====== BLOCK B: STYLES ======
250 T$ = "B. STYLES  (BOLD ESC ! / UL ESC X / HEADLINE CTRL-N)": GOSUB 9100
260 GOSUB 1000
270 PRINT "plain : ";S$
280 PRINT "bold  : ";E$;"!";S$;E$;Q$
290 PRINT "undrln: ";E$;"X";S$;E$;"Y"
300 PRINT "headln: ";CHR$(14);"WIDE HEADLINE";CHR$(15)
310 W$ = "HEAVY"
320 PRINT "over2x: ";: PRINT W$;: GOSUB 2400: PRINT
330 PRINT "over3x: ";: PRINT W$;: GOSUB 2400: GOSUB 2400: PRINT
340 V$ = "STRIKE": PRINT "strike: ";: PRINT V$;: FOR I = 1 TO LEN(V$): PRINT CHR$(8);: NEXT I: FOR I = 1 TO LEN(V$): PRINT "-";: NEXT I: PRINT
350 PRINT "stack : ";E$;"!";E$;"X";CHR$(14);"BUW";CHR$(15);E$;"Y";E$;Q$
360 REM ====== BLOCK C: PROPORTIONAL SPACING ======
370 REM ESC 1-6 IS A ONE-SHOT GAP BETWEEN TWO CHARACTERS,
375 REM NOT A MODE: SEND ONE AFTER EVERY CHAR TO SPACE A LINE.
380 T$ = "C. PROPORTIONAL  (ESC p/P + ONE-SHOT ESC 1-6 GAPS)": GOSUB 9100
390 PRINT E$;"p";"PROP PICA PACKS TIGHT: illili WWMM jiff"
400 PRINT E$;"P";"PROP ELITE: the quick brown fox jumps a lazy dog"
410 W$ = "spaced": PRINT "gap 0: ";W$;"  gap 3: ";: G$ = "3": GOSUB 2500
420 PRINT "  gap 6: ";: G$ = "6": GOSUB 2500: PRINT E$;"N"
430 REM ====== BLOCK D: LINE FEED PITCH ======
440 T$ = "D. LINE FEED PITCH  (ESC A/B/T nn)": GOSUB 9100
450 PRINT E$;"A";"6 LPI line one (ESC A)"
460 PRINT "6 LPI line two"
470 PRINT E$;"B";"8 LPI line one (ESC B)"
480 PRINT "8 LPI line two"
490 PRINT E$;"T";"30";"T30 = 30/144 inch roomy"
500 PRINT "T30 line two"
510 PRINT E$;"T";"10";"T10 = 10/144 inch tight"
520 PRINT "T10 line two";E$;"A"
530 REM ====== BLOCK E: PRINT DIRECTION ======
540 T$ = "E. DIRECTION  (ESC > UNIDIR / ESC < BIDIR)": GOSUB 9100
550 PRINT E$;"<";"Bidirectional (default): ";S$
560 PRINT E$;">";"Unidirectional (cleaner): ";S$;E$;"<"
570 REM ====== BLOCK F: REPEAT PRINT ======
580 T$ = "F. REPEAT  (ESC R nnn c)": GOSUB 9100
590 PRINT E$;"R";"040";"=";: PRINT
600 PRINT E$;"R";"010";"*";" ten stars, ";E$;"R";"005";"#";" five hashes"
610 PRINT E$;"R";"040";"=";: PRINT
620 REM ====== BLOCK G: PAPER FEED BOTH WAYS ======
630 REM NO ESC X/Y/Z ON THE DMP: UP+DOWN FEEDS PLACE SUPER/
640 REM SUBSCRIPTS. FACTORY SW1-7 MAKES LF END-OF-LINE, SO
650 REM ESC Z H R (CR-ONLY) GUARDS THE HOPS; ESC D H R RESTORES.
660 T$ = "G. REVERSE FEED  (ESC r/f + ESC T, CTRL-_ SKIP)": GOSUB 9100
670 PRINT E$;"ZHR";: REM LF = PURE FEED (SW A-7 OPEN)
680 PRINT E$;"T";"08";"base ";E$;"r";CHR$(10);"UP ";E$;"f";CHR$(10);"base ";E$;"r";CHR$(10);"UP ";E$;"f";CHR$(10);"base";E$;"DHR";E$;"A"
690 PRINT
700 PRINT "3-line skip via CTRL-_ :";CHR$(31);"3";"landed here"
710 GOSUB 9000
720 PRINT E$;"!";"--- END OF COUT BLOCKS ---";E$;Q$
730 PRINT : PRINT
740 REM ====== 8-BIT BLOCKS H-K VIA DIRECT LATCH POKE ======
750 PRINT D$;"PR#0": REM COUT BACK TO SCREEN; STREAM GOES DIRECT
760 PRINT "STREAMING 8-BIT BLOCKS (TABS/ZEROS/GFX/FONT)..."
770 RESTORE
780 READ B: IF B < 0 THEN 800
790 POKE A,B: GOTO 780
800 PRINT "DONE. FORM FEED SENT."
810 PRINT D$;"SAVE /SCREEN.TO.PRINT/APPLE.DMP/TEXT.TEST"
820 END
1000 REM == FULL PRINTABLE ASCII ==
1010 FOR R = 0 TO 2:S = 32 + R * 32:F = S + 31: IF F > 126 THEN F = 126
1020 L$ = "": FOR I = S TO F:L$ = L$ + CHR$(I): NEXT I
1030 PRINT L$
1040 NEXT R
1050 RETURN
2400 REM == OVERSTRIKE W$ ONCE MORE ==
2410 FOR I = 1 TO LEN(W$): PRINT CHR$(8);: NEXT I: PRINT W$;: RETURN
2500 REM == PRINT W$ IN PROP-ELITE, GAP G$ AFTER EVERY CHAR ==
2510 PRINT E$;"P";: FOR I = 1 TO LEN(W$): PRINT MID$(W$,I,1);E$;G$;: NEXT I: PRINT E$;"N";: RETURN
9000 REM == NORMALISE (DMP SAFE SET) ==
9010 PRINT E$;Q$;: REM BOLD OFF
9020 PRINT E$;"Y";: REM UNDERLINE OFF
9030 PRINT CHR$(15);: REM HEADLINE OFF
9040 PRINT E$;"$";: REM ROM FONT
9050 PRINT E$;"N";: REM PICA (POWER-ON DEFAULT)
9060 PRINT E$;"A";: REM 6 LPI
9070 PRINT E$;"f";: REM FORWARD FEED
9080 PRINT E$;"<";: REM BIDIRECTIONAL
9090 PRINT E$;"DHR";: REM END-OF-LINE AT CR,LF,FF (FACTORY)
9095 RETURN
9100 REM == UNDERLINED SECTION TITLE ==
9110 GOSUB 9000
9120 PRINT E$;"X";T$;E$;"Y"
9130 PRINT
9140 RETURN'''

lines = prog.splitlines()
ln, i, per = 7000, 0, 16
while i < len(stream):
    chunk = stream[i:i + per]
    lines.append("%d DATA %s" % (ln, ",".join(str(x) for x in chunk)))
    ln += 10
    i += per

text = "\n".join(lines)
import os
here = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(here, "text-test.bas"), "w").write(text + "\n")
print("lines:", len(lines), " stream bytes:", len(stream) - 1)
