# Notice

This repository holds device definitions and plugins for GlowDen, an independent,
community-made framework for configuring gaming peripherals on Linux. It is not affiliated
with, endorsed by or supported by Logitech, DTS or Xperi.

## Trademarks

Logitech, Logitech G, G HUB, LIGHTSYNC, LIGHTSPEED, Blue VO!CE and the product names of
Logitech devices are trademarks of Logitech International S.A. DTS and ClearVoice are
trademarks of DTS, Inc. (Xperi). They are used here only to say which devices and files
these definitions and plugins work with.

## What this repository contains and what it doesn't

- **Definitions** describe protocol facts worked out from the devices and from captures of
  their traffic, for interoperability: product ids, HID++ feature numbers, parameter
  layouts, LED positions. Pictures in this repository are original drawings.
- **Product photos**: some definitions point at a product photo Logitech publishes on its
  website. GlowDen downloads it on the user's machine on first use; the photo isn't
  stored in or distributed from this repository.
- **The Blue VO!CE plugin** is original code: a loader that runs the DTS ClearVoice 2
  engine from the user's own G HUB installation, read in place from a folder the user
  chooses. No DTS or Logitech binary is part of this repository or its releases. The
  engine stays under its owners' license; whether you may use it this way depends on the
  terms you accepted for G HUB, and what you point GlowDen at is your responsibility.

Nothing here is decrypted from protected files.

## License

The definitions and plugins are licensed under MIT or Apache-2.0 (`LICENSE-MIT`,
`LICENSE-APACHE`). Plugin binaries include Rust crates under their own licenses.
