import base64
from pathlib import Path
import re
import subprocess
import sys
import unittest
from unittest.mock import mock_open, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "libexec"))
import puzzllm_kilix_bridge as bridge


class PrivateDisplayTest(unittest.TestCase):
    def test_private_windows_start_without_terminal_bars(self):
        with patch.dict(bridge.os.environ), patch("builtins.open", mock_open()), \
             patch.object(bridge.subprocess, "Popen", side_effect=RuntimeError("captured")) as launch:
            with self.assertRaisesRegex(RuntimeError, "captured"):
                bridge.main("kitty", "kitten", 1)
        args = launch.call_args.args[0]
        configs = [args[i + 1] for i, arg in enumerate(args) if arg == "--config"]
        self.assertEqual(configs[-1], "/opt/puzzllm/bin/puzzllm-kilix.conf")
        private = (Path(bridge.__file__).parent / Path(configs[-1]).name).read_text().splitlines()
        for setting in ("tab_bar_style hidden", "window_title_bar_min_windows 0", "hide_window_decorations yes"):
            self.assertIn(setting, private)

    def test_input_and_close_select_only_the_exact_pane_role(self):
        environments = {role: {"PUZZLLM_PANE": role, "PUZZLLM_PANE_INIT": "/opt/puzzllm/bin/puzzllm-agent-init"}
                        for role in ("agent", "shell")}
        with patch.object(bridge, "window_id", return_value="101"), \
             patch.object(bridge.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            for line, expected in (("type agent eA==", "agent"), ("type shell eA==", "shell"), ("close-agent", "agent")):
                bridge.handle(line, "kitten", {})
                cmd = run.call_args.args[0]
                selector = cmd[cmd.index("--match") + 1]
                self.assertTrue(selector.startswith("env:"))
                key, value = selector[4:].split("=", 1)
                matches = [role for role, env in environments.items()
                           if any(re.search(key, k) and re.search(value, v) for k, v in env.items())]
                self.assertEqual(matches, [expected])

    def test_capture_places_both_windows_before_reading_pixels(self):
        windows = {"agent": "", "shell": ""}
        sizes, placed = {}, {}
        with patch.object(bridge, "window_id", side_effect=windows.get), \
             patch.object(bridge.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run, \
             patch.object(bridge.os.path, "isfile", return_value=False):
            bridge.handle("resize agent 888 934", "kitten", sizes)
            bridge.handle("resize shell 888 534", "kitten", sizes)
            bridge.capture(sizes, placed)
            run.assert_not_called()
            windows.update(agent="101", shell="102")
            bridge.capture(sizes, placed)
            commands = [c.args[0] for c in run.call_args_list]
            self.assertEqual(commands[:2], [
                ["xdotool", "windowmove", "101", "0", "0", "windowsize", "101", "888", "934"],
                ["xdotool", "windowmove", "102", "1024", "0", "windowsize", "102", "888", "534"],
            ])
            self.assertEqual([cmd[:3] for cmd in commands[2:]], [
                ["import", "-window", "101"], ["import", "-window", "102"],
            ])
            run.reset_mock()
            windows["agent"] = "103"
            bridge.handle("resize shell 720 400", "kitten", sizes)
            bridge.capture(sizes, placed)
            self.assertEqual(placed, {"agent": ("103", 888, 934), "shell": ("102", 720, 400)})
            self.assertEqual(run.call_args_list[0].args[0][1:5], ["windowmove", "103", "0", "0"])
            self.assertEqual(run.call_args_list[1].args[0][1:5], ["windowmove", "102", "1024", "0"])

    def test_submit_and_restart_share_the_private_kitten(self):
        with patch.object(bridge, "window_id", return_value="101"), \
             patch.object(bridge.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            goal = b"Complete the goal in GOAL.md."
            encoded = base64.b64encode(goal + b"\n").decode()
            bridge.handle("type agent " + encoded, "/private/kitten", {})
            bridge.handle("restart-agent", "/private/kitten", {})
            self.assertEqual(run.call_args_list[0].kwargs["input"], goal + b"\r")
            self.assertEqual([c.args[0][:5] for c in run.call_args_list], [
                ["/private/kitten", "@", "--to", bridge.listen, operation]
                for operation in ("send-text", "close-window", "launch")
            ])


if __name__ == "__main__":
    unittest.main()
