"""Printable black-and-white assembly drawing of DigiDash3's back (parts) side."""
import re, math, csv, sys
from PIL import Image, ImageDraw, ImageFont

PCB, PLACE, OUT = sys.argv[1:4]
t = open(PCB).read()
DPI = 300
PW, PH = int(11 * DPI), int(8.5 * DPI)          # letter landscape
FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"
FONTB = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
X0, X1, Y0, Y1 = 20, 349, 20, 133               # board outline

vals = {}
for r in csv.DictReader(open(PLACE)):
    vals[r["Ref"]] = r["Val"]


def num(s):
    return [float(v) for v in s.split()]


fps = []
for blk in t.split("\n\t(footprint ")[1:]:
    m = re.search(r'\(property "Reference" "([^"]+)"', blk)
    if not m:
        continue
    ref = m.group(1)
    layer = re.match(r'"[^"]*"\s*\(layer "([^"]+)"', blk)
    side = layer.group(1) if layer else "B.Cu"
    a = re.search(r'\n\t\t\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', blk)
    fx, fy, fr = float(a[1]), float(a[2]), float(a[3] or 0)
    r = math.radians(fr)

    def tr(lx, ly):
        return (fx + lx * math.cos(r) + ly * math.sin(r), fy - lx * math.sin(r) + ly * math.cos(r))

    segs, circles, texts = [], [], []
    for g in re.finditer(r'\n\t\t\((fp_line|fp_rect|fp_circle|fp_arc|fp_poly)(.*?)\n\t\t\)', blk, re.S):
        kind, body = g.group(1), g.group(2)
        if '(layer "B.Fab")' not in body and '(layer "F.Fab")' not in body:
            continue
        P = lambda key: num(re.search(r'\(' + key + r' ([-\d. ]+)\)', body).group(1))
        if kind == "fp_line":
            segs.append([tr(*P("start")), tr(*P("end"))])
        elif kind == "fp_rect":
            (ax, ay), (bx, by) = P("start"), P("end")
            segs.append([tr(ax, ay), tr(bx, ay), tr(bx, by), tr(ax, by), tr(ax, ay)])
        elif kind == "fp_circle":
            c, e = P("center"), P("end")
            circles.append((tr(*c), math.hypot(e[0] - c[0], e[1] - c[1])))
        elif kind == "fp_arc":
            s, mm, e = P("start"), P("mid"), P("end")
            # circle through three points, sampled
            (x1, y1), (x2, y2), (x3, y3) = s, mm, e
            d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
            if abs(d) < 1e-9:
                segs.append([tr(*s), tr(*e)]); continue
            ux = ((x1**2 + y1**2) * (y2 - y3) + (x2**2 + y2**2) * (y3 - y1) + (x3**2 + y3**2) * (y1 - y2)) / d
            uy = ((x1**2 + y1**2) * (x3 - x2) + (x2**2 + y2**2) * (x1 - x3) + (x3**2 + y3**2) * (x2 - x1)) / d
            R = math.hypot(x1 - ux, y1 - uy)
            a1, a2, a3 = (math.atan2(y - uy, x - ux) for x, y in (s, mm, e))
            def norm(a): return a % (2 * math.pi)
            sweep = norm(a3 - a1)
            if norm(a2 - a1) > sweep:
                sweep -= 2 * math.pi
            pts = [tr(ux + R * math.cos(a1 + sweep * i / 24), uy + R * math.sin(a1 + sweep * i / 24)) for i in range(25)]
            segs.append(pts)
        elif kind == "fp_poly":
            pts = [tr(*map(float, p)) for p in re.findall(r'\(xy ([-\d.]+) ([-\d.]+)\)', body)]
            segs.append(pts + pts[:1])
    for g in re.finditer(r'\(fp_text user "([^"$]+)"\s*\(at ([-\d.]+) ([-\d.]+)[^)]*\)\s*(?:\(unlocked yes\)\s*)?\(layer "B\.Fab"\)', blk):
        texts.append((g.group(1), tr(float(g.group(2)), float(g.group(3)))))
    pads = []
    for p in re.finditer(r'\(pad "([^"]*)" (\w+) (\w+)\s*\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)\s*\(size ([-\d.]+) ([-\d.]+)\)', blk):
        pads.append((p.group(1), p.group(3), tr(float(p.group(4)), float(p.group(5))), float(p.group(7)), float(p.group(8))))
    fps.append(dict(ref=ref, side=side, segs=segs, circles=circles, texts=texts, pads=pads))


def body_box(f):
    pts = [q for s in f["segs"] for q in s]
    for (cx, cy), R in f["circles"]:
        pts += [(cx - R, cy - R), (cx + R, cy + R)]
    if not pts:
        pts = [p[2] for p in f["pads"]]
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def render(view, scale, title, labels_values):
    """view: (vx0, vx1) in board x range (mirrored view), scale: px per mm."""
    im = Image.new("L", (PW, PH), 255)
    d = ImageDraw.Draw(im)
    vx0, vx1 = view
    ox = (PW - (vx1 - vx0) * scale) / 2
    oy = PH * 0.12 + (PH * 0.84 - (Y1 - Y0) * scale) / 2
    # mirrored: board x larger -> further left
    X = lambda x: ox + (vx1 - x) * scale
    Y = lambda y: oy + (y - Y0) * scale
    lw = max(2, int(scale * 0.18))
    d.rectangle((X(X1), Y(Y0), X(X0), Y(Y1)), outline=0, width=lw + 2)
    for f in fps:
        if not f["side"].startswith("B"):
            continue
        for s in f["segs"]:
            d.line([(X(x), Y(y)) for x, y in s], fill=0, width=lw, joint="curve")
        for (cx, cy), R in f["circles"]:
            d.ellipse((X(cx) - R * scale, Y(cy) - R * scale, X(cx) + R * scale, Y(cy) + R * scale), outline=0, width=lw)
        for name, ptype, (px_, py_), w, h in f["pads"]:
            rr = min(w, h) / 2 * scale * 0.75
            if name == "1" and len(f["pads"]) > 2:
                d.rectangle((X(px_) - rr, Y(py_) - rr, X(px_) + rr, Y(py_) + rr), outline=0, width=max(1, lw // 2))
            else:
                d.ellipse((X(px_) - rr, Y(py_) - rr, X(px_) + rr, Y(py_) + rr), outline=0, width=max(1, lw // 2))
        for txt, (tx, ty) in f["texts"]:
            ft = ImageFont.truetype(FONTB, int(scale * 1.3))
            d.text((X(tx), Y(ty)), txt, font=ft, fill=0, anchor="mm")
    # labels
    for f in fps:
        if not f["side"].startswith("B"):
            continue
        ref = f["ref"]
        if ref.startswith("H") or ref.startswith("NT") or ref.startswith("MH"):
            continue
        bx0, by0, bx1, by1 = body_box(f)
        cx, cy = (bx0 + bx1) / 2, (by0 + by1) / 2
        if not (vx0 - 2 <= cx <= vx1 + 2):
            continue
        w, h = (bx1 - bx0), (by1 - by0)
        vertical = h > w * 1.4
        span = (h if vertical else w)
        size = max(1.3, min(2.6, span / max(len(ref), 3) * 1.1))
        fr_ = ImageFont.truetype(FONTB, int(size * scale))
        lines = [(ref, fr_)]
        if labels_values and ref in vals and vals[ref] not in (ref,):
            v = vals[ref]
            vs = max(1.0, min(size * 0.75, span / max(len(v), 3) * 1.25))
            lines.append((v, ImageFont.truetype(FONT, int(vs * scale))))
        # draw to temp image with white halo
        tw = int(max(d.textlength(s, font=fo) for s, fo in lines)) + 12
        th = sum(fo.size for _, fo in lines) + 8 * len(lines) + 6
        tmp = Image.new("L", (tw, th), 0)
        mask = Image.new("L", (tw, th), 0)
        td, md = ImageDraw.Draw(tmp), ImageDraw.Draw(mask)
        yy = 3
        for s, fo in lines:
            md.text((tw / 2, yy), s, font=fo, fill=255, anchor="mt", stroke_width=max(3, fo.size // 6))
            td.text((tw / 2, yy), s, font=fo, fill=255, anchor="mt")
            yy += fo.size + 8
        if vertical:
            tmp = tmp.rotate(90, expand=True); mask = mask.rotate(90, expand=True)
        black = Image.new("L", tmp.size, 0)
        white = Image.new("L", tmp.size, 255)
        x0p, y0p = int(X(cx) - tmp.size[0] / 2), int(Y(cy) - tmp.size[1] / 2)
        im.paste(white, (x0p, y0p), mask)
        im.paste(black, (x0p, y0p), tmp)
    ft = ImageFont.truetype(FONTB, 60)
    fs = ImageFont.truetype(FONT, 40)
    d.text((PW / 2, 70), title, font=ft, fill=0, anchor="mt")
    d.text((PW / 2, 150), "Back of the board (the parts side), seen from the back. Slot 1 (J31) at the right. "
           "Square pad = pin 1.  + = capacitor positive.  K = diode cathode (band).", font=fs, fill=0, anchor="mt")
    return im


pages = []
full_scale = (PW - 200) / (X1 - X0)
pages.append(render((X0, X1), full_scale, "DigiDash Rev 3: assembly drawing, whole board (page 1 of 4)", False))
s2 = DPI / 25.4 * 1.45
half = (X1 - X0) / 2
# mirrored view: left half of the page = high board x
pages.append(render((X0 + half - 8, X1), s2, "DigiDash Rev 3: left half, power section (page 2 of 4)", True))
pages.append(render((X0, X0 + half + 8), s2, "DigiDash Rev 3: right half, slots and CAN (page 3 of 4)", True))

# parts list page
im = Image.new("L", (PW, PH), 255); d = ImageDraw.Draw(im)
groups = {}
for r in csv.DictReader(open(PLACE)):
    if r["Side"] != "bottom":
        continue
    groups.setdefault((r["Val"], r["Package"]), []).append(r["Ref"])
def refkey(s):
    m = re.match(r'([A-Z]+)(\d+)', s); return (m.group(1), int(m.group(2))) if m else (s, 0)
rows = sorted(groups.items(), key=lambda kv: refkey(sorted(kv[1], key=refkey)[0]))
d.text((PW / 2, 70), "DigiDash Rev 3: parts list, back side (page 4 of 4)", font=ImageFont.truetype(FONTB, 60), fill=0, anchor="mt")
fo = ImageFont.truetype(FONT, 30); fb = ImageFont.truetype(FONTB, 30)
y = 190; col = 0; colw = PW / 2 - 60
for (v, pkg), refs in rows:
    refs = sorted(refs, key=refkey)
    line = ", ".join(refs)
    x = 80 + col * (colw + 40)
    d.rectangle((x, y + 4, x + 26, y + 30), outline=0, width=3)
    d.text((x + 40, y), f"{v}", font=fb, fill=0)
    d.text((x + 40 + 330, y), f"x{len(refs)}", font=fo, fill=0)
    # wrap refs
    words, cur, lines = refs, "", []
    for wd in words:
        trial = (cur + ", " + wd) if cur else wd
        if d.textlength(trial, font=fo) > colw - 470:
            lines.append(cur + ","); cur = wd
        else:
            cur = trial
    lines.append(cur)
    for i, ln in enumerate(lines):
        d.text((x + 40 + 420, y + i * 36), ln, font=fo, fill=0)
    y += 36 * len(lines) + 10
    if y > PH - 120:
        y = 190; col += 1
pages.append(im)
pages[0].save(OUT, save_all=True, append_images=pages[1:], resolution=DPI)
for i, p in enumerate(pages):
    p.save(OUT.replace(".pdf", f"_p{i+1}.png"))
