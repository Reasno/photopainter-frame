"""Weekly import of new family portraits into the photo frame library on Home Assistant.

Steps:
1. List the Photos smart album (people filter) and keep portrait photos captured after the watermark.
2. Export them, score sharpness and face quality, drop near-duplicates.
3. Crop to 480x800 around the faces, dither to the six panel colors, save as 24-bit BMP.
4. Reverse-geocode places, append to photos.json, upload BMPs and catalog to Home Assistant.
5. Advance the watermark to the newest capture time that was examined.

The watermark file holds {"capture_seconds": <float>, "updated": <iso time>}; capture_seconds counts
local-time seconds since 1970-01-01, the same unit Photos reports.
"""

from __future__ import annotations

import argparse
import io
import json
import math
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timedelta
from pathlib import Path

from PIL import Image, ImageCms, ImageFilter, ImageOps, ImageStat

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(TOOLS))
from build_catalog import place_name  # noqa: E402

STATE_DIR = Path.home() / ".local/state/photopainter"
WATERMARK = STATE_DIR / "watermark.json"
LOG = STATE_DIR / "weekly.log"
ALBUM = "墨水相框人物"
# Home Assistant host settings, kept outside the repository:
# {"remote": "root@<host>", "remote_dir": "<picture directory on the host>", "ssh_options": ["-o", "..."]}
CONFIG = STATE_DIR / "config.json"
REMOTE = REMOTE_DIR = ""
SSH_OPTS: list[str] = []
# Camera formats only; PNG is almost always a screenshot.
EXTENSIONS = {".jpg", ".jpeg", ".heic", ".heif", ".dng", ".tif", ".tiff"}
MAX_PER_RUN = 40
MAX_PER_DAY = 4
BURST_SECONDS = 180
# Taller than 9:16.5 is a phone screen capture, not a camera photo.
MIN_WIDTH_TO_HEIGHT = 0.55
PALETTE_RGB = (0, 0, 0, 255, 255, 255, 255, 255, 0, 255, 0, 0, 0, 0, 0, 0, 0, 255, 0, 255, 0)


def log(message: str) -> None:
    line = f"{datetime.now():%Y-%m-%d %H:%M:%S} {message}"
    print(line, flush=True)
    with LOG.open("a", encoding="utf-8") as f:
        f.write(line + "\n")


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def ssh(command: str) -> str:
    return run(["ssh", *SSH_OPTS, REMOTE, command]).stdout


def load_watermark() -> float:
    return float(json.loads(WATERMARK.read_text())["capture_seconds"])


def save_watermark(seconds: float) -> None:
    WATERMARK.write_text(json.dumps({"capture_seconds": seconds, "updated": datetime.now().isoformat(timespec="seconds"),
                                     "capture_time": local_time(seconds).isoformat(timespec="seconds")}))


def local_time(seconds: float) -> datetime:
    return datetime(1970, 1, 1) + timedelta(seconds=seconds)


def list_album(work: Path) -> list[dict]:
    out = work / "album.tsv"
    run(["osascript", str(HERE / "list_album.applescript"), ALBUM, str(out)], timeout=1800)
    rows = []
    for line in out.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) < 6:
            continue
        rows.append({
            "id": parts[0], "filename": parts[1], "width": int(parts[2]), "height": int(parts[3]),
            "favorite": parts[4] == "true", "capture_seconds": float(parts[5]),
            "lat": parts[6] if len(parts) > 6 else "", "lon": parts[7] if len(parts) > 7 else "",
        })
    return rows


def export(items: list[dict], work: Path) -> None:
    """Exports rendered JPEGs, one directory per item so identical file names never collide."""
    for item in items:
        target = work / "export" / item["id"].split("/")[0]
        target.mkdir(parents=True, exist_ok=True)
        script = f'''with timeout of 300 seconds
tell application "Photos"
export {{media item id "{item['id']}"}} to (POSIX file "{target}") as alias without using originals
end tell
end timeout'''
        try:
            run(["osascript", "-e", script], timeout=360)
        except subprocess.SubprocessError as error:
            log(f"export failed {item['filename']}: {error}")
            continue
        files = [p for p in target.iterdir() if p.suffix.lower() in {".jpg", ".jpeg"}]
        if files:
            item["path"] = str(files[0])


def analyze_faces(items: list[dict], work: Path) -> None:
    binary = work / "analyze_faces"
    run(["swiftc", "-O", "-o", str(binary), str(HERE / "analyze_faces.swift")], timeout=600)
    out = work / "faces.json"
    run([str(binary), str(out), *[i["path"] for i in items]], timeout=1800)
    faces = {r["path"]: r.get("faces", []) for r in json.loads(out.read_text())}
    for item in items:
        item["faces"] = faces.get(item["path"], [])


def open_photo(path: str) -> Image.Image:
    with Image.open(path) as original:
        image = ImageOps.exif_transpose(original)
        image.load()
        icc = image.info.get("icc_profile")
        if icc:
            try:
                image = ImageCms.profileToProfile(image, ImageCms.ImageCmsProfile(io.BytesIO(icc)),
                                                  ImageCms.createProfile("sRGB"), outputMode="RGB")
            except (ImageCms.PyCMSError, OSError):
                pass
        return image.convert("RGB")


def dhash(image: Image.Image) -> int:
    small = image.convert("L").resize((9, 8), Image.Resampling.LANCZOS)
    px = list(small.get_flattened_data()) if hasattr(small, "get_flattened_data") else list(small.getdata())
    value = 0
    for y in range(8):
        for x in range(8):
            value = (value << 1) | (px[y * 9 + x] > px[y * 9 + x + 1])
    return value


def score(item: dict, image: Image.Image) -> float | None:
    """Returns a quality score, or None when the photo is not good enough for the frame."""
    preview = image.copy()
    preview.thumbnail((320, 480), Image.Resampling.LANCZOS)
    gray = preview.convert("L")
    stat = ImageStat.Stat(gray)
    edges = gray.filter(ImageFilter.FIND_EDGES).crop((2, 2, gray.width - 2, gray.height - 2))
    edge_strength = ImageStat.Stat(edges).rms[0]
    faces = item["faces"]
    if not faces or stat.stddev[0] < 15 or edge_strength < 7:
        return None
    face_quality = max(f["quality"] for f in faces)
    largest_face = max(f["w"] * f["h"] for f in faces)
    if face_quality < 0.3 or largest_face < 0.002:
        return None
    item["dhash"] = dhash(preview)
    return (2.6 * face_quality + min(1, math.log1p(edge_strength) / 4) + min(0.5, math.sqrt(largest_face))
            + 1.4 * item["favorite"] - 0.3 * abs(stat.mean[0] - 125) / 125)


def fit_for_frame(image: Image.Image, faces: list[dict]) -> Image.Image:
    """Crops to 3:5 keeping all faces, or pads with white when the faces do not fit."""
    width, height = image.size
    target_ratio = 480 / 800
    crop_w = min(1.0, target_ratio / (width / height))
    crop_h = min(1.0, (width / height) / target_ratio)
    margin = 0.035
    boxes = [f for f in faces if f["w"] * f["h"] > 0.0015] or faces
    xmin = max(0, min(f["x"] for f in boxes) - margin)
    xmax = min(1, max(f["x"] + f["w"] for f in boxes) + margin)
    ymin = max(0, min(1 - f["y"] - f["h"] for f in boxes) - margin)
    ymax = min(1, max(1 - f["y"] for f in boxes) + margin)
    if crop_w * crop_h < 0.77 or xmax - xmin > crop_w or ymax - ymin > crop_h:
        return ImageOps.pad(image, (480, 800), method=Image.Resampling.LANCZOS, color=(255, 255, 255))
    x = min(max((xmin + xmax - crop_w) / 2, 0), 1 - crop_w)
    y = min(max((ymin + ymax - crop_h) / 2, 0), 1 - crop_h)
    cx, cy = (1 - crop_w) / 2, (1 - crop_h) / 2
    if cx <= xmin and cx + crop_w >= xmax:
        x = cx
    if cy <= ymin and cy + crop_h >= ymax:
        y = cy
    box = (round(x * width), round(y * height), round((x + crop_w) * width), round((y + crop_h) * height))
    return image.crop(box).resize((480, 800), Image.Resampling.LANCZOS)


def dither(image: Image.Image) -> Image.Image:
    palette = Image.new("P", (1, 1))
    palette.putpalette(PALETTE_RGB + (0, 0, 0) * 249)
    return image.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG).convert("RGB")


def geocode(items: list[dict], work: Path) -> dict:
    located = [i for i in items if i["lat"] and i["lon"]]
    if not located:
        return {}
    coords = work / "coords.tsv"
    coords.write_text("".join(f"{i['id']}\t{i['lat']}\t{i['lon']}\n" for i in located), encoding="utf-8")
    binary = work / "geocode_places"
    run(["swiftc", "-O", "-o", str(binary), str(TOOLS / "geocode_places.swift")], timeout=600)
    out = work / "places.json"
    try:
        run([str(binary), str(coords), str(out)], timeout=1800)
        return json.loads(out.read_text(encoding="utf-8"))
    except (subprocess.SubprocessError, OSError, ValueError) as error:
        log(f"geocoding failed, places left empty: {error}")
        return {}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true", help="select and convert, but do not upload or move the watermark")
    args = parser.parse_args()
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    global REMOTE, REMOTE_DIR, SSH_OPTS
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    REMOTE, REMOTE_DIR = config["remote"], config["remote_dir"]
    SSH_OPTS = ["-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=15", *config.get("ssh_options", [])]

    watermark = load_watermark()
    log(f"watermark {local_time(watermark):%Y-%m-%d %H:%M:%S}")
    work = Path(tempfile.mkdtemp(prefix="photopainter_weekly_"))
    try:
        album = list_album(work)
        newest = max([watermark] + [a["capture_seconds"] for a in album])
        fresh = [a for a in album if a["capture_seconds"] > watermark]
        candidates = [a for a in fresh
                      if a["height"] > a["width"] >= 480
                      and a["width"] / a["height"] >= MIN_WIDTH_TO_HEIGHT
                      and Path(a["filename"]).suffix.lower() in EXTENSIONS]
        log(f"album {len(album)} items, {len(fresh)} after watermark, {len(candidates)} portrait photos")
        if not candidates:
            if not args.dry_run:
                save_watermark(newest)
            log("nothing new")
            return 0

        export(candidates, work)
        exported = [c for c in candidates if c.get("path")]
        analyze_faces(exported, work)
        scored = []
        for item in exported:
            image = open_photo(item["path"])
            if image.height <= image.width:
                continue
            value = score(item, image)
            if value is not None:
                item["score"] = value
                scored.append(item)

        remote_catalog = json.loads(ssh(f"cat {REMOTE_DIR}/photos.json"))
        known_ids = {c.get("id") for c in remote_catalog}
        chosen: list[dict] = []
        per_day: dict[str, int] = {}
        for item in sorted(scored, key=lambda i: -i["score"]):
            if item["id"] in known_ids:
                continue
            # Keep the best shot of each burst and of each near-identical scene.
            if any(abs(item["capture_seconds"] - c["capture_seconds"]) < BURST_SECONDS
                   or (item["dhash"] ^ c["dhash"]).bit_count() <= 2
                   or (abs(item["capture_seconds"] - c["capture_seconds"]) < 1800
                       and (item["dhash"] ^ c["dhash"]).bit_count() <= 10) for c in chosen):
                continue
            day = f"{local_time(item['capture_seconds']):%Y-%m-%d}"
            if per_day.get(day, 0) >= MAX_PER_DAY:
                continue
            per_day[day] = per_day.get(day, 0) + 1
            chosen.append(item)
            if len(chosen) >= MAX_PER_RUN:
                break
        log(f"{len(exported)} exported, {len(scored)} passed quality, {len(chosen)} selected")

        places = geocode(chosen, work)
        out_dir = work / "out"
        out_dir.mkdir()
        new_entries = []
        for item in sorted(chosen, key=lambda i: i["capture_seconds"]):
            taken = local_time(item["capture_seconds"])
            name = f"family_{taken:%Y%m%d_%H%M%S}_{item['id'][:8].lower()}.bmp"
            dither(fit_for_frame(open_photo(item["path"]), item["faces"])).save(out_dir / name, format="BMP")
            new_entries.append({"file": name, "taken": taken.isoformat(timespec="seconds"),
                                "place": place_name(places.get(item["id"])), "id": item["id"]})

        if args.dry_run:
            dest = STATE_DIR / "dry_run"
            shutil.rmtree(dest, ignore_errors=True)
            shutil.copytree(out_dir, dest)
            (dest / "new_entries.json").write_text(json.dumps(new_entries, ensure_ascii=False, indent=1), encoding="utf-8")
            log(f"dry run: {len(new_entries)} pictures in {dest}")
            return 0

        if new_entries:
            run(["rsync", "-a", "--no-owner", "--no-group", "-e", "ssh " + " ".join(SSH_OPTS),
                 f"{out_dir}/", f"{REMOTE}:{REMOTE_DIR}/"], timeout=1800)
            catalog_path = work / "photos.json"
            catalog_path.write_text(json.dumps(remote_catalog + new_entries, ensure_ascii=False, indent=1), encoding="utf-8")
            run(["scp", "-q", *SSH_OPTS, str(catalog_path), f"{REMOTE}:{REMOTE_DIR}/photos.json.new"])
            ssh(f"mv {REMOTE_DIR}/photos.json.new {REMOTE_DIR}/photos.json")
            verify = json.loads(ssh(f"cat {REMOTE_DIR}/photos.json"))
            missing = ssh(" ; ".join(f"test -f {REMOTE_DIR}/{e['file']} || echo {e['file']}" for e in new_entries)).split()
            if len(verify) != len(remote_catalog) + len(new_entries) or missing:
                raise RuntimeError(f"upload verification failed, missing={missing}")
        save_watermark(newest)
        log(f"added {len(new_entries)} pictures, library now {len(remote_catalog) + len(new_entries)}; "
            f"watermark -> {local_time(newest):%Y-%m-%d %H:%M:%S}")
        for entry in new_entries:
            log(f"  + {entry['file']} {entry['taken']} {entry['place']}")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
