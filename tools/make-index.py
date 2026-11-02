#!/usr/bin/env python3
"""Builds a GlowDen device source in OUT: index.json plus every file it lists, flat.

  tools/make-index.py OUT [--version V] [--plugin ID=BINARY]...

A source is published by uploading OUT's files to one place (a GitHub release); GlowDen
reads index.json from there and fetches the files next to it. Plugins are listed with the
binaries given here (built by CI), with their SHA-256 so GlowDen can check downloads.
"""
import argparse
import hashlib
import json
import pathlib
import platform
import shutil
try:
    import tomllib
except ImportError:  # Python < 3.11
    import tomli as tomllib

ROOT = pathlib.Path(__file__).resolve().parent.parent

PLUGINS = {
    "blue-voice": {
        "name": "Blue VO!CE",
        "description": "Runs Blue VO!CE and the voice effects with your own copy of the engine from G HUB.",
    },
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--version", default="dev")
    ap.add_argument("--plugin", action="append", default=[], help="ID=path/to/binary")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    devices = []
    for toml_path in sorted(ROOT.glob("devices/*/device.toml")):
        folder = toml_path.parent
        dev = tomllib.loads(toml_path.read_text())
        files = {}
        for f in sorted(p for p in folder.rglob("*") if p.is_file()):
            rel = f.relative_to(folder).as_posix()
            asset = f"{folder.name}__{rel.replace('/', '__')}"
            shutil.copyfile(f, out / asset)
            files[rel] = asset
        plugins = [dev["voice"]["plugin"]] if dev.get("voice", {}).get("plugin") else []
        devices.append({
            "id": folder.name,
            "name": dev["name"],
            "kind": dev.get("kind"),
            "products": dev["products"],
            "files": files,
            "plugins": plugins,
        })

    plugins = []
    arch = f"{platform.machine()}-linux"
    for spec in args.plugin:
        pid, binary = spec.split("=", 1)
        data = pathlib.Path(binary).read_bytes()
        asset = f"{pid}-{arch}"
        (out / asset).write_bytes(data)
        (out / asset).chmod(0o755)
        plugins.append({
            "id": pid,
            **PLUGINS.get(pid, {"name": pid, "description": ""}),
            "binaries": {arch: {"file": asset, "sha256": hashlib.sha256(data).hexdigest()}},
        })

    index = {"format": 1, "name": "GlowDen devices", "version": args.version,
             "devices": devices, "plugins": plugins}
    (out / "index.json").write_text(json.dumps(index, indent=2) + "\n")
    print(f"{len(devices)} devices, {len(plugins)} plugins -> {out}")


if __name__ == "__main__":
    main()
