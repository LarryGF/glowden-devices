# Contributing to GlowDen devices

This repository holds what GlowDen knows about individual devices: definitions (data) and
plugins (code a device needs beyond what GlowDen does for every device). Changes to the
engine itself go to [GlowDen](https://github.com/larryGF/glowden).

## Ground rules

- **No Logitech or DTS material.** No pictures, product photos, sounds, presets, firmware,
  DLLs or text copied from G HUB or Logitech's sites. A definition may point at a public
  product photo by URL (GlowDen downloads it on the user's machine) and may include a
  picture you drew yourself.
- **Facts in your own words.** Product ids, HID++ feature numbers, zone ids and parameter
  layouts come from the device itself or from observing USB traffic. Note where bytes came
  from ("USB capture of G HUB doing X", "device reply") in a comment.
- **No decryption** of protected files.
- **Safe writes.** Anything a definition sends to a device (e.g. `on_enable` requests,
  firmware effect parameters) must have been seen in a capture of G HUB or confirmed by
  reading the device first. Never firmware/DFU commands.

## Adding or improving a device

Follow [`ADDING-DEVICES.md`](ADDING-DEVICES.md). In short:

1. Connect the device and run `glowdenctl list` to get its product id and HID++ features.
2. Create `devices/<id>/device.toml` (`<id>` in lowercase-with-dashes, e.g. `g305`).
3. Test it locally: build a source and point GlowDen at it.

   ```sh
   cargo build --release            # only if you touched a plugin
   tools/make-index.py dist --plugin blue-voice=target/release/glowden-blue-voice
   ```

   In GlowDen → Settings → Device sources, add the `dist` folder, install your device from
   it and check the device page (picture, LEDs, buttons, effects). Run `make-index.py` again
   after each change; *Check for updates* re-reads it.
4. Open a pull request with what you tested and on which device and firmware.

## Plugins

A plugin is a program in `plugins/<id>/`, in any language, that GlowDen starts with `serve`
and talks to with JSON lines on stdin/stdout (`{"id", "method", "params"}` requests,
`{"id", "result"|"error"}` replies, and `{"event", …}` messages). `plugins/blue-voice` is
the example; its `src/main.rs` documents the methods GlowDen's VO!CE support uses.

- Keep plugins self-contained: they run as the user, so request only what they need and
  never download or execute anything else.
- Build them for the oldest supported base (CI uses Ubuntu 22.04) so they run on newer
  systems.
- A new plugin needs an entry in `PLUGINS` in `tools/make-index.py`, its build in
  `.github/workflows/release-please.yml`, and a `--plugin` argument there.

## Commits and releases

Commits follow [Conventional Commits](https://www.conventionalcommits.org): `feat: add
G305`, `fix: G515 zone order`, `docs: …`. release-please opens release PRs from them;
merging one publishes `index.json`, the definitions and the plugin binaries to a GitHub
release, which GlowDen picks up as the built-in source.

## License

By contributing you agree that your contributions are licensed under MIT OR Apache-2.0, like
the rest of the repository.
