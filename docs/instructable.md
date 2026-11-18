# Xiaophone: a Tiny Gramophone Whose Record Is the Album Cover

---

## Intro: Meet Xiaophone

> IMAGES (upload at the top of this step, in order) → media\00_Intro\: 01_cover_main.png, 02_front.png, 03_closeup.png, 04_back.png, 05_moods.png

*Xiǎo* (小) means "little" in Chinese, and this little gramophone runs on a Seeed **XIAO**, so meet **Xiaophone**, the littlest gramophone on the shelf.

She's about 7 cm wide, ivory and gold like an old music box, with a big petal horn. Her "record" is actually a **1.28" round display**. Whatever song is playing, Xiaophone finds its **album cover**, crops it into a circle, adds vinyl grooves and **spins it like a real record**.

How it works, in one line: **the radio tells Xiaophone the song name → she looks up the album cover → it becomes her record.**

She has moods, too:
- **Sleepy:** at power-on the label has closed eyes and blushing cheeks. Then she blinks awake, plays a 3-note music-box chime, and the record spins up.
- **New song:** the old record spins down, a soft vinyl crackle plays, the new cover fades in with a ring of gold sparkles, and the song title curls around the rim for a few seconds.
- **No cover found:** a cream label with a gently pulsing gold heart spins instead.
- **Wi-Fi lost:** the record slowly winds down until the connection comes back.

Everything runs on **one board**: Wi-Fi internet radio, cover lookup, audio and the spinning display. You plug in a USB-C cable and she plays.

All code, Blender scripts and STLs: **https://github.com/Abhinavkrishna3211/Xiaophone**

---

## Supplies

> IMAGES (upload at the top of this step, in order) → media\01_Supplies\: 01_components.png, 02_supplies_table.png

**Electronics**
- 1 × Seeed Studio **XIAO ESP32S3 Plus** (16 MB flash, 8 MB PSRAM) + the U.FL Wi-Fi antenna from its box
- 1 × **1.28" round IPS display**, 240×240, **GC9A01** driver, SPI (the blue octagonal board, 36 × 44 mm)
- 1 × **MAX98357A** I2S amplifier breakout
- 1 × **2" (Ø56.6 mm) 4–8 Ω speaker**, about 2–3 W (mine is 18.4 mm deep)
- 1 × 220–470 µF electrolytic capacitor
- 26–28 AWG silicone wire, heat-shrink
- USB-C cable + 5 V / 2 A charger or power bank

**Enclosure**
- 3D printer (I used a Bambu Lab P2S with AMS)
- **Matte ivory PLA**, **silk gold PLA**, a little **matte black PLA**
- 3 × **M2 × 8 mm** self-tapping screws
- A few cm of **1.75 mm filament** (used as pins for the small gold parts)
- Super glue / CA, double-sided foam tape, hot glue
- Optional: gold paint pen for touch-ups

**Tools**
- Soldering iron, flush cutters, small screwdriver
- A computer with **VS Code + PlatformIO**
- (Only if you want to remix the shape) Blender 4.x

---

## Step 1: How It Works

> IMAGES (upload at the top of this step, in order) → media\02_Step1_How_It_Works\: 01_how_it_works.png, 02_cover_lookup.png

The XIAO ESP32S3 does three jobs at once, split across its two cores:

1. **Radio (core 0):** it streams an internet radio station over Wi-Fi and sends the audio over I2S to the MAX98357A, which drives the speaker. Radio streams carry the current song as text ("ICY metadata"), e.g. `Daft Punk - Get Lucky`.
2. **Cover lookup (low-priority task):** when the title changes, it cleans up the text (removes "(Remastered)", "[Live]", "feat. …"), searches **MusicBrainz** for that song, picks the original album over compilations and live versions, then downloads the front cover thumbnail from the **Cover Art Archive**.
3. **The record (core 1):** the JPEG is decoded into PSRAM, cut into a 240 px circle, given grooves, a glossy highlight and a gold spindle, and then rotated at a 33⅓ rpm feel at about 25 fps.

The audio task always gets priority, so a cover download never makes the music stutter. The last 20 covers are cached, so a song that comes around again shows up instantly.

---

## Step 2: Wiring

> IMAGES (upload at the top of this step, in order) → media\03_Step2_Wiring\: 01_wiring_diagram.png, 02_wiring_table.png

Tips:
- Solder the **220–470 µF capacitor across the MAX98357A VIN and GND** pins (watch the polarity). It stops pops and brown-outs when the bass hits.
- The display header order (left to right) is **GND, VCC, SCL, SDA, RES, DC, CS, BLK**. Double-check VCC before powering up. There's no spare if it gets 5 V.
- Cut wires about **8–10 cm** long so the bottom plate can be opened without pulling anything.
- **Test everything on a breadboard first** (Step 4) before soldering it into the case.

---

## Step 3: Install the Firmware

> IMAGES (upload at the top of this step, in order) → media\04_Step3_Firmware\: 01_firmware_steps.png, 02_secrets_h.png

1. Install **VS Code** and the **PlatformIO** extension.
2. Download or clone the repo: `git clone https://github.com/Abhinavkrishna3211/Xiaophone`
3. Open the `firmware/` folder in PlatformIO.
4. Copy `src/secrets.example.h` to `src/secrets.h` and fill in your Wi-Fi name, password and up to three station URLs. The defaults already work well because they send good song titles:
   - Radio Paradise: `http://stream.radioparadise.com/mp3-128`
   - SomaFM Indie Pop Rocks: `http://ice1.somafm.com/indiepop-128-mp3`
   - SomaFM Underground 80s: `http://ice1.somafm.com/u80s-128-mp3`
5. Upload the sound files (the chime and the vinyl crackle) to flash: **PlatformIO → Upload Filesystem Image** (`pio run -t uploadfs`).
6. Screw the Wi-Fi antenna onto the XIAO, then upload the firmware and open the serial monitor at 115200 baud: `pio run -t upload -t monitor`.

Settings you can tweak in `src/config.h`:
- `VOLUME` (0–21, default 12)
- `SPI_HZ`: display clock, 40 MHz by default. Try 80 MHz if your wiring is short and the picture stays clean.
- `DEMO_MODE 1`: no radio needed. It shows five well-known covers in turn, one every 20 s. Great for filming.

The libraries do the heavy lifting: **ESP32-audioI2S** (streaming + metadata), **LovyanGFX** (fast display with DMA), **JPEGDEC** (cover decoding) and **ArduinoJson**. All sounds and graphics are original and generated in code.

---

## Step 4: Test on the Breadboard

> IMAGES (upload at the top of this step, in order) → media\05_Step4_Breadboard_Test\: none yet (videos cover this step)

> VIDEO 1, display test (embed at the top of this step) → https://www.youtube.com/watch?v=9CsGYHmGGgo

> VIDEO 2, speaker test (embed after video 1) → https://www.youtube.com/watch?v=QAdcZ29-DCE

Power it from USB and watch the serial monitor. You should see, in order:
1. `[WIFI]` connects, and the display shows the **sleepy label**.
2. The chime plays, the record spins up, and music starts (`[AUDIO]`).
3. `[META]` prints the song name, then `[COVER]` reports the lookup, and the cover fades in.

Every 5 s it also prints frames per second, heap and PSRAM. Expect around 25 fps.

**If something's off:**
- Black screen: check VCC is on 3V3, then check CS/DC/RES. Drop `SPI_HZ` to 20 MHz to rule out long wires.
- Hum or crackling audio: add or check the big capacitor, and keep the amp wires away from the display wires.
- The heart label stays and no cover appears: some stations don't send song titles, and MusicBrainz doesn't know every song. Try one of the default stations.

Let it play for 10 minutes. You're looking for no dropouts, a cover on every song change, and no reboots.

---

## Step 5: Designing the Gramophone Body

> IMAGES (upload at the top of this step, in order) → media\06_Step5_Designing_The_Body\: 01_base_model_parts.png, 02_scaled_to_hardware.png, 03_hollow_shell.png, 04_internals.png, 05_assembled.png

I wanted a chubby, music-box style gramophone: square cabinet, raised side panels, gold corner caps, bun feet and a big scalloped horn.

1. **Base model:** I drew a reference image of the look I wanted and turned it into a rough 3D model with an image-to-3D generator (Tripo3D). That gives you the cute shape, but it's one lumpy mesh at a random size with no room inside.
2. **Rebuilt around the hardware:** a set of re-runnable **Blender Python scripts** (`enclosure/scripts/00_import.py` … `06_export.py`, run with `blender -b -P <script>.py`) does the rest:
   - cleans and remeshes the model so it's watertight,
   - **scales it from the record, not the cabinet:** the platter becomes ~42 mm across to frame the 33 mm screen, and the cabinet ends up 68 mm wide, just enough for the 2" speaker in the floor,
   - hollows the cabinet with 2 mm walls,
   - cuts off a removable **bottom plate** (with the feet) that holds the speaker,
   - cuts the stepped **display pocket**, the **USB-C slot**, the speaker pod and sound holes,
   - repairs the spots where the AI mesh came out torn: the broken front-right corner is rebuilt as a mirror of the clean back-right one, and the horn's collar is re-turned with a clean profile,
   - swaps the generator's lumpy gold pieces (corner caps, side trims, grille ring) for **clean turned parts** of the same size and position, so the gold edges look crisp,
   - splits the gold decorations into separate parts with pin holes,
   - exports print-ready STLs plus a colour-tagged 3MF.

The rule I followed: **keep the base model's cute proportions and detail exactly**. Only the inside and the broken bits change.

Every dimension lives in one `DIM` table in `_common.py`. If your display or speaker is a different size, change the number and re-run the scripts. `check_parts.py` then checks that every STL is watertight.

**Inside the box:** the speaker sits in a rounded-square **plinth** under the bottom plate, firing down through a flower pattern of sound holes. The bun feet lift everything so the sound escapes underneath, and from the side nothing round shows. The display is held in a stepped pocket under the lid. The XIAO stands upright in the back-right corner with its USB-C port at a slot low on the back. The amp sits in a corner beside the speaker magnet. The Wi-Fi antenna sticks to an inside side wall near the top, away from the magnet.

---

## Step 6: Print the Display Test Piece First

> IMAGES (upload at the top of this step, in order) → media\07_Step6_Display_Test_Piece\: 01_fit_test.png

Before the big print, print **`stl/test_display_pocket.stl`**. It's just the top 6 mm of the cabinet (about 20 minutes). Push the display in from underneath. The glass should sit flush in the round recess, and the blue board should drop onto the four little pins.

If it's tight, sand the pocket lightly. If the pins don't line up with your board's holes, just snip them off; the pocket alone holds the board.

---

## Step 7: Print All the Parts

> IMAGES (upload at the top of this step, in order) → media\08_Step7_Print_Parts\: 01_printed_parts.png, 02_print_layout.png, 03_print_settings.png

All STLs are in `enclosure/stl/`, already oriented for printing. If you have an AMS, open **`xiaophone_all.3mf`** (or `plate_ivory.3mf` / `plate_gold.3mf`), where every part is named and coloured.

I used a 0.4 mm nozzle with 0.16 mm layers on the visible parts, and ironing on for the shell's top surface. Printing the ivory and gold parts on **separate plates** keeps the colours clean with no purge waste, and you never have to paint the gold.

---

## Step 8: Gold Decorations

> IMAGES (upload at the top of this step, in order) → media\09_Step8_Gold_Decorations\: 01_parts_laid_out.png, 02_gold_parts_fitted.png

There are 4 side trims, 3 corner caps, the grille ring, heart, crank and pivot knob. The small gold parts each have **Ø1.9 mm holes** that line up with holes in the shell.

1. Cut short snips of **1.75 mm filament** and push them into the shell holes as pins.
2. Add a drop of CA glue and press each gold part onto its pins.
3. Glue the **black vinyl ring** and the **gold rim** around the display window. The screen shows through the middle like the label of a record.
4. Glue the **pivot knob** at the back corner and the **tonearm** onto it, with the headshell just resting on the rim.
5. Plug the **horn** into its socket at the back.

Touch up any seams with a gold paint pen if you like.

---

## Step 9: Mount the Display

> IMAGES (upload at the top of this step, in order) → media\10_Step9_Mount_Display\: none yet, add your own photo (see PHOTOS_TO_ADD.txt)

1. Turn the shell upside down.
2. Drop the display in **glass first**, with the **header facing the back** of the cabinet.
3. Seat the board on its pins and add a few dots of hot glue on the board edges (not on the glass) to hold it.
4. Solder (or plug) the 8 display wires now. It's much easier before the rest goes in.

---

## Step 10: Speaker, Amp and XIAO

> IMAGES (upload at the top of this step, in order) → media\11_Step10_Speaker_Amp_XIAO\: 01_inside_layout.png, 02_usb_c_slot.png

1. **Speaker:** press the speaker into the plinth in the bottom plate, cone facing down. The four little clips snap over its frame. A thin foam or felt ring under the frame stops rattles.
2. **Amp:** solder the speaker wires to the MAX98357A, then stick the amp in a corner beside the speaker magnet with foam tape.
3. **XIAO:** slide it into the back-right corner so its **USB-C port lines up with the slot** in the back wall. Check that a cable plugs in fully and the board doesn't move. Add a dot of hot glue if needed.
4. **Antenna:** peel the Wi-Fi antenna and stick it flat on an inside **side** wall near the top, as far from the speaker magnet as possible. Click the U.FL plug onto the XIAO.
5. Tuck the wires so nothing presses on the back of the display.

Power it up once **before closing**, to check that the screen, sound and Wi-Fi all still work.

---

## Step 11: Close It Up

> IMAGES (upload at the top of this step, in order) → media\12_Step11_Close_It_Up\: 01_assembled.png, 02_exploded_studio.png, 03_exploded.png

Fit the bottom plate into the lip of the shell and fix it with **3 × M2 × 8 mm screws** up through the feet. The screw heads hide inside the feet. The fourth corner has no screw because the XIAO lives there.

Plug in USB-C. Xiaophone wakes up sleepy, blinks, chimes and starts spinning. 🎶

---

## Step 12: Bonus: Spotify Connect

> IMAGES (upload at the top of this step, in order) → media\13_Step12_Spotify_Bonus\: none yet (the video covers this step)

> VIDEO, playing from Spotify (embed at the top of this step) → https://www.youtube.com/shorts/4yCHCLWo2VQ

If you have **Spotify Premium**, the `spotify` branch of the repo (experimental) turns Xiaophone into a Spotify Connect speaker. Put your phone on the same Wi-Fi, tap the devices icon in Spotify and choose **Xiaophone**. The radio pauses, the song plays from her horn, and the record shows the cover Spotify sends. Volume follows the phone, and when you stop for 5 minutes the radio comes back on its own.

It uses **cspot**, an unofficial open-source Spotify Connect client (not made or endorsed by Spotify). That branch builds with ESP-IDF + Arduino instead of plain Arduino. It needs a few extra Python packages for the Spotify protobuf step, and a firmware built from it is GPL-3.0 because cspot is. The README on that branch has the exact setup steps.

---

## Step 13: Enjoy!

> IMAGES (upload at the top of this step, in order) → media\14_Step13_Enjoy\: 01_closeup.png, 02_front.png

The best part is the little surprise every few minutes: a new song starts, the record winds down, crackles, and a new cover twinkles into place.

Ideas if you build your own:
- Swap in your favourite stations in `secrets.h`. Any stream that sends "Artist - Title" works, in any language. My tests found the right covers for English, Hindi, Malayalam, Tamil and Telugu songs.
- Change the shape: every dimension is in `_common.py`.
- Print it in other colours: mint and rose gold would be adorable.

Thanks for reading! If you build a Xiaophone, I'd love to see what's spinning on yours. 💛

**Credits:** song data from MusicBrainz (CC0), cover images from the Cover Art Archive. Libraries: ESP32-audioI2S, LovyanGFX, JPEGDEC, ArduinoJson, and cspot for the Spotify bonus.
