#!/usr/bin/env python3
# Generator for MATH.SUM  -  ImageWriter I custom-font math demo.
#
# Reproduces the TeX inline-vs-display summation gag on the ORIGINAL
# ImageWriter (1983):
#   inline:  "This sum <S(i=1..oo)> 1/i looks different than"
#   display: a big roomy   oo
#                          S   1
#                          S   -
#                          S   i
#                         i=1
#
# The IW-I has NO super/subscript commands (ESC x/y/z arrived with the
# IW-II) and no half-height mode. The period-correct substitute -- called
# out in the IW-I User's Manual itself (Part I, Ch.4 intro, p.33: "Feed
# paper both up and down, permitting the generation of mathematical
# formulas and the placement of subscripts and superscripts") -- is
# half-line REVERSE and FORWARD feeds:
#     ESC T nn   line pitch nn/144"      (Ch.4, line feed pitch)
#     ESC r/f    reverse / forward feed  (Ch.4, "Reverse line feeding")
#     LF         one feed at that pitch
# Raised limits print after a reverse hop, dropped limits after forward
# hops; a matching reverse hop returns to the baseline.
#
# Because there is no half-height mode, the subscript "i=1" uses small
# DOWNLOADED glyphs (mini i, =, 1) so the limits read script-sized, like
# the IW-II original. Custom chars: ESC + (16-col bank), ESC I..CTRL-D
# download (width code 0x40+n = top 8 wires), ESC ' select / ESC $ off --
# all present on the IW-I (Manual Ch.5; same wire format as the IW-II).
#
# Emits an Applesoft program that POKEs the raw byte stream straight to
# the SSC (slot 2) ACIA data register ($C0A8 = 49320), bypassing ProDOS
# COUT. That path is 8-bit transparent (no MSB mask, no CTRL-D
# interception) - proven on HardHat.

# --- glyph helper: ASCII-art rows (top->bottom) -> column bytes (bit0=top) ---
def art(rows):
    h = len(rows)                       # dot rows (<=8 for one cell)
    w = max(len(r) for r in rows)
    cols = []
    for x in range(w):
        b = 0
        for y in range(h):
            r = rows[y]
            if x < len(r) and r[x] == '#':
                b |= (1 << y)           # bit y ; bit0 = top row
        cols.append(b)
    return cols

def glyph(key, cols):                   # width code: A..P = 1..16 columns
    return [key, 0x40 + len(cols)] + list(cols)

# ---------------------------------------------------------------- glyph art ---
# Small inline capital sigma (7 wide x 8 tall) - stays cramped for the gag.
INLINE_SIGMA = art([
    "#######",
    "#......",
    ".#.....",
    "..#....",
    "..#....",
    ".#.....",
    "#......",
    "#######",
])

# Small inline infinity (7 wide x 6 tall) - raised by a reverse half-feed.
INLINE_INF = art([
    ".##.##.",
    "#..#..#",
    "#..#..#",
    "#..#..#",
    "#..#..#",
    ".##.##.",
])

# Mini script-sized subscript chars (6 tall) - the IW-I cannot shrink its
# ROM font, so the "i=1" limit is downloaded at script size.
MINI_I = art([
    ".#.",
    "...",
    "##.",
    ".#.",
    ".#.",
    "###",
])
MINI_EQ = art([
    "....",
    "####",
    "....",
    "####",
    "....",
])
MINI_1 = art([
    ".#.",
    "##.",
    ".#.",
    ".#.",
    ".#.",
    "###",
])

# Big DISPLAY sigma, 16 dots tall x 12 wide, 2-dot strokes so it reads bold.
# Split into two 8-dot halves that abut at the vertex (rows 7/8).
SIGMA_16 = [
    "############",  # 0  top bar
    "##..........",  # 1
    ".##.........",  # 2  upper stroke sloping down-right (2-dot thick)
    "..##........",  # 3
    "...##.......",  # 4
    "....##......",  # 5
    ".....##.....",  # 6
    "......##....",  # 7  -> end of TOP half
    "......##....",  # 8  vertex (vertical centre)
    ".....##.....",  # 9
    "....##......",  # 10 lower stroke sloping down-left
    "...##.......",  # 11
    "..##........",  # 12
    ".##.........",  # 13
    "##..........",  # 14
    "############",  # 15 bottom bar
]
SIGMA_TOP = art(SIGMA_16[0:8])
SIGMA_BOT = art(SIGMA_16[8:16])

# Big DISPLAY infinity, 13 wide x 7 tall - two clear loops, NOT truncated.
BIG_INF = art([
    "..##.....##..",
    ".#..#...#..#.",
    "#....#.#....#",
    "#.....#.....#",
    "#....#.#....#",
    ".#..#...#..#.",
    "..##.....##..",
])

# Fraction bar, 12 wide x 2 tall (rides at vertical centre of the cell).
FRAC_BAR = art([
    "",
    "",
    "",
    "############",
    "############",
])

# glyph code assignments (spare punctuation; undefined codes fall to ROM)
C_ISIG = 92     # '\'  inline sigma (small)
C_IINF = 123    # '{'  inline infinity (small)
C_STOP = 91     # '['  big sigma - top half
C_SBOT = 93     # ']'  big sigma - bottom half
C_BINF = 126    # '~'  big infinity
C_BAR  = 95     # '_'  fraction bar
C_MI   = 94     # '^'  mini i
C_MEQ  = 96     # '`'  mini =
C_M1   = 124    # '|'  mini 1

# ------------------------------------------------------------ byte stream -----
stream = []
stream += [27, 0x4E]                       # ESC N  : pica 10 cpi (80 dpi graphics)
stream += [27, 0x2B]                       # ESC +  : 16-col custom cells, clear set
stream += [27, 0x49]                       # ESC I  : begin download
stream += glyph(C_ISIG, INLINE_SIGMA)
stream += glyph(C_IINF, INLINE_INF)
stream += glyph(C_STOP, SIGMA_TOP)
stream += glyph(C_SBOT, SIGMA_BOT)
stream += glyph(C_BINF, BIG_INF)
stream += glyph(C_BAR,  FRAC_BAR)
stream += glyph(C_MI,   MINI_I)
stream += glyph(C_MEQ,  MINI_EQ)
stream += glyph(C_M1,   MINI_1)
stream += [4]                              # CTRL-D : end download

def txt(s):
    return [ord(c) for c in s]

CR = [13, 10]
ESC = 27

def escT(n):                               # ESC T nn : line pitch nn/144"
    return [ESC, 0x54] + txt("%02d" % n)

def escF(n):                               # ESC F nnnn : head to dot column n
    return [ESC, 0x46] + txt("%04d" % n)

REV = [ESC, 0x72]                          # ESC r : reverse feeding
FWD = [ESC, 0x66]                          # ESC f : forward feeding
LF  = [10]

# ---- activate custom font + inline sentence (pica, deliberately cramped) -----
# Stacked limits with NO script mode: hop up 8/144" for the raised infinity,
# hop down 20/144" for the dropped i=1, hop back up 12/144" to the baseline.
# Net paper motion is zero; CR is withheld until the line is fully built.
# CRITICAL: the power-on default inserts a CR before every LF (ESC l 0,
# Table A-16), which would fling the head to the left margin mid-hop.
# ESC l 1 makes LF a pure paper feed for the hops; ESC l 0 restores after.
stream += [ESC, 0x27]                      # ESC '  : custom font (low ASCII)
stream += [ESC, 0x41]                      # ESC A  : 6 lpi
stream += CR                               # blank line: headroom for the raised limit
stream += txt("   This sum ")
stream += [C_ISIG]                         # inline sigma
stream += [ESC, 0x6C, 0x31]                # ESC l 1 : LF without CR insertion
stream += escT(8)  + REV + LF + [C_IINF]   # up 8/144 -> superscript infinity
stream += [8]                              # CTRL-H : back one cell, under the infinity
stream += escT(20) + FWD + LF + [C_MI, C_MEQ, C_M1]   # down 20/144 -> subscript i=1
stream += escT(12) + REV + LF              # up 12/144 -> back on the baseline
stream += FWD                              # restore forward feeding
stream += [ESC, 0x6C, 0x30]                # ESC l 0 : CR before LF again (default)
stream += txt(" 1/i looks different than")
stream += [ESC, 0x41]                      # ESC A : 6 lpi (clears the 12/144 pitch)
stream += CR
stream += CR + CR                          # gap before the display equation

# ---- display equation block --------------------------------------------------
# ESC F counts dot columns at the pitch graphics density (pica = 80 dpi), so
# these are absolute, roomy, pitch-steady positions. Tight 8/72" line pitch
# (ESC T 16) makes the two Sigma halves abut into one 16-dot glyph.
SX = 120                                    # sigma left dot-column
FX = 142                                    # fraction column (just right of sigma, clears i=1)
INFX = SX + 1                               # infinity centred over sigma

stream += escT(16)                          # ESC T 16 : line = 8/72"
stream += escF(INFX) + [C_BINF] + CR                        # L1: infinity
stream += escF(SX) + [C_STOP] + escF(FX + 2) + txt("1") + CR  # L2: sigma-top | numerator 1
# L3 feeds 24/144 into L4 (escT before its CR) so the i=1 limit clears the
# sigma's bottom bar instead of touching it.
stream += escF(SX) + [C_SBOT] + escF(FX) + [C_BAR] + escT(24) + CR  # L3: sigma-bot | fraction bar
# L4: i=1 centred UNDER the sigma (absolute ESC F - the IW-I backspace steps
# by the LAST advance, a 16-col custom cell here, which lands too far left)
stream += escF(SX - 6) + txt("i=1") + escF(FX + 2) + txt("i") + CR

# ---- restore -----------------------------------------------------------------
stream += [ESC, 0x24]                       # ESC $  : custom font off
stream += [ESC, 0x41]                       # ESC A  : 6 lpi
stream += CR + CR + CR                      # feed paper out
stream += [-1]                              # sentinel

# ------------------------------------------------------- emit Applesoft --------
prog = [
 "10  REM  SUM 1/I - IMAGEWRITER I CUSTOM FONT",
 "20  REM  NO ESC X/Y/Z ON THE IW-I: LIMITS STACKED WITH",
 "30  REM  ESC T + ESC R/F HALF-LINE FEEDS (MANUAL CH.4)",
 "40  A = 49320: REM SSC SLOT2 ACIA TX  ($C0A8)",
 "50  READ B: IF B < 0 THEN 80",
 "60  POKE A,B",
 "70  GOTO 50",
 "80  PRINT CHR$(4);\"SAVE /SCREEN.TO.PRINT/IMAGEWRITERI/MATH.SUM\"",
 "90  END",
]
ln, i, per = 100, 0, 16
while i < len(stream):
    chunk = stream[i:i + per]
    prog.append("%d DATA %s" % (ln, ",".join(str(x) for x in chunk)))
    ln += 10
    i += per

text = "\n".join(prog)
import os
here = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(here, "math-sum.bas"), "w").write(text + "\n")
print(text)
print("\n; stream length =", len(stream) - 1, "bytes")
