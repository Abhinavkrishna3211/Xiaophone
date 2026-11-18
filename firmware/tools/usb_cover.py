"""Send the album cover of whatever Windows is playing (Spotify app, a browser, ...) to the
Xiaophone over its USB serial port, so the record shows it while the PC plays into the USB speaker.

    pip install pyserial pillow winsdk
    python tools/usb_cover.py            (finds the Xiaophone's COM port by itself)
    python tools/usb_cover.py COM7

Packet (see usb_speaker.cpp): b"XPC1", u16 meta length, u32 JPEG length, "artist\\ttitle", JPEG.
"""
import asyncio
import io
import json
import re
import struct
import sys
import time
import urllib.parse
import urllib.request

import serial
from PIL import Image, ImageFilter, ImageOps
from serial.tools import list_ports
from winsdk.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as Media
from winsdk.windows.storage.streams import Buffer, InputStreamOptions

VID, PID = 0x303A, 0x8000  # CONFIG_UAC_TUSB_VID / PID


def find_port():
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device
    return None


async def now_playing():
    """(artist, title, thumbnail bytes or None) of the current media session, or None."""
    s = (await Media.request_async()).get_current_session()
    if not s:
        return None
    p = await s.try_get_media_properties_async()
    thumb = None
    if p.thumbnail:
        st = await p.thumbnail.open_read_async()
        buf = Buffer(st.size)
        await st.read_async(buf, buf.capacity, InputStreamOptions.READ_AHEAD)
        thumb = bytes(memoryview(buf))[: buf.length]
    return p.artist or "", p.title or "", thumb


JUNK = re.compile(r"[(\[][^)\]]*(video|lyric|visuali[sz]er|official|audio|full|song|4k|hd)[^)\]]*[)\]]"
                  r"|\b(official|full|lyrics?|lyrical|video|visuali[sz]er)\b.*$", re.I)


def clean(title):
    """'Kalyani (with X) OFFICIAL MUSIC VIDEO | ARJN | ...' -> 'Kalyani'. YouTube titles carry a lot of junk."""
    t = re.split(r"\s[|｜]", title)[0]
    t = JUNK.sub("", t)
    return re.sub(r"\s+", " ", t).strip(" -:")


_art = {}


def album_art(artist, title):
    """600 px album cover from the iTunes search API (free, no key), or None. Cached per song."""
    key = (artist, title)
    if key not in _art:
        _art[key] = None
        t = clean(title)
        for term in (f"{artist} {t}", t):
            try:
                q = urllib.parse.urlencode({"term": term, "entity": "song", "limit": 1})
                with urllib.request.urlopen(f"https://itunes.apple.com/search?{q}", timeout=5) as r:
                    res = json.load(r)["results"]
                if res:
                    url = res[0]["artworkUrl100"].replace("100x100bb", "600x600bb")
                    with urllib.request.urlopen(url, timeout=5) as r:
                        _art[key] = r.read()
                    break
            except (OSError, ValueError, KeyError):
                pass
    return _art[key]


def square(im):
    """Whole picture fitted into a 240 px square over a blurred, zoomed copy of itself: nothing cut off."""
    bg = ImageOps.fit(im, (240, 240)).filter(ImageFilter.GaussianBlur(12))
    fg = ImageOps.contain(im, (240, 240), Image.LANCZOS)
    bg.paste(fg, ((240 - fg.width) // 2, (240 - fg.height) // 2))
    return bg


def to_jpeg(raw, artist="", title=""):
    """240 px baseline JPEG (the firmware's JPEGDEC can't do PNG or progressive). Square art is used as is;
    a wide video thumbnail is swapped for the real album cover, or shown whole if none is found."""
    im = Image.open(io.BytesIO(raw)).convert("RGB")
    if abs(im.width - im.height) > 0.05 * max(im.size):
        art = album_art(artist, title)
        im = ImageOps.fit(Image.open(io.BytesIO(art)).convert("RGB"), (240, 240), Image.LANCZOS) if art else square(im)
    else:
        im = ImageOps.fit(im, (240, 240), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, "JPEG", quality=90, progressive=False)
    return out.getvalue()


def packet(artist, title, jpg):
    meta = f"{artist}\t{title}".encode("utf-8")[:250]
    return b"XPC1" + struct.pack("<HI", len(meta), len(jpg)) + meta + jpg


def main():
    sys.stdout.reconfigure(errors="replace")  # emoji in a title must not kill it on a cp1252 console
    want = sys.argv[1] if len(sys.argv) > 1 else None
    port, last = None, None
    while True:
        try:
            if port is None:
                name = want or find_port()
                if not name:
                    print("waiting for the Xiaophone's USB serial port...", end="\r")
                    time.sleep(2)
                    continue
                port = serial.Serial(name, 115200, timeout=3)
                print(f"connected on {name}                      ")
                last = None  # new connection: send the current cover again
            port.write(b"\n")  # keepalive: fails once the board restarted, so we reconnect and resend the cover
            np = asyncio.run(now_playing())
            if np and np[2]:
                key = (np[0], np[1], hash(np[2]))
                if key != last:
                    port.reset_input_buffer()
                    port.write(packet(np[0], np[1], to_jpeg(np[2], np[0], np[1])))
                    ans = port.readline().decode(errors="replace").strip() or "no answer"
                    print(f"{np[0]} - {np[1]}: {ans}")
                    last = key
            time.sleep(1)
        except (serial.SerialException, OSError) as e:
            print(f"lost the port ({e}), retrying")
            if port:
                port.close()
            port = None
            time.sleep(2)
        except KeyboardInterrupt:
            return


if __name__ == "__main__":
    if sys.argv[1:] == ["--test"]:
        assert clean("KALYANI (with Shreya Ghoshal) OFFICIAL MUSIC VIDEO | ARJN | KDS") == "KALYANI (with Shreya Ghoshal)"
        assert clean("God Mode - Video Song | Suriya's Karuppu") == "God Mode"
        assert clean("Am I Dreaming (Visualizer)") == "Am I Dreaming"
        assert clean("Veezha Veera Lyrical (Tamil) | OM") == "Veezha Veera"
        assert square(Image.new("RGB", (150, 83))).size == (240, 240)
        print("ok")
    else:
        main()
