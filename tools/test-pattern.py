#!/usr/bin/env python3
"""Launch temporary 2D test windows on the selected virtual monitors."""
import os
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "host/diagnostics"))
from doctor import kscreen_environment
binary = root / "build/quest-test-pattern"
os.execve(str(binary), [str(binary), *sys.argv[1:]], kscreen_environment())
