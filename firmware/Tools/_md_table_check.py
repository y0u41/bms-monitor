import io, sys, glob, os
ROOT = r"D:/正点原子/个人项目实战/BMS项目设计"
files = sys.argv[1:] or ["Build.md", "AGENTS.md", "Tech_Design.md", "PRD.md",
                         "附录C_设计审查记录.md", "开工执行清单.md", "画板指南_AFE采样板.md"]
bad = 0
tables = 0
for fn in files:
    p = os.path.join(ROOT, fn)
    raw = io.open(p, "rb").read()
    cr = raw.count(b"\r")
    lines = raw.decode("utf-8").split("\n")
    i = 0
    while i < len(lines):
        if lines[i].strip().startswith("|") and i + 1 < len(lines) and \
           lines[i + 1].strip().startswith("|") and set(lines[i + 1].strip()) <= set("|-: "):
            j = i
            rows = []
            while j < len(lines) and lines[j].strip().startswith("|"):
                rows.append((j + 1, lines[j]))
                j += 1
            tables += 1
            counts = {}
            for ln, text in rows:
                cells = text.replace("\\|", "@ESCAPED@").split("|")
                counts[ln] = len(cells)
            hdr = counts[rows[0][0]]
            for ln, c in counts.items():
                if c != hdr:
                    bad += 1
                    print("COLUMN-MISMATCH %s:%d  cols=%d (header=%d)" % (fn, ln, c, hdr))
            i = j
        else:
            i += 1
    if cr:
        print("CR-FOUND %s : %d" % (fn, cr))
print("tables=%d mismatches=%d" % (tables, bad))
