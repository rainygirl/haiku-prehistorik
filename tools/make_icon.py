#!/usr/bin/env python3
"""Builds the application icon from your own copy of the game.

The icon is the roast ham in the thought bubble on the fourth intro screen.
That artwork belongs to Titus, so it is not stored in this repository: the
build runs `Prehistorik --dump-intro` to capture the intro frame from your
game files, and this script traces the ham into an HVIF vector icon.

Usage: make_icon.py <intro.ppm> <out.rdef> [preview.png]

Each same-coloured 4-connected region of pixels becomes one orthogonal
polygon, so the icon keeps the original pixel art exactly. The bubble
around the ham becomes transparent and the grey shadow it casts on the
bubble becomes a translucent shadow. Only the standard library is needed;
a PNG preview is written when Pillow is available.
"""
import sys

# Where the ham sits on the 320x200 intro screen.
CROP = (174, 14, 226, 55)
SHADOW = (0x00, 0x00, 0x00, 0x5a)


def read_ppm(path):
    data = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    pos += 1
    if fields[0] != b'P6' or int(fields[3]) != 255:
        sys.exit('%s: expected a binary PPM (P6, 8 bits)' % path)
    width, height = int(fields[1]), int(fields[2])
    raw = data[pos:pos + width * height * 3]
    return width, height, [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]


def load_sprite(path):
    """Crops the ham and returns rows of RGBA tuples (None = transparent)."""
    width, height, pixels = read_ppm(path)
    if (width, height) != (320, 200):
        sys.exit('%s: expected the 320x200 intro frame, got %dx%d' % (path, width, height))
    x0, y0, x1, y1 = CROP
    w, h = x1 - x0, y1 - y0
    px = [[pixels[(y0 + y) * width + x0 + x] for x in range(w)] for y in range(h)]
    bubble = px[0][0]
    if bubble[0] != bubble[1] or bubble[1] != bubble[2] or bubble[0] < 0xc0:
        sys.exit('%s: this does not look like the thought bubble screen' % path)

    # Bubble pixels reachable from the crop's edge are background.
    outside = [[False] * w for _ in range(h)]
    stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:
        x, y = stack.pop()
        if 0 <= x < w and 0 <= y < h and not outside[y][x] and px[y][x] == bubble:
            outside[y][x] = True
            stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]

    # Grey pixels touching the background are the shadow cast on the bubble.
    def grey(c):
        return c[0] == c[1] == c[2] and c != bubble and c[0] > 0
    shadow = [[False] * w for _ in range(h)]
    stack = [(x, y) for y in range(h) for x in range(w) if outside[y][x]]
    while stack:
        x, y = stack.pop()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not shadow[ny][nx] and not outside[ny][nx] and grey(px[ny][nx]):
                shadow[ny][nx] = True
                stack.append((nx, ny))

    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            if outside[y][x]:
                row.append(None)
            elif shadow[y][x]:
                row.append(SHADOW)
            else:
                row.append(px[y][x] + (0xff,))
        rows.append(row)

    # Trim to the drawn area.
    ys = [y for y in range(h) if any(rows[y])]
    xs = [x for x in range(w) if any(rows[y][x] for y in range(h))]
    return [row[min(xs):max(xs) + 1] for row in rows[min(ys):max(ys) + 1]]


def components(sprite, colour):
    """4-connected regions of one colour, as sets of (x, y)."""
    h, w = len(sprite), len(sprite[0])
    seen, out = set(), []
    for y in range(h):
        for x in range(w):
            if sprite[y][x] != colour or (x, y) in seen:
                continue
            stack, comp = [(x, y)], set()
            seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                comp.add((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and sprite[ny][nx] == colour and (nx, ny) not in seen:
                        seen.add((nx, ny))
                        stack.append((nx, ny))
            out.append(comp)
    return out


def outer_boundary(comp):
    """Traces the outer edge of a pixel region as a list of corner points.
    Holes are ignored: whatever lies inside them is painted afterwards."""
    edges = {}
    for x, y in comp:
        if (x, y - 1) not in comp: edges.setdefault((x, y), []).append((x + 1, y))
        if (x + 1, y) not in comp: edges.setdefault((x + 1, y), []).append((x + 1, y + 1))
        if (x, y + 1) not in comp: edges.setdefault((x + 1, y + 1), []).append((x, y + 1))
        if (x - 1, y) not in comp: edges.setdefault((x, y + 1), []).append((x, y))
    start = min(edges, key=lambda p: (p[1], p[0]))
    loop, cur, prev_dir = [start], start, (1, 0)
    while True:
        options = edges[cur]
        if len(options) == 1:
            nxt = options[0]
        else:
            # at a pinch point prefer turning right to stay on the outside
            def turn_rank(p):
                d = (p[0] - cur[0], p[1] - cur[1])
                return -(prev_dir[0] * d[1] - prev_dir[1] * d[0])
            nxt = sorted(options, key=turn_rank)[0]
        options.remove(nxt)
        prev_dir = (nxt[0] - cur[0], nxt[1] - cur[1])
        cur = nxt
        if cur == start:
            break
        loop.append(cur)
    pts, n = [], len(loop)
    for i in range(n):
        a, b, c = loop[i - 1], loop[i], loop[(i + 1) % n]
        if (b[0] - a[0]) * (c[1] - b[1]) != (b[1] - a[1]) * (c[0] - b[0]):
            pts.append(b)
    return pts


def coord(v):
    v = float(v)
    if -32 <= v <= 95 and v == int(v):
        return bytes([int(v) + 32])
    n = int(round((v + 128) * 102))
    return bytes([0x80 | (n >> 8), n & 0xff])


def build_icon(sprite):
    h, w = len(sprite), len(sprite[0])
    colours = sorted({c for row in sprite for c in row if c}, key=lambda c: (c[3], c[0] + c[1] + c[2]))
    regions = []
    for colour in colours:
        for comp in components(sprite, colour):
            xs = [p[0] for p in comp]
            ys = [p[1] for p in comp]
            area = (max(xs) - min(xs) + 1) * (max(ys) - min(ys) + 1)
            regions.append((colour, area, outer_boundary(comp)))
    # Shadow first; everything else largest-first, so a region sitting inside
    # another region's hole is always painted after it.
    regions.sort(key=lambda r: (r[0][3] == 0xff, -r[1]))

    margin = 1.5
    scale = min((64 - 2 * margin) / w, (64 - 2 * margin) / h)
    ox, oy = (64 - w * scale) / 2, (64 - h * scale) / 2
    paths = [[(round(ox + x * scale, 2), round(oy + y * scale, 2)) for x, y in pts] for _, _, pts in regions]
    if len(paths) > 255 or any(len(p) > 255 for p in paths):
        sys.exit('icon too complex for HVIF: %d paths' % len(paths))

    style_index = {c: i for i, c in enumerate(colours)}
    out = bytearray(b'ncif')
    out.append(len(colours))
    for r, g, b, a in colours:
        out += bytes([3, r, g, b]) if a == 0xff else bytes([1, r, g, b, a])
    out.append(len(paths))
    for pts in paths:
        out += bytes([2 | 8, len(pts)])       # closed, straight lines only
        for x, y in pts:
            out += coord(x) + coord(y)
    out.append(len(regions))
    for i, (colour, _, _) in enumerate(regions):
        out += bytes([10, style_index[colour], 1, i, 0])
    return bytes(out), regions, paths


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sprite = load_sprite(sys.argv[1])
    icon, regions, paths = build_icon(sprite)
    hexstr = icon.hex()
    lines = ['\t$"' + hexstr[i:i + 64] + '"' for i in range(0, len(hexstr), 64)]
    open(sys.argv[2], 'w').write('/* Generated by tools/make_icon.py from the game\'s intro screen. */\n'
                                 'resource vector_icon array {\n%s\n};\n' % '\n'.join(lines))
    print('icon: %d bytes, %d colours, %d paths' % (len(icon), len({r[0] for r in regions}), len(paths)))
    if len(sys.argv) > 3:
        try:
            from PIL import Image, ImageDraw
        except ImportError:
            return
        S = 16
        big = Image.new('RGBA', (64 * S, 64 * S), (0, 0, 0, 0))
        for (colour, _, _), pts in zip(regions, paths):
            layer = Image.new('RGBA', big.size, (0, 0, 0, 0))
            ImageDraw.Draw(layer).polygon([(x * S, y * S) for x, y in pts], fill=colour)
            big = Image.alpha_composite(big, layer)
        big.resize((256, 256), Image.LANCZOS).save(sys.argv[3])


if __name__ == '__main__':
    main()
