import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("doctor", Path(__file__).resolve().parents[1] / "host/diagnostics/doctor.py")
doctor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(doctor)


def config():
    return {"outputs": [
        {"name": f"Virtual-QUEST-{i + 1}", "id": i + 2, "connected": True, "enabled": True,
         "scale": 1, "currentModeId": "1", "pos": {"x": i * 2560, "y": 0},
         "modes": [{"id": "1", "size": {"width": 2560, "height": 1440}, "refreshRate": 60}]}
        for i in range(3)]}


class OutputVerification(unittest.TestCase):
    def test_valid_with_physical_display(self):
        c = config()
        c["outputs"].append({"name": "eDP-1", "id": 1, "enabled": True})
        errors, outputs = doctor.verify_outputs(c)
        self.assertEqual(errors, [])
        self.assertEqual(len(outputs), 3)

    def test_advertised_mode_is_not_current_mode(self):
        c = config()
        c["outputs"][0]["modes"].append({"id": "2", "size": {"width": 1920, "height": 1080}, "refreshRate": 60})
        c["outputs"][0]["currentModeId"] = "2"
        self.assertTrue(doctor.verify_outputs(c)[0])

    def test_duplicate_missing_extra_disabled_and_scale(self):
        for mutate in (
            lambda c: c["outputs"].pop(),
            lambda c: c["outputs"].append(copy.deepcopy(c["outputs"][0])),
            lambda c: c["outputs"][0].update(name="Virtual-QUEST-4"),
            lambda c: c["outputs"][0].update(enabled=False),
            lambda c: c["outputs"][0].update(scale=1.25),
            lambda c: c["outputs"][0].update(id=3),
        ):
            with self.subTest(mutate=mutate):
                c = config()
                mutate(c)
                self.assertTrue(doctor.verify_outputs(c)[0])

    def test_unprefixed_and_numeric_mode_ids(self):
        c = config()
        for o in c["outputs"]:
            o["name"] = o["name"].removeprefix("Virtual-")
            o["currentModeId"] = 1
        self.assertFalse(doctor.verify_outputs(c)[0])

    def test_logical_size_cannot_mask_missing_mode(self):
        c = config()
        c["outputs"][0]["size"] = {"width": 2560, "height": 1440}
        c["outputs"][0]["modes"] = []
        self.assertTrue(doctor.verify_outputs(c)[0])


if __name__ == "__main__":
    unittest.main()
