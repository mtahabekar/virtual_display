#!/usr/bin/env python3
"""Install the already verified host and user units; enabling is a separate command."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
libexec = Path.home() / ".local/libexec"
units = Path.home() / ".config/systemd/user"
config = Path.home() / ".config/quest-displays"
for directory in (libexec, units, config):
    directory.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(root / "tools/register-desktop.py"), "--native-user-paths"], check=True)
for source, name in [(root / "build/quest-streams", "quest-streams"),
                     (root / "tools/stream-launcher.py", "quest-stream-launcher"),
                     (root / "tools/laptop-display.py", "quest-laptop-display")]:
    with tempfile.NamedTemporaryFile(dir=libexec, delete=False) as file:
        temporary = Path(file.name)
    try:
        shutil.copyfile(source, temporary)
        temporary.chmod(0o755)
        temporary.replace(libexec / name)
    finally:
        temporary.unlink(missing_ok=True)
for name in ("quest-displays.service", "quest-streams.service"):
    shutil.copyfile(root / "host" / name, units / name)
if not (config / "config.json").exists():
    shutil.copyfile(root / "host/config/default.json", config / "config.json")
subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
print("Installed user units. Stop any manual display host, then run:")
print("systemctl --user enable --now quest-displays.service")
