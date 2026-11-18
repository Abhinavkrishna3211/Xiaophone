# Hardware dimensions (measured / from datasheets)

## Round display — GC9A01 1.28" 240×240 (display_module_dimensions.png, display_datasheet.png)
- Blue PCB: octagonal, **36 mm × 35 mm**, **44 mm** corner-to-corner diagonal.
- 4 mounting holes near the corners (≈ Ø2.5 mm).
- 8-pin header along one 36 mm edge, **24 mm** wide, 2.54 mm pitch.
  Order: GND, VCC, SCL, SDA, RES, DC, CS, BLK.
- Glass / LCD outline: **Ø35.2 mm** (backlight frame 35.6 mm wide × 38.1 mm tall incl. the FPC side).
- Polarizer: Ø34.74 mm. **Active (visible) area: Ø32.4 mm.**
- Panel thickness: **1.5 mm** (glass sits on the PCB). FPC flex folds behind on the edge opposite the header (~3 mm extra).
- Lid cut: Ø33.0 mm window → Ø36.0 mm × 1.7 mm glass recess → 36.6 × 35.6 mm × 2.0 mm PCB recess
  → clearance behind for header pins + wires (≥ 10 mm).

## Speaker — Conel 2" round, code 1101 (speaker_*.jpg)
- Frame outer diameter: **56.6 mm** (measured).
- Total depth (frame lip to back of magnet): **18.4 mm** (measured).
- Magnet: **Ø36 × 8 mm** (box spec).
- Solder tabs on the frame rim (leave ~5 mm clearance beside the rim for wires).
- Mount: bottom plate, firing down, Ø57.0 mm pocket with 2 mm lip.

## MAX98357A amp board (max98357_back.png)
- ≈ 18 × 18 mm, 7-pin header + 2-pad speaker terminal on the opposite edge.

## XIAO ESP32S3 Plus
- ≈ 21 × 17.5 mm, USB-C on one short edge.

## Design reference
- design_reference.png = the reference image for the base model (ivory + gold, no plinth).
