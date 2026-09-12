#!/usr/bin/env python3
"""Run the real KScreen Wayland backend with the verified Qt/NVIDIA workaround."""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host/diagnostics"))
from doctor import kscreen_environment

os.execvpe("kscreen-doctor", ["kscreen-doctor", *sys.argv[1:]], kscreen_environment())
