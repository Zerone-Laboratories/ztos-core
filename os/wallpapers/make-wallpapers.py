#!/usr/bin/env python3
"""make-wallpapers.py ZIP OUT

Turns the Windows Phone / Lumia wallpaper pack (the OneDrive zip) into the
set ztOS ships: one folder per series under OUT/backgrounds/ztos, the
original photos unchanged, and a small cover-cropped copy of each in
.thumbs/<series>/ for the picker. Photos that appear in more than one
series (the Lumia Icon set repeats the Lumia 1520 ones) are kept once, in
the first series they come in. The Lumia Cyan folder keeps its 1080x1920
photos only; the other sizes are the same photos smaller.

The output is not committed: the photos are Microsoft's and Nokia's, and the
set is 150 MB. The installer copies OUT/backgrounds/ztos to
/usr/local/share/backgrounds/ztos.

Needs Python 3 with Pillow. The archive's WPSounds.zip (sounds) is skipped.
"""
import hashlib
import os
import re
import shutil
import sys
import tempfile
import zipfile

from PIL import Image, ImageOps

# series folder on the phone, and where its photos sit in the archive
SERIES = [
    ("Lumia Cyan", "Lumia Cyan Wallpapers/backgrounds/1080x1920"),
    ("Lumia Cyan lock screens", "Lumia Cyan Wallpapers/lockscreen/1080x1920"),
    ("Lumia 1520", "Lumia 1520 Wallpaper"),
    ("Lumia 1020", "Lumia 1020 Wallpapers"),
    ("Lumia 930", "Lumia 930 Wallpapers"),
    ("Lumia 928", "Lumia 928 Wallpapers"),
    ("Lumia 925", "Lumia 925 Wallpapers"),
    ("Lumia 640", "Lumia 640 Wallpapers"),
    ("Lumia Icon", "Lumia Icon Wallpapers"),
    ("Nokia 3, 5 and 6", "Nokia 3, 5, and 6 Wallpapers"),
]
THUMB = (216, 384)   # the picker's tiles are drawn at 2x their size


def main(zip_path, out):
    stage = tempfile.mkdtemp(prefix="wallpapers-")
    try:
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(stage)
        root = os.path.join(out, "backgrounds", "ztos")
        shutil.rmtree(os.path.join(out, "backgrounds"), ignore_errors=True)
        seen = set()
        count = {}
        for series, folder in SERIES:
            src_dir = os.path.join(stage, folder)
            if not os.path.isdir(src_dir):
                sys.exit("missing in the archive: " + folder)
            dst = os.path.join(root, series)
            thumbs = os.path.join(root, ".thumbs", series)
            os.makedirs(dst)
            os.makedirs(thumbs)
            for name in sorted(os.listdir(src_dir), key=str.lower):
                if not name.lower().endswith((".jpg", ".jpeg")):
                    continue
                path = os.path.join(src_dir, name)
                with open(path, "rb") as f:
                    digest = hashlib.sha256(f.read()).hexdigest()
                if digest in seen:
                    continue
                seen.add(digest)
                # the picker and the file list use the name as it is; a slash would split it
                clean = re.sub(r"[\\/]", "-", name)
                shutil.copyfile(path, os.path.join(dst, clean))
                make_thumb(path, os.path.join(thumbs, clean + ".jpg"))  # the whole name: .jpg and .jpeg twins differ
                count[series] = count.get(series, 0) + 1
        for series, _ in SERIES:
            print("%4d  %s" % (count.get(series, 0), series))
        print("%4d  total" % sum(count.values()))
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def make_thumb(src, dst):
    """cover-cropped to THUMB, from the middle, upright"""
    im = ImageOps.exif_transpose(Image.open(src)).convert("RGB")
    k = max(THUMB[0] / im.width, THUMB[1] / im.height)
    im = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
    left = (im.width - THUMB[0]) // 2
    top = (im.height - THUMB[1]) // 2
    im.crop((left, top, left + THUMB[0], top + THUMB[1])).save(dst, quality=82)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
