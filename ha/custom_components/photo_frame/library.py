"""Photo selection, status bar rendering and frame state."""

from __future__ import annotations

import io
import json
import logging
import random
import threading
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

_LOGGER = logging.getLogger(__name__)

WIDTH, HEIGHT = 480, 800
BAR_HEIGHT = 40
FONT_SIZE = 24
MARGIN = 10
LOW_BATTERY = 15

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
RED = (255, 0, 0)
GREEN = (0, 255, 0)


@dataclass(frozen=True)
class Photo:
    file: str
    taken: date | None
    place: str


@dataclass
class FrameState:
    """Most recent report from the frame."""

    updated: datetime | None = None
    battery: int | None = None
    charging: bool = False
    photo: Photo | None = None
    image: bytes | None = None


class PhotoLibrary:
    """Pictures in `photo_dir` described by `photos.json`; each is shown once before any repeats."""

    def __init__(self, photo_dir: Path, font: Path, storage_dir: Path) -> None:
        self._dir = photo_dir
        self._font_path = font
        self._deck_path = storage_dir / "photo_frame.deck.json"
        self._state_path = storage_dir / "photo_frame.state.json"
        self._lock = threading.Lock()
        self._photos: dict[str, Photo] = {}
        self._font: ImageFont.FreeTypeFont | None = None
        self.state = FrameState()

    @property
    def count(self) -> int:
        return len(self._photos)

    def load(self) -> None:
        catalog = json.loads((self._dir / "photos.json").read_text(encoding="utf-8"))
        photos = {}
        for item in catalog:
            if not (self._dir / item["file"]).is_file():
                continue
            taken = date.fromisoformat(item["taken"][:10]) if item.get("taken") else None
            photos[item["file"]] = Photo(item["file"], taken, item.get("place", ""))
        self._photos = photos
        self._font = ImageFont.truetype(str(self._font_path), FONT_SIZE)
        self._load_state()

    def render_next(self, battery: int | None, charging: bool) -> tuple[bytes, Photo]:
        with self._lock:
            photo = self._photos[self._draw()]
            image = self._compose(photo, battery, charging)
            out = io.BytesIO()
            image.save(out, format="BMP")
            png = io.BytesIO()
            image.save(png, format="PNG")
            self.state = FrameState(datetime.now(timezone.utc), battery, charging, photo, png.getvalue())
            self._save_state()
            return out.getvalue(), photo

    def _compose(self, photo: Photo, battery: int | None, charging: bool) -> Image.Image:
        image = Image.open(self._dir / photo.file).convert("RGB")
        if image.size != (WIDTH, HEIGHT):
            image = image.resize((WIDTH, HEIGHT), Image.Resampling.NEAREST)
        self._draw_bar(image, photo, battery, charging)
        return image

    def _draw(self) -> str:
        deck: list[str] = []
        try:
            deck = json.loads(self._deck_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            pass
        deck = [name for name in deck if name in self._photos]
        if not deck:
            deck = list(self._photos)
            random.shuffle(deck)
        name = deck.pop()
        self._deck_path.parent.mkdir(parents=True, exist_ok=True)
        self._deck_path.write_text(json.dumps(deck), encoding="utf-8")
        return name

    def _save_state(self) -> None:
        state = self.state
        data = {
            "updated": state.updated.isoformat() if state.updated else None,
            "battery": state.battery,
            "charging": state.charging,
            "photo": state.photo.file if state.photo else None,
        }
        self._state_path.write_text(json.dumps(data), encoding="utf-8")

    def _load_state(self) -> None:
        try:
            data = json.loads(self._state_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        photo = self._photos.get(data.get("photo") or "")
        state = FrameState(
            datetime.fromisoformat(data["updated"]) if data.get("updated") else None,
            data.get("battery"),
            bool(data.get("charging")),
            photo,
        )
        if photo:
            png = io.BytesIO()
            self._compose(photo, state.battery, state.charging).save(png, format="PNG")
            state.image = png.getvalue()
        self.state = state

    def _draw_bar(self, image: Image.Image, photo: Photo, battery: int | None, charging: bool) -> None:
        draw = ImageDraw.Draw(image)
        draw.fontmode = "1"
        top = HEIGHT - BAR_HEIGHT
        draw.rectangle((0, top, WIDTH - 1, HEIGHT - 1), fill=WHITE)
        draw.line((0, top, WIDTH - 1, top), fill=BLACK)

        right = WIDTH - MARGIN
        if battery is not None or charging:
            right = self._draw_battery(draw, top, right, battery, charging) - 12

        parts = []
        if photo.taken:
            parts.append(f"{photo.taken.year}.{photo.taken.month}.{photo.taken.day}")
        if photo.place:
            parts.append(photo.place)
        text = self._fit("  ".join(parts), right - MARGIN)
        draw.text((MARGIN, self._text_y(top)), text, font=self._font, fill=BLACK)

    def _draw_battery(self, draw: ImageDraw.ImageDraw, top: int, right: int, battery: int | None, charging: bool) -> int:
        color = RED if battery is not None and battery < LOW_BATTERY and not charging else BLACK
        label = "充电" if charging else ""
        if battery is not None:
            label = f"{label} {battery}%".strip()
        x = right - int(draw.textlength(label, font=self._font))
        draw.text((x, self._text_y(top)), label, font=self._font, fill=color)

        body_w, body_h = 30, 16
        bx1 = x - 8
        bx0 = bx1 - body_w
        by0 = top + (BAR_HEIGHT - body_h) // 2
        by1 = by0 + body_h
        draw.rectangle((bx0, by0, bx1, by1), outline=color, width=2)
        draw.rectangle((bx1 + 1, by0 + 5, bx1 + 3, by1 - 5), fill=color)
        if battery:
            fill_w = max(1, round((body_w - 6) * battery / 100))
            draw.rectangle((bx0 + 3, by0 + 3, bx0 + 2 + fill_w, by1 - 3), fill=GREEN if charging else color)
        return bx0

    def _text_y(self, top: int) -> int:
        ascent, descent = self._font.getmetrics()
        return top + 1 + (BAR_HEIGHT - 1 - (ascent + descent)) // 2

    def _fit(self, text: str, width: int) -> str:
        if self._font.getlength(text) <= width:
            return text
        while text and self._font.getlength(text + "…") > width:
            text = text[:-1]
        return text.rstrip() + "…"
