#!/usr/bin/env python3
"""fetch_extern.py -- the third-party sources vrcore builds against, at their pinned commits (THIRD_PARTY.md).
The one list of these pins: a game's setup calls this script instead of keeping its own copy.

  fetch_extern.py [--desktop] [--extern DIR]

  (default)   the full set, for the native host and the APK: SDL 3, Dear ImGui, OpenXR SDK, Vulkan-Headers,
              VMA, volk, cgltf, and Steam Audio's release zip (checked against its sha256; 181 MB)
  --desktop   SDL 3 and Dear ImGui only (what a game's desktop build also needs)
  --extern    where they go (default: ../../extern from vrcore, CMake's VRCORE_EXTERN_DIR), one folder each:
              sdl3, imgui, openxr, vulkan-headers, vma, volk, cgltf, steam-audio

Needs git, and curl for the zip (the system trust store checks the download). Python's standard library only.
A folder already at its pin is left as it is. Nothing fetched here is committed (extern/ is git-ignored).
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

VRCORE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (url, commit, folder)
SDL3 = ("https://github.com/libsdl-org/SDL.git", "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c", "sdl3")  # release-3.4.16
IMGUI = ("https://github.com/ocornut/imgui.git", "f1cc2ae15e53a861a874c3034aae6798fde194ab", "imgui")  # v1.92.9b
OPENXR = ("https://github.com/KhronosGroup/OpenXR-SDK.git", "f2448a8797c85814aa892efc1ab8707900fbcc78", "openxr")  # release-1.1.63
VULKAN_HEADERS = ("https://github.com/KhronosGroup/Vulkan-Headers.git", "6802bb4733b63ed5efd3adb308a6c885ef180ea1",
                  "vulkan-headers")  # vulkan-sdk-1.4.363.0
VMA = ("https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git", "3aa921224c154a0d2c43912bc88e1c42ce1f7607",
       "vma")  # v3.4.0
VOLK = ("https://github.com/zeux/volk.git", "54fc0d7a72887cbfc5b030d26897b2b40ac53fb8", "volk")  # vulkan-sdk-1.4.363.0
CGLTF = ("https://github.com/jkuhlmann/cgltf.git", "360db1a95480fe102ae9c69b27c5d101167ff5ba", "cgltf")  # v1.15
# Steam Audio ships its libraries only in the release zip (tag v4.8.1 = 0da18255cca520771f363ee01f100572b39a308e)
STEAM_AUDIO_ZIP = ("https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip",
                   "4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449", "steam-audio")

DESKTOP = (SDL3, IMGUI)
FULL = DESKTOP + (OPENXR, VULKAN_HEADERS, VMA, VOLK, CGLTF)


def say(msg):
    print(f"\n== {msg}", flush=True)


def run(cmd, check=True):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=check)


def need(tool, hint):
    path = shutil.which(tool)
    if not path:
        sys.exit(f"fetch_extern: {tool} not found. {hint}")
    return path


# The pinned commit alone, at depth 1 (a full clone of SDL's history is large, and a reset connection loses it all).
# A checkout already there (shallow or full) is fetched into and checked out.
def fetch(url, commit, dest):
    if not os.path.isdir(os.path.join(dest, ".git")):
        os.makedirs(dest, exist_ok=True)
        run(["git", "-C", dest, "init", "-q"])
        run(["git", "-C", dest, "remote", "add", "origin", url])
    for attempt in range(3):
        if run(["git", "-C", dest, "fetch", "--depth", "1", "origin", commit], check=False).returncode == 0:
            break
    run(["git", "-C", dest, "checkout", "-q", "--detach", commit])


def fetch_zip(url, sha256, dest):
    """A release zip, checked against its pinned sha256, unpacked into dest."""
    stamp = os.path.join(dest, ".sha256")
    if os.path.isfile(stamp):
        with open(stamp) as f:
            if f.read().strip() == sha256:
                return
    tmp = dest + ".zip"
    need("curl", "Install curl (the system trust store checks the download).")
    try:
        run(["curl", "-fsSL", "--retry", "3", "-o", tmp, url])
        digest = hashlib.sha256()
        with open(tmp, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                digest.update(block)
        if digest.hexdigest() != sha256:
            sys.exit(f"fetch_extern: {url} has sha256 {digest.hexdigest()}, pinned {sha256}")
        shutil.rmtree(dest, ignore_errors=True)
        with zipfile.ZipFile(tmp) as z:
            z.extractall(dest)
        with open(stamp, "w") as f:
            f.write(sha256 + "\n")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--desktop", action="store_true", help="SDL 3 and Dear ImGui only")
    ap.add_argument("--extern", default=os.path.join(VRCORE, "..", "..", "extern"), help="where they go")
    args = ap.parse_args()
    need("git", "Install Git.")
    extern = os.path.abspath(args.extern)
    os.makedirs(extern, exist_ok=True)

    say("Fetching vrcore's " + ("desktop " if args.desktop else "") + "libraries into " + extern)
    for url, commit, dest in DESKTOP if args.desktop else FULL:
        fetch(url, commit, os.path.join(extern, dest))
    if not args.desktop:
        say("Fetching Steam Audio 4.8.1 (release zip)")
        url, sha256, dest = STEAM_AUDIO_ZIP
        fetch_zip(url, sha256, os.path.join(extern, dest))


if __name__ == "__main__":
    main()
