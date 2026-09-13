import importlib.util
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), TOOLS / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


launcher = module('stream-launcher')
kwin = module('configure-kwin-gpu')


class GpuConfigTests(unittest.TestCase):
    def test_legacy_config_keeps_nvenc(self):
        self.assertEqual(launcher.encoder_arguments({})[:2], ['--encoder', 'nvenc'])

    def test_explicit_auto_policy(self):
        args = launcher.encoder_arguments({'encoder': 'auto', 'allow_nvenc_fallback': True})
        self.assertIn('--allow-nvenc-fallback', args)
        self.assertNotIn('--allow-nvenc-fallback', launcher.encoder_arguments({'encoder': 'auto'}))

    def test_invalid_config(self):
        for config in ({'allow_nvenc_fallback': 'false'}, {'encoder': 'x264'}, {'gop_frames': True},
                       {'capture_memory': 'cuda'}, {'vbv_ms': 0}):
            with self.assertRaises(ValueError): launcher.encoder_arguments(config)

    def test_kwin_order_resolves_pci_every_login(self):
        devices = [{'pci': '0000:02:00.0', 'vendor': '0x10de', 'card': '/dev/dri/card1'},
                   {'pci': '0000:00:02.0', 'vendor': '0x8086', 'card': '/dev/dri/card9'}]
        script = kwin.script(devices)
        self.assertLess(script.index('pci-0000:00:02.0'), script.index('pci-0000:02:00.0'))
        self.assertNotIn('/dev/dri/card9', script)
        self.assertNotIn('CUDA_VISIBLE_DEVICES', script)
        with self.assertRaises(ValueError): kwin.script(devices[:1])
