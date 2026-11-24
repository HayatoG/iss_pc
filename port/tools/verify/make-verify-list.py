"""Reference list of a known-good CUSA00223 1.07 game folder's art/: path, size, sha256 per file."""
import hashlib, json, os, sys, time
game, out = sys.argv[1], sys.argv[2]
art = os.path.join(game, "art")
files = []
start = time.time(); total = 0
for dirpath, dirnames, names in os.walk(art):
    dirnames.sort()
    for name in sorted(names):
        full = os.path.join(dirpath, name)
        h = hashlib.sha256()
        with open(full, "rb") as f:
            for block in iter(lambda: f.read(8 << 20), b""):
                h.update(block)
        size = os.path.getsize(full); total += size
        files.append([os.path.relpath(full, game).replace("\\", "/"), size, h.hexdigest()])
title = sys.argv[3] if len(sys.argv) > 3 else "CUSA00223"
doc = {"format": "iss-game-files", "version": 1, "title_id": title, "app_ver": "01.07",
       "note": f"art/ of a complete {title} 1.07 game folder (base game + update 1.07)", "files": files}
with open(out, "w", encoding="utf-8", newline="\n") as f:
    json.dump(doc, f, separators=(",", ":"))
print(len(files), "files", round(total / 2**30, 2), "GiB", round(time.time() - start), "s")
