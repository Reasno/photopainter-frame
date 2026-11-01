"""Build photos.json for the Home Assistant photo_frame integration.

Inputs: the picture manifest (file name, capture time, Photos library id) and reverse-geocoded
address fields keyed by library id. Output: [{"file", "taken", "place"}].
"""

import argparse
import json
from pathlib import Path

MAX_DETAIL_CHARS = 7
REGION_SUFFIXES = ("特别行政区", "市")


def short_city(fields):
    city = fields.get("city") or fields.get("province") or ""
    for suffix in REGION_SUFFIXES:
        if city.endswith(suffix) and len(city) > len(suffix) + 1:
            return city[: -len(suffix)]
    return city


def place_name(fields):
    if not fields:
        return ""
    city = short_city(fields)
    if fields.get("iso") not in ("CN", "HK", "MO", "TW"):
        parts = [fields.get("country", ""), city]
    else:
        interests = [i for i in fields.get("interest", []) if len(i) <= MAX_DETAIL_CHARS]
        street = fields.get("name", "")
        if not interests and street and len(street) <= MAX_DETAIL_CHARS and not any(c.isdigit() for c in street):
            detail = street
        else:
            detail = interests[0] if interests else fields.get("area", "")
        for prefix in (fields.get("city", ""), city):
            if prefix and detail.startswith(prefix) and len(detail) > len(prefix) + 1:
                detail = detail[len(prefix):]
                break
        parts = [city, detail]
    seen = []
    for part in parts:
        if part and part not in seen and not any(part in s for s in seen):
            seen.append(part)
    return " ".join(seen)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("places", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    places = json.loads(args.places.read_text(encoding="utf-8"))
    catalog = []
    for row in manifest:
        catalog.append(
            {
                "file": row["文件名"],
                "taken": row["拍摄时间"],
                "place": place_name(places.get(row["图库标识"])),
            }
        )
    args.output.write_text(json.dumps(catalog, ensure_ascii=False, indent=1), encoding="utf-8")
    with_place = sum(1 for c in catalog if c["place"])
    print(f"{len(catalog)} pictures, {with_place} with place")


if __name__ == "__main__":
    main()
