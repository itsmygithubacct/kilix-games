import base64
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import Mock, mock_open, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "libexec"))
import puzzllm_kilix_bridge as bridge


class PrivateDisplayTest(unittest.TestCase):
    def test_restricted_coding_tools_can_observe_and_focus_a_specific_pane(self):
        with patch.object(bridge.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"shell text")) as run:
            read = ["get-text", "--match", "id:2", "--extent=screen"]
            self.assertEqual(bridge.restricted_control("kitten", read), (0, b"shell text"))
            self.assertEqual(run.call_args.args[0], ["kitten", "@", "--to", bridge.listen, *read])
            self.assertEqual(bridge.restricted_control("kitten", ["focus-window", "--match", "id:1"])[0], 0)
            run.reset_mock()
            for argv in (["launch", "/bin/sh"], ["get-text", "--match", "all"], ["ls", "--all-env-vars"]):
                self.assertEqual(bridge.restricted_control("kitten", argv)[0], 2)
            run.assert_not_called()

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

    def test_goal_click_and_idle_delivery_target_only_the_coding_window(self):
        with TemporaryDirectory() as folder, patch.object(bridge, "root", folder), \
             patch.object(bridge, "agent_window", return_value=([], 9)), \
             patch.object(bridge, "ready_for_goal", side_effect=[False, True, True]), \
             patch.object(bridge.subprocess, "run") as run:
            request = Path(folder, "goals", "auto-request")
            request.parent.mkdir()
            request.write_text("11")
            bridge.handle("auto-goal 10", "kitten", {})
            bridge.handle("auto-goal 11", "kitten", {})
            run.assert_not_called()
            bridge.handle("auto-goal 11", "kitten", {})
            bridge.handle("auto-goal 11", "kitten", {})
            self.assertEqual(run.call_count, 2)
            self.assertEqual(Path(folder, "auto-receipt").read_text(), "11 sent\n")
            request.write_text("12")
            bridge.handle("auto-goal 12", "kitten", {})
            bridge.handle("goal agent", "kitten", {})
            self.assertEqual(run.call_count, 6)
            for typed, enter in zip(run.call_args_list[::2], run.call_args_list[1::2]):
                self.assertEqual(typed.kwargs["input"], b"complete the goal in GOAL.md")
                self.assertEqual(typed.args[0][4:], ["send-text", "--match", "id:9", "--stdin"])
                self.assertEqual(enter.args[0][4:], ["send-key", "--match", "id:9", "enter"])

    def test_native_idle_verdict_for_session_and_composer(self):
        try:
            from kilix_tui import pane_center
            import idleness
        except ImportError:
            self.skipTest("Kilix readiness modules are not installed in the test path")
        info = Mock()
        info.pane.id = 12
        with patch.object(pane_center.Inspector, "snapshot", return_value=Mock(panes=[info])), \
             patch.object(bridge.subprocess, "run") as capture:
            for provider, prompt in [("codex", "› "), ("claude", "────────────\n❯ \n────────────")]:
                for activity, screen, expected in [
                    ("working", prompt, False), ("waiting", prompt, False), ("idle", prompt, True),
                    ("idle", prompt.replace("› ", "› draft").replace("❯ ", "❯ draft"), False),
                    ("agent", prompt, False), ("idle", "", False),
                ]:
                    with self.subTest(provider=provider, activity=activity, screen=screen), \
                         patch.dict(bridge.os.environ, PUZZLLM_AGENT=provider):
                        info.to_dict.return_value = {"activity": activity, "coding_session": {"provider": provider}}
                        capture.return_value.stdout = screen
                        self.assertEqual(bridge.ready_for_goal("kitten", [], 12), expected)

    def test_uncertain_enter_is_never_replayed(self):
        with TemporaryDirectory() as folder, patch.object(bridge, "root", folder), \
             patch.object(bridge, "agent_window", return_value=([], 9)), \
             patch.object(bridge, "ready_for_goal", return_value=True), \
             patch.object(bridge.subprocess, "run", side_effect=[None, subprocess.TimeoutExpired("enter", 2)]) as run:
            request = Path(folder, "goals", "auto-request")
            request.parent.mkdir()
            request.write_text("1")
            bridge.handle("auto-goal 1", "kitten", {})
            bridge.handle("auto-goal 1", "kitten", {})
            self.assertEqual(run.call_count, 2)
            self.assertEqual(Path(folder, "auto-receipt").read_text(), "1 attempted\n")

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
