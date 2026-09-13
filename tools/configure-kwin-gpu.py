#!/usr/bin/env python3
"""Prepare Intel-first KWin DRM ordering without restarting the session."""
import argparse
import os
from pathlib import Path
import subprocess
from gpu_devices import discover

MARKER = '# Managed by quest configure-kwin-gpu.py\n'


def script(devices):
    ordered = sorted((d for d in devices if 'card' in d), key=lambda d: (d['vendor'] != '0x8086', d['pci']))
    if not any(d['vendor'] == '0x8086' for d in ordered):
        raise ValueError('MANUAL GPU ENABLE REQUIRED: no Intel DRM card')
    # Resolve stable PCI links at every login. KWin 5.27 splits on unescaped
    # colons; resolving here avoids the escaping required for PCI by-path names.
    paths = ['"$(readlink -f /dev/dri/by-path/pci-' + d['pci'] + '-card)"' for d in ordered]
    return MARKER + 'quest_drm_devices=""\nquest_drm_ok=1\nfor quest_drm_card in ' + ' '.join(paths) + '; do\n' + \
        '    if [ ! -c "$quest_drm_card" ]; then quest_drm_ok=0; break; fi\n' + \
        '    quest_drm_devices="${quest_drm_devices:+$quest_drm_devices:}$quest_drm_card"\ndone\n' + \
        'if [ "$quest_drm_ok" = 1 ]; then export KWIN_DRM_DEVICES="$quest_drm_devices"; fi\n' + \
        'unset quest_drm_devices quest_drm_card quest_drm_ok\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    g = p.add_mutually_exclusive_group(required=True)
    for action in ('dry-run', 'apply', 'revert'):
        g.add_argument('--' + action, action='store_true')
    args = p.parse_args()
    # Native user config, not Snap's inherited XDG_CONFIG_HOME.
    target = Path.home() / '.config/plasma-workspace/env/quest-intel-primary.sh'
    if target.exists() and not target.read_text().startswith(MARKER):
        p.error(f'Refusing to overwrite an unmanaged file: {target}')
    if args.revert:
        target.unlink(missing_ok=True)
        print(f'Removed only {target}. MANUAL SESSION RESTART REQUIRED to clear its session effect.')
        return
    try:
        content = script(discover())
    except ValueError as e:
        p.error(str(e))
    print(f'{target}:\n{content}')
    if args.apply:
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix('.tmp')
        temporary.write_text(content)
        temporary.chmod(0o700)
        os.replace(temporary, target)
        print('MANUAL SESSION RESTART REQUIRED: save work, log out, select Plasma (Wayland).')
    print('Verify later: python3 tools/verify-gpu.py')


if __name__ == '__main__':
    main()
