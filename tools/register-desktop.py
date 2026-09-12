#!/usr/bin/env python3
"""Install only the M1 binary and required KDE interface declaration for this user."""
import argparse
import filecmp
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/quest-displays")
    parser.add_argument("--native-user-paths", action="store_true",
                        help="Use normal Plasma data/config/cache paths when invoked from a Snap IDE")
    args = parser.parse_args()
    env = dict(os.environ)
    if args.native_user_paths:
        env.update(XDG_DATA_HOME=str(Path.home() / ".local/share"),
                   XDG_CONFIG_HOME=str(Path.home() / ".config"),
                   XDG_CACHE_HOME=str(Path.home() / ".cache"),
                   XDG_DATA_DIRS="/usr/local/share:/usr/share:/var/lib/snapd/desktop")
    source = args.binary.resolve(strict=True)
    dest = Path.home() / ".local/libexec/quest-displays"
    applications = Path(env.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "applications"
    dest.parent.mkdir(parents=True, exist_ok=True)
    applications.mkdir(parents=True, exist_ok=True)
    # Atomic replacement avoids truncating an executable that might be running.
    # Do not replace an identical running executable: /proc/PID/exe would
    # otherwise acquire a deleted suffix and invalidate live-state checks.
    if not dest.exists() or not filecmp.cmp(source, dest, shallow=False):
        with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as tmp:
            temp = Path(tmp.name)
        try:
            shutil.copyfile(source, temp)
            temp.chmod(0o755)
            temp.replace(dest)
        finally:
            temp.unlink(missing_ok=True)
    # Desktop Exec escaping is distinct from shell quoting.
    escaped = str(dest).replace("\\", "\\\\\\\\").replace('"', '\\\\"').replace("`", "\\\\`").replace("$", "\\\\$").replace("%", "%%")
    desktop = applications / "org.questdisplays.Host.desktop"
    desktop.write_text(
        "[Desktop Entry]\nType=Application\nName=Quest Displays Host\n"
        "Comment=Own three KWin virtual monitors\nNoDisplay=true\n"
        f'Exec="{escaped}" --run\nTerminal=false\n'
        # KDE's custom QStringList property uses commas, not XDG semicolons.
        "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n"
    )
    print(f"Installed {dest}\nRegistered {desktop}")
    try:
        result = subprocess.run(["kbuildsycoca5", "--noincremental"],
                                env={**env, "QT_QPA_PLATFORM": "offscreen"}, timeout=30)
    except subprocess.TimeoutExpired:
        raise SystemExit("KDE cache refresh timed out; from a Snap IDE retry with --native-user-paths")
    if result.returncode:
        raise SystemExit("KDE desktop cache refresh failed; retry kbuildsycoca5 --noincremental before running the host")
    print(f"After logging into Plasma (Wayland): {dest} --run")
    print("No service or autostart entry was installed. Stop a running host before reinstalling.")


if __name__ == "__main__":
    main()
