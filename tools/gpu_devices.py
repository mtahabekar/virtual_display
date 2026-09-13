"""DRM discovery by PCI identity, independent of card/render numbering."""
from pathlib import Path
import json


def discover(sysroot=Path('/sys/class/drm'), devroot=Path('/dev/dri')):
    devices = {}
    for node in sorted(sysroot.iterdir()):
        if not (node.name.startswith('renderD') or
                (node.name.startswith('card') and node.name[4:].isdigit())):
            continue
        try:
            pci = (node / 'device').resolve(strict=True)
            vendor = (pci / 'vendor').read_text().strip()
            if vendor not in ('0x8086', '0x10de'):
                continue
            entry = devices.setdefault(str(pci), {'pci': pci.name, 'vendor': vendor,
                'driver': (pci / 'driver').resolve().name})
            kind = 'render' if node.name.startswith('renderD') else 'card'
            stable = devroot / 'by-path' / f'pci-{pci.name}-{kind}'
            entry[kind] = str(stable if stable.exists() else devroot / node.name)
            entry[kind + '_node'] = str(devroot / node.name)
        except OSError:
            continue
    return sorted(devices.values(), key=lambda d: d['pci'])


if __name__ == '__main__':
    print(json.dumps(discover(), indent=2))
