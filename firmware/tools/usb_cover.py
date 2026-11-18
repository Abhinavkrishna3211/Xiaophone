"""Send the album cover of whatever Windows is playing (Spotify app, a browser, ...) to the
Xiaophone over its USB serial port, so the record shows it while the PC plays into the USB speaker.

    pip install pyserial pillow winsdk
    python tools/usb_cover.py            (finds the Xiaophone's COM port by itself)
    python tools/usb_cover.py COM7

Packet (see usb_speaker.cpp): b"XPC1", u16 meta length, u32 JPEG length, "artist\\ttitle", JPEG.
"""
import asyncio
import io
import struct
import sys
import time

import serial
from PIL import Image, ImageOps
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


def to_jpeg(raw):
    """Centre square, 240 px, baseline JPEG: the firmware's JPEGDEC can't do PNG or progressive."""
    im = ImageOps.fit(Image.open(io.BytesIO(raw)).convert("RGB"), (240, 240), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, "JPEG", quality=90, progressive=False)
    return out.getvalue()


def packet(artist, title, jpg):
    meta = f"{artist}\t{title}".encode("utf-8")[:250]
    return b"XPC1" + struct.pack("<HI", len(meta), len(jpg)) + meta + jpg


def main():
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
            np = asyncio.run(now_playing())
            if np and np[2]:
                key = (np[0], np[1], hash(np[2]))
                if key != last:
                    port.reset_input_buffer()
                    port.write(packet(np[0], np[1], to_jpeg(np[2])))
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
    main()
