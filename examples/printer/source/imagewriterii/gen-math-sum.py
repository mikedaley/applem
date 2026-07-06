#!/usr/bin/env python3
# Generator for MATH.SUM  -  ImageWriter II custom-font math demo.
#
# Reproduces the TeX inline-vs-display summation gag:
#   inline:  "This sum <S(i=1..oo)> 1/i looks different than"
#   display: a big roomy   oo
#                          S   1
#                          S   -
#                          S   i
#                         i=1
#
# Emits an Applesoft program that POKEs a raw byte stream straight to the
# SSC (slot 2) ACIA data register ($C0A8 = 49320), bypassing ProDOS COUT.
# That path is 8-bit transparent (no MSB mask, no line-wrap CR injection,
# no CTRL-D command interception) - proven on HardHat.
#
# v2 changes (roominess pass):
#   * 16-column custom cells (ESC +) so glyphs can be BIG - the old 7-col
#     infinity read as truncated at pica density.
#   * Redrawn glyphs from ASCII art (bit0 = TOP dot). Full-height infinity,
#     a proper 16-dot-tall display Sigma (two stacked 8-dot halves), a small
#     separate inline Sigma + inline infinity for the cramped inline look.
#   * Display equation positioned with ESC F (absolute dot column, tracks the
#     pitch) instead of counting spaces - far roomier and rock steady.
#   * Subscript i=1 dropped UNDER the Sigma with CTRL-H backspaces, per spec.

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

# Small inline infinity (7 wide x 6 tall) - rides as a superscript.
INLINE_INF = art([
    ".##.##.",
    "#..#..#",
    "#..#..#",
    "#..#..#",
    "#..#..#",
    ".##.##.",
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

# ------------------------------------------------------------ byte stream -----
stream = []
stream += [27, 0x2B]                       # ESC +  : 16-col cells, clear set
stream += [27, 0x49]                       # ESC I  : begin download
stream += glyph(C_ISIG, INLINE_SIGMA)
stream += glyph(C_IINF, INLINE_INF)
stream += glyph(C_STOP, SIGMA_TOP)
stream += glyph(C_SBOT, SIGMA_BOT)
stream += glyph(C_BINF, BIG_INF)
stream += glyph(C_BAR,  FRAC_BAR)
stream += [4]                              # CTRL-D : end download

def txt(s):
    return [ord(c) for c in s]

CR = [13, 10]
ESC = 27

def escF(n):                               # ESC F nnnn : head to dot column n
    return [ESC, 0x46] + txt("%04d" % n)

# ---- activate custom font + inline sentence (pica, deliberately cramped) -----
stream += [ESC, 0x27]                      # ESC '  : custom font (low ASCII)
stream += [ESC, 0x41]                      # ESC A  : 6 lpi
stream += txt("   This sum ")
stream += [C_ISIG]                         # inline sigma
stream += [ESC, ord('x'), C_IINF]          # superscript infinity (advances 1 cell)
stream += [8]                              # CTRL-H x1 : i=1 starts under the infinity, clear of the sigma
stream += [ESC, ord('y')] + txt("i=1")     # subscript i=1
stream += [ESC, ord('z')]                  # scripts off
stream += txt(" 1/i looks different than")
stream += CR
stream += CR + CR                          # gap before the display equation

# ---- display equation block --------------------------------------------------
# ESC F counts dot columns at the pitch graphics density (pica = 80 dpi), so
# these are absolute, roomy, pitch-steady positions. Tight 8/72" line pitch
# (ESC T 16) makes the two Sigma halves abut into one 16-dot glyph.
SX = 120                                    # sigma left dot-column
FX = 142                                    # fraction column (just right of sigma, clears i=1)
INFX = SX + 1                               # infinity centred over sigma

stream += [ESC, 0x54] + txt("16")           # ESC T 16 : line = 8/72"
stream += escF(INFX) + [C_BINF] + CR                        # L1: infinity
stream += escF(SX) + [C_STOP] + escF(FX) + txt("1") + CR    # L2: sigma-top | numerator 1
stream += escF(SX) + [C_SBOT] + escF(FX) + [C_BAR] + CR     # L3: sigma-bot | fraction bar
# L4: i=1 dropped UNDER the sigma via one backspace (centres it) | denominator i
stream += escF(SX) + [8] + txt("i=1") + escF(FX) + txt("i") + CR

# ---- restore -----------------------------------------------------------------
stream += [ESC, 0x24]                       # ESC $  : custom font off
stream += [ESC, 0x41]                        # ESC A  : 6 lpi
stream += CR + CR + CR                       # feed paper out
stream += [-1]                               # sentinel

# ------------------------------------------------------- emit Applesoft --------
prog = [
 "10  REM  SUM 1/I - IMAGEWRITER II CUSTOM FONT",
 "20  REM  INLINE + DISPLAY SUMMATION, DOWNLOADED GLYPHS",
 "30  A = 49320: REM SSC SLOT2 ACIA TX  ($C0A8)",
 "40  READ B: IF B < 0 THEN 70",
 "50  POKE A,B",
 "60  GOTO 40",
 "70  END",
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
