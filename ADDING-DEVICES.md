# Adding a device

Most Logitech G devices speak HID++ 2.0 and report their features. glowden drives the standard
ones for any device that has them, without a definition:

| Feature | What works automatically |
|---|---|
| `0x8071` RGB effects | Fixed, breathing, color cycle (effects looked up by id) |
| `0x8081` per-key lighting | Live effects (zones laid out in a line without a layout) |
| `0x8040` brightness | Brightness, scaled to the device's maximum |
| `0x2201` adjustable DPI, `0x8060` report rate | Sensitivity panel |
| `0x1004` unified battery | Battery level |
| `0x8370` microphone | Gain, mute |

A definition file adds what can't be discovered: the device's kind, its picture and LED
positions, key geometry, buttons the host has to handle, and firmware-only effects.

## Step by step

1. **Identify the device.** `target/release/glowdenctl list` prints each device with its USB
   product id and HID++ features. Note the product id (`046d:XXXX`) and which of the
   features above it has.
2. **Write a minimal definition** in `drivers/<device>.toml` (or
   `~/.local/share/glowden/drivers/` for a personal one):
   ```toml
   name = "G502 X PLUS"
   kind = "mouse"            # keyboard | mouse | headset | microphone
   products = [0x4099]       # receiver-attached devices: the device's own id, not the receiver's
   ghub_model = "g502x_plus" # folder for its pictures in ~/.local/share/glowden/devices/
   ```
3. **Restart the service** and check its log. When the app started the service, kill it
   (`for p in $(pgrep -x glowdend); do kill $p; done`) and the app starts it again; otherwise
   `setsid -f target/release/glowdend >> ~/.local/state/glowden/glowdend.log 2>&1`.
   The log lists `driver: <name> from <file>` for every definition it loaded, or why it
   skipped one.
4. **Add a picture** (next section), then the optional parts: [buttons the host
   handles](#buttons-the-host-handles), [firmware effects](#firmware-effects), and
   [key geometry](#key-geometry-keyboards).
5. **Check it in the app**: the device card and page show the picture, and live colors
   appear on its LEDs on the Lighting page.

## Pictures

The viewer lights the LEDs in the device picture. Three sources, in the order glowden tries
them:

### 1. G HUB's own pictures (best)

G HUB keeps per-device packages with pictures and LED geometry. Copy
`C:\ProgramData\LGHUB\depots` from a Windows install that has seen the device, then:

```sh
glowdenctl import-ghub /path/to/depots
```

Packages land in `~/.local/share/glowden/devices/<model>/`. glowden picks the picture whose pixel
size matches the LED geometry in `metadata.json`. These pictures have transparent cutouts
where light shines through (key legends, logos, diffusers), and the live colors are drawn
behind them, as G HUB does. Nothing needs to go in the definition beyond `ghub_model`.

Some packages ship encrypted pictures (files that start with `21 05 21 20`). The importer
skips them; use a product photo instead.

### 2. A product photo

The manufacturer's public product photo, downloaded once by the service into
`~/.local/share/glowden/devices/<model>/product-photo.png`. It is never shipped with glowden.

1. **Find a top view.** On the product page at logitechg.com, the gallery images are
   under `https://resource.logitechg.com/<format>/content/dam/gaming/…/gallery/…-top-angle-…png`.
   Use the format `f_png`. `f_png,q_auto` sends a 256-color version with jagged edges.
2. **Measure the crop.** The photos have a transparent background. Run
   `tools/photo-regions.py photo.png --point 1:0,0` without `--crop`; it prints the alpha
   bounding box, which is the `crop`.
3. **Place the LEDs.** Pick one point per zone on the lit area of the photo, in
   cropped-photo pixels. To find which zone is where, light one zone at a time
   (`glowdenctl <device> key <zone> ff0000`) and look at the device. Then:
   ```sh
   tools/photo-regions.py photo.png --crop X Y W H --top 850 \
       --point 1:265,1280 --point 2:660,1390 ... --preview preview.png
   ```
   `--top` keeps other lit parts of the photo (a DPI indicator, a logo) out of the regions.
   Check `preview.png`: every zone should light its own part of the lit area. Paste the
   printed `[[layout.photo.led]]` blocks into the definition.
4. **Add the photo to the definition:**
   ```toml
   [layout.photo]
   url = "https://resource.logitechg.com/f_png/content/dam/…/top-angle-…png"
   crop = [716, 198, 1046, 1761]
   cutout = "vivid"

   [[layout.photo.led]]
   zone = 1
   path = "M0 850 L517 850 …"
   ```

With `cutout = "vivid"`, strongly colored pixels inside the LED regions (the LEDs lit in
the photo) turn grey and see-through, and the live colors show there. The regions only
need to cover their part of the lit area. A changed `url` is fetched again.

### 3. A drawn fallback

Used when there is no imported picture and the photo can't be downloaded (offline). Draw
an SVG at the size of the G HUB geometry, so the LED shapes from `metadata.json` line up:

```toml
[layout]
picture = "pictures/<device>.svg"   # relative to the definition file
picture_clip = "M…Z"                # optional: light only inside this outline
```

Without any picture, the page shows a generic device icon with one colored dot per zone.

## Buttons the host handles

When glowden takes over a mouse's lighting, the mouse switches to host mode and leaves some
buttons to the host. On the G502 X PLUS these are DPI up, DPI down and DPI shift. glowden
watches them through the button spy feature (`0x8110`) and steps through the DPI levels
from the Sensitivity panel; the level chosen with the buttons is saved.

```toml
[input]
dpi_up = 0x0200      # button-spy masks of each button
dpi_down = 0x0400
dpi_shift = 0x0010   # held: lowest level; released: back to the current one
```

To find the masks, capture G HUB while pressing each button (see below). The mouse reports
`11 <index> <spy index> 00 <mask hi> <mask lo> …` on press and `… 00 00` on release;
button n is bit n−1.

The same feature lets the Assignments page give any button another job (a key or shortcut,
a macro, a mouse button, scrolling, DPI, a program). An assigned button is told to send
nothing to the PC, and glowden performs its action through a virtual input device
(`/dev/uinput`, see `packaging/udev`). Name the buttons and set what unassigned buttons send:

```toml
[input]
routing = [1, 2, 3, 4, 0, 5, 0, 0, 0, 0, 0]   # button n → PC button (0 = host handles it)

[[button]]
number = 4
label = "Back"
```

Button numbers can be found with "Detect button" on the Assignments page.

## Firmware effects

Effects stored in the device. Find candidates with `glowdenctl <device> effects` (cluster `0xff`
lists the all-zones effects) and try them with
`glowdenctl <device> raw-effect <cluster> <index> <hex params>`.

```toml
[[firmware_effect]]
id = "ripple"
label = "Ripple"
cluster = 0xff
effect_id = 0x0017
params = "{r}{g}{b}ff01000014"   # up to 10 bytes; {r}{g}{b} = chosen color
```

## Key geometry (keyboards)

```toml
[layout]
keys = "iso-tkl"     # built-in key geometry for live effects
zones = "hid-usage"  # zone id = USB key code − 3 (otherwise zone ids are used as-is)

[input]
reactive = true      # read key presses from the device for reactive effects
```

## Where definitions are read from

In this order; a later file wins for the same product id:

1. `/usr/share/glowden/drivers/` (shipped definitions)
2. `drivers/` in the repo (development builds)
3. `~/.local/share/glowden/drivers/` (your own)
4. directories in `$GLOWDEN_DRIVERS`

## Field reference

| Field | Meaning |
|---|---|
| `name`, `kind`, `products`, `ghub_model` | Identity; see step 2 |
| `layout.keys`, `layout.zones` | Key geometry and zone numbering (keyboards) |
| `layout.lighting_view` | `"first"`: show only the first view of multi-view G HUB pictures |
| `layout.picture`, `layout.picture_clip` | Drawn fallback picture |
| `layout.photo.url`, `.crop`, `.cutout` | Product photo |
| `[[layout.photo.led]]` `zone`, `path` | LED regions on the photo |
| `input.reactive` | Key presses drive reactive effects |
| `input.dpi_up`, `.dpi_down`, `.dpi_shift` | Button-spy masks of host-handled DPI buttons |
| `input.routing` | What each button sends to the PC when unassigned |
| `[[button]]` `number`, `label` | Button names on the Assignments page |
| `[voice]` `source`, `description` | Blue VO!CE: PipeWire source to process, virtual mic name |
| `[[firmware_effect]]` | Firmware effects |

## Probing and captures

- `glowdenctl <device> call <feature> <function> [hex params]` sends one HID++ request and prints
  the reply. Use reads (usually function 0 or 1) first, and follow the device safety rules
  in `CLAUDE.md` before writing anything.
- `GLOWDEN_DEBUG=1` prints every frame the CLI or service sends and receives.
- To see what G HUB does, run it in a Windows VM with the device passed through and record
  the USB traffic with the scripts in `../vm/` (next to this repo, not part of it):
  `start.sh <device>`, `capture.sh <device> <label>` while changing
  one setting, then `decode.sh captures/<device>/<label>.pcapng`. Keep one action per
  capture and name it after the action.

## When data isn't enough

New protocol behaviour (a feature glowden doesn't implement yet) goes in
`crates/hidpp/src/devices.rs` as a function per feature, keyed by HID++ feature id, so every
device with that feature gets it. Record where the bytes came from (USB capture, device
reply) in a comment.
