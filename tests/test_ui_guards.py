"""Source guard tests, not a substitute for on-device UI tests."""
import re
import hashlib
import unittest
from pathlib import Path

MAIN = (Path(__file__).resolve().parents[1] / "main/main.cpp").read_text("ascii")
USB = (Path(__file__).resolve().parents[1] / "main/usb_net.cpp").read_text("ascii")

def body(name):

    match = re.search(r"static [^\n]+ " + name + r"\([^\n]*\)\s*\{", MAIN)
    assert match, name
    i, depth = match.end(), 1
    start = i
    while depth:
        depth += (MAIN[i] == "{") - (MAIN[i] == "}")
        i += 1
    return MAIN[start:i-1]

class Guards(unittest.TestCase):
    def test_input_delay(self):
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(16));", MAIN[MAIN.index("extern \"C\" void app_main"):])
    def test_animation_nonblocking(self):
        self.assertNotIn("vTaskDelay", body("move_sel"))
        self.assertIn("g_anim_start", body("move_sel"))
    def test_typing_invalidates(self):
        self.assertIn("g_ui_dirty = true", body("tick_textin"))
    def test_mouse_release(self):
        self.assertIn("hid_mouse(0, 0, 0, 0)", body("tick_mouse").split("leave_screen()")[0])
    def test_lazy_wired(self):
        self.assertNotIn("wifi_net_start", body("sync_localserve"))
        self.assertNotIn("wifi_net_start", body("act_ls_wired"))
        self.assertIn("ls_prepare_wifi", body("act_ls_wireless"))
    def test_gzip_fixture_matches_firmware(self):
        source = (Path(__file__).resolve().parents[1] / "main/localserve.cpp").read_text("ascii")
        fixture = (Path(__file__).parent / "test_gzip.cpp").read_text("ascii")
        start = source.index("static bool ae_accepts_gzip(")
        function = source[start:source.index("static bool client_accepts_gzip(", start)].strip()
        self.assertIn(function, fixture)
    def test_path_fixture_matches_firmware(self):
        fixture = (Path(__file__).parent / "test_path_push.cpp").read_text("ascii")
        start = MAIN.index("static bool path_push(")
        function = MAIN[start:MAIN.index("// Drop the last path", start)].strip()
        self.assertIn(function, fixture)
    def test_path_guard(self):
        b = body("path_push")
        self.assertLess(b.index("return false"), b.index("memcpy"))
        self.assertIn("n + slash + len >= sizeof(g_path)", b)
    def test_every_screen_draw_guard(self):
        loop = MAIN[MAIN.index("extern \"C\" void app_main"):]
        for v in ("KB", "MOUSE", "PAD", "FILES", "FILEACT", "TEXTIN", "CONFIRM", "DUCK", "DUCKRUN", "WIFISCAN", "WIFISTAT", "HOTSPOT", "CAPPICK", "CAPTIVE", "SNIFF"):
            self.assertRegex(loop, "case VIEW_" + v + r":.*redraw \|\| g_ui_dirty")
    def test_ncm_descriptors_unchanged(self):
        begin, end = "enum { ITF_NUM_NET", "static const char* TAG"
        self.assertEqual(hashlib.sha256(USB[USB.index(begin):USB.index(end)].encode()).hexdigest(), "f92a4f0ca2388cf14638512eee1529b07285ffbb7d93de1a2ba0eb3539f92398")
    def test_pm_default_safe(self):
        self.assertIn("static bool g_power_save = false;", MAIN)
        self.assertIn("cfg.light_sleep_enable = false;", MAIN)
        self.assertIn("g_power_save ? 80 : 240", MAIN)
    def test_no_sd_handle_inflation(self):
        sd = (Path(__file__).resolve().parents[1] / "main/sd_store.cpp").read_text()
        self.assertRegex(sd, r"max_files\s*=\s*4;")

if __name__ == "__main__":
    unittest.main(verbosity=2)
