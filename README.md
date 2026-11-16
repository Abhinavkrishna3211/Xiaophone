# Xiaophone

A tiny ivory + gold gramophone whose "record" is a 1.28" round display showing the
album cover of the song playing — cropped into a circle and spinning like vinyl.
One Seeed XIAO ESP32S3 Plus streams Wi-Fi internet radio, looks up the cover, and spins it.

## Wiring (XIAO ESP32S3 Plus)

| Part | Pin | XIAO pin (GPIO) |
|---|---|---|
| GC9A01 display | VCC | 3V3 (never 5V) |
| | GND | GND |
| | SCL (SCK) | D8 (GPIO7) |
| | SDA (MOSI) | D10 (GPIO9) |
| | CS | D1 (GPIO2) |
| | DC | D2 (GPIO3) |
| | RES | D0 (GPIO1) |
| | BLK | 3V3 |
| MAX98357A | VIN | 5V |
| | GND | GND |
| | BCLK | D4 (GPIO5) |
| | LRC | D3 (GPIO4) |
| | DIN | D5 (GPIO6) |
| | GAIN, SD | unconnected |
| Speaker | + / − | MAX98357 outputs |

Add a 220–470 µF cap across the MAX98357 VIN/GND.

## Build

Firmware (VS Code + PlatformIO):
1. Copy `firmware/src/secrets.example.h` to `firmware/src/secrets.h` and fill in Wi-Fi + stations.
2. Open `firmware/` in PlatformIO, then **Upload** and **Monitor** (115200 baud).

Enclosure: Blender scripts in `enclosure/scripts/` (run with `blender -b -P <script>.py`)
produce print-ready STLs in `enclosure/stl/`. Cabinet + lid in matte ivory PLA,
horn and small parts in silk gold PLA.

## License

MIT
