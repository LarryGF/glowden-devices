<p align="center"><img src="assets/logo.png" width="300" alt="GlowDen"></p>

# GlowDen devices

Device definitions and plugins for [GlowDen](https://github.com/larryGF/glowden), the open
device hub for Logitech G peripherals on Linux. GlowDen reads this repository's releases as
its built-in device source; anyone can publish a source of their own in the same format.

Not affiliated with Logitech. Definitions describe protocol facts (product ids, feature
numbers, parameter layouts); no Logitech or DTS files are part of this repository. See [`NOTICE.md`](NOTICE.md).

## Using it

Nothing to install: GlowDen lists this source as built in (Settings → Device sources) and
installs a device's definition automatically when the device connects; you can also install
or remove definitions one by one there. Plugins run only after you choose *Allow plugins*
for the source.

| Device | Definition | Plugin |
|---|---|---|
| G515 LS TKL keyboard | ISO TKL layout, per-key zones, reactive input, firmware ripple | – |
| G502 X PLUS mouse | Drawn picture, product-photo LED regions, host DPI buttons, button names | – |
| G522 LIGHTSPEED headset | Identity | – |
| Yeti GX microphone | Blue VO!CE source and on-enable settings | `blue-voice` |

To use another source (or a local checkout while developing), add the URL of its
`index.json`, or a folder holding one, in the same place.

## Layout

| Path | What |
|---|---|
| `devices/<id>/device.toml` | A device definition, plus any files it refers to (e.g. a drawn picture) |
| `plugins/<id>/` | A plugin: a program GlowDen runs for devices that need code |
| `tools/make-index.py` | Builds a publishable source: `index.json` plus every file, flat |
| `ADDING-DEVICES.md` | How to write a definition |

## Definitions

A definition adds what a device doesn't report over HID++ itself: its kind, picture model,
key layout, LED regions, host-handled buttons, firmware effects and Blue VO!CE settings. See
`ADDING-DEVICES.md`.

## Plugins

A plugin is any executable that speaks JSON lines on stdin/stdout; GlowDen starts it with
`serve`. Plugins run only after the user allows the source's plugins in GlowDen's settings.

`plugins/blue-voice` runs Blue VO!CE and G HUB's voice effects from the user's own copy of
the DTS ClearVoice 2 engine (found in their G HUB folders), loaded by a small PE loader and
hosted on PipeWire as a virtual microphone. Protocol: see the top of its `src/main.rs`.

## Publishing a source

```sh
cargo build --release
tools/make-index.py dist --version 0.1.0 --plugin blue-voice=target/release/glowden-blue-voice
```

Upload everything in `dist/` to one place (a GitHub release does it in CI) and add the URL of
its `index.json` in GlowDen → Settings → Device sources. A local folder works too.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

MIT or Apache-2.0, at your option.
