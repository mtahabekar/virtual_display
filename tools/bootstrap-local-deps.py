#!/usr/bin/env python3
"""Extract Ubuntu 24.04 media development/tool packages locally; never use sudo."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1] / ".deps"
packages = root / "packages"
packages.mkdir(parents=True, exist_ok=True)
# Versions match the installed and tested Ubuntu Noble runtime libraries.
specs = ["libpipewire-0.3-dev=1.0.5-1ubuntu3.3", "libspa-0.2-dev=1.0.5-1ubuntu3.3",
         "ffmpeg=7:6.1.1-3ubuntu5", "libavdevice60=7:6.1.1-3ubuntu5",
         "libopenal1=1:1.23.1-4build1"]
print("Downloading and extracting into", root, flush=True)
subprocess.run(["apt-get", "download", *specs], cwd=packages, check=True)
for package in packages.glob("*.deb"):
    subprocess.run(["dpkg-deb", "-x", str(package), str(root / "root")], check=True)
print("No system packages were installed. Reconfigure the project with CMake.")
