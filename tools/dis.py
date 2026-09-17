"""Print listing lines within a linear address range: dis.py START END [file]"""
import sys
a = int(sys.argv[1], 16); b = int(sys.argv[2], 16)
f = sys.argv[3] if len(sys.argv) > 3 else "analysis/listing.asm"
for line in open(f):
    tok = line.split()
    if not tok: continue
    if line.startswith("; ---- data") or line.startswith(";   "):
        try:
            addr = int(tok[3].split("-")[0], 16) if line.startswith("; ---- data") else int(tok[1].rstrip(":"), 16)
        except Exception:
            continue
    else:
        try: addr = int(tok[0], 16)
        except ValueError: continue
    if a <= addr < b: sys.stdout.write(line)
