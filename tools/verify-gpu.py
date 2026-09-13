#!/usr/bin/env python3
"""Read GPU inventory and KWin's actual renderer; no benchmark or restart."""
import json
import shutil
import subprocess
from gpu_devices import discover

print(json.dumps(discover(), indent=2))
for command in ('qdbus6', 'qdbus'):
    if shutil.which(command):
        result = subprocess.run([command, 'org.kde.KWin', '/KWin', 'supportInformation'],
                                capture_output=True, text=True, timeout=10)
        if result.returncode == 0:
            print('\n'.join(line for line in result.stdout.splitlines()
                            if any(key in line for key in ('KWin version:', 'OpenGL vendor', 'OpenGL renderer', 'Driver:'))))
            break
else:
    print('Cannot query KWin; run qdbus org.kde.KWin /KWin supportInformation in Plasma.')
