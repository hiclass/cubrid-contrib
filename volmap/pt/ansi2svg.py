"""ansi2svg.py - render a pty capture as SVG.

Why SVG rather than HTML text: the map uses braille glyphs (U+2800..28FF) for the
fill ramp, and the common monospace fonts (SF Mono, Menlo, Consolas) do not carry
them - the browser substitutes boxes.  Drawing the braille dots as rectangles
removes the font dependency entirely, so the figure looks the same everywhere.

ASCII text is still drawn as text; every monospace font has it.

    python3 ansi2svg.py cap.bin ROWS COLS out.svg
"""
import re
import sys

from ansi2html import simulate, c256          # reuse the verified terminal model

CW = 7.0          # cell width  (px)
CH = 13.0         # cell height (px)
PAD = 10.0
FG = '#d7dae0'
BG = '#000000'

# Braille dot layout: bit0..7 -> (column, row) in a 2x4 grid.
DOT = {0: (0, 0), 1: (0, 1), 2: (0, 2), 3: (1, 0),
       4: (1, 1), 5: (1, 2), 6: (0, 3), 7: (1, 3)}
DOT_W = CW * 0.30
DOT_H = CH * 0.17


def esc(s):
    return (s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;'))


def braille_path(cp):
    """Sub-path for one braille cell, relative to its top-left corner."""
    bits = cp - 0x2800
    d = []
    for b in range(8):
        if not (bits & (1 << b)):
            continue
        col, row = DOT[b]
        x = (CW * 0.12) + col * (CW * 0.42)
        y = (CH * 0.10) + row * (CH * 0.21)
        d.append(f'M{x:.1f} {y:.1f}h{DOT_W:.0f}v{DOT_H:.0f}h-{DOT_W:.0f}z')
    return ''.join(d)


_BR_CACHE = {}


def braille(cx, cy, cp, fill):
    """One braille cell as a single path, translated into place."""
    d = _BR_CACHE.get(cp)
    if d is None:
        d = _BR_CACHE[cp] = braille_path(cp)
    if not d:
        return ''
    return (f'<g transform="translate({cx:.1f},{cy:.1f})">'
            f'<path d="{d}" fill="{fill}"/></g>')


def render(grid, rows, cols):
    w = cols * CW + PAD * 2
    h = rows * CH + PAD * 2
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.0f} {h:.0f}" '
           f'width="{w:.0f}" height="{h:.0f}" font-family="monospace" '
           f'font-size="{CH * 0.80:.1f}" shape-rendering="crispEdges">',
           f'<rect width="{w:.0f}" height="{h:.0f}" fill="{BG}"/>']

    # 1) background runs
    for r in range(rows):
        c = 0
        while c < cols:
            cell = grid[r][c]
            bg = cell.bg
            if bg is None:
                c += 1
                continue
            n = 1
            while c + n < cols and grid[r][c + n].bg == bg:
                n += 1
            x = PAD + c * CW
            y = PAD + r * CH
            out.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{n * CW:.2f}" '
                       f'height="{CH:.2f}" fill="{bg}"/>')
            c += n

    # 2) glyphs - braille as dots, everything else as text
    bypath = {}
    for r in range(rows):
        y = PAD + r * CH
        run = []           # (x, text, fill) accumulated for one text run
        cur = None
        buf = ''
        bx = 0.0
        for c in range(cols):
            cell = grid[r][c]
            ch = cell.ch
            fg = cell.fg or FG
            if cell.rev:
                fg = cell.bg or BG
            o = ord(ch)
            if 0x2800 <= o <= 0x28FF:
                if buf:
                    run.append((bx, buf, cur))
                    buf = ''
                d = _BR_CACHE.get(o)
                if d is None:
                    d = _BR_CACHE[o] = braille_path(o)
                if d:
                    ox, oy = PAD + c * CW, y
                    bypath.setdefault(fg, []).append(
                        re.sub(r'M([\d.]+) ([\d.]+)',
                               lambda m: 'M%.0f %.0f' % (float(m.group(1)) + ox,
                                                         float(m.group(2)) + oy), d))
                continue
            if ch == ' ':
                if buf:
                    run.append((bx, buf, cur))
                    buf = ''
                continue
            if buf and fg == cur:
                buf += ch
            else:
                if buf:
                    run.append((bx, buf, cur))
                buf = ch
                cur = fg
                bx = PAD + c * CW
        if buf:
            run.append((bx, buf, cur))
        for x, text, fill in run:
            out.append(f'<text x="{x:.2f}" y="{y + CH * 0.78:.2f}" '
                       f'fill="{fill}" xml:space="preserve">{esc(text)}</text>')

    for fill, ds in bypath.items():
        out.append(f'<path fill="{fill}" d="{"".join(ds)}"/>')
    out.append('</svg>')
    return '\n'.join(out)


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 1
    data = open(sys.argv[1], 'rb').read()
    rows, cols = int(sys.argv[2]), int(sys.argv[3])
    grid = simulate(data, rows, cols)
    open(sys.argv[4], 'w', encoding='utf-8').write(render(grid, rows, cols))
    print(f'wrote {sys.argv[4]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
