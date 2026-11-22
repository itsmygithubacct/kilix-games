"""Defensive incident rooms, evidence isolation and the live campaign door."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "libexec"))
import puzzllm_incident as incident
from puzzllm_challenges import Incorrect


class IncidentRooms(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.state = Path(temp.name)
        self.workspace = self.state / "workspace"
        self.workspace.mkdir()

    def cli(self, *args):
        return subprocess.run([ROOT / "build/puzzllm", *args], capture_output=True, text=True,
                              env=dict(os.environ, PUZZLLM_ROOT=str(ROOT), PUZZLLM_STATE=str(self.state)),
                              timeout=15)

    def test_evidence_report_and_reset(self):
        for room, kind in ((19,"ssh"),(20,"root")):
            (self.state/"progress").write_text(f"level {room}\n")
            name = f"{kind}-incident.json"
            initial = self.cli("--check-level", str(room))
            self.assertEqual(initial.returncode, 1, initial)
            self.assertIn(name, initial.stdout)
            log = self.state / "incident-evidence" / (kind+".log")
            self.assertTrue(log.is_file(), log)
            case, fields = incident.prepare(self.state, kind)
            for key, value in fields.items():
                if key != "advisory":
                    self.assertIn(value, log.read_text())
            events = [line.split() for line in log.read_text().splitlines()]
            if kind == "ssh":
                host = next(row for row in events if "alarm;" in row)
                gateway = next(row for row in events if row[1]=="gateway" and host[2] in row)
                self.assertEqual((gateway[0],host[0]), (fields["gateway_event"],fields["server_event"]))
            else:
                host = next(row for row in events if "authorization=absent" in row)
                gateway = next(row for row in events if row[1]=="login" and host[2] in row)
                self.assertEqual((gateway[0],host[0]), (fields["login_event"],fields["root_event"]))
            uid = next(field for field in gateway if field.startswith("uid="))[4:]
            self.assertIn(["directory",uid,fields["account"]],events)
            report = dict(fields, repair=incident.ACTIONS[kind])
            for field in fields:
                with self.assertRaises(Incorrect):
                    incident.diagnose(kind,json.dumps(dict(report, **{field:"incorrect"})),fields)
            for actions in ([], list(reversed(report["repair"])), ["disable-service"]):
                with self.assertRaises(Incorrect):
                    incident.diagnose(kind,json.dumps(dict(report,repair=actions)),fields)
            (self.workspace/name).write_text(json.dumps(dict(report,repair=[])))
            self.assertIn("Missing repair", self.cli("--goal-status").stdout)
            self.workspace.rename(self.state/("old-"+kind))
            self.workspace.mkdir()
            newcase, fresh = incident.prepare(self.state, kind)
            self.assertNotEqual(newcase,case)
            with self.assertRaises(Incorrect):
                incident.diagnose(kind,json.dumps(report),fresh)

    @unittest.skipUnless(os.getenv("PUZZLLM_LIVE_INCIDENTS")=="1","opt-in Ubuntu VM checks")
    def test_incomplete_guest_repairs(self):
        from puzzllm_incident_vm import verify
        for kind, omitted in (("ssh","restart-ssh"), ("root","remove-unapproved-unit")):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory(dir=self.state) as temp:
                actions = [action for action in incident.ACTIONS[kind] if action != omitted]
                with self.assertRaisesRegex(ValueError, "FAIL guest check"):
                    verify(Path(temp), kind, actions)

    @unittest.skipUnless(os.getenv("PUZZLLM_LIVE_INCIDENTS")=="1","opt-in Ubuntu VM checks")
    def test_live_doors(self):
        for room, kind in ((19,"ssh"),(20,"root")):
            (self.state/"progress").write_text(f"level {room}\n")
            case, fields = incident.prepare(self.state, kind)
            report = dict(fields,repair=incident.ACTIONS[kind])
            (self.workspace/(kind+"-incident.json")).write_text(json.dumps(report))
            until=time.monotonic()+450
            while time.monotonic()<until:
                check=self.cli("--goal-status")
                if check.returncode==0 or "VM check pending:" in check.stdout:
                    break
                time.sleep(1)
            self.assertEqual(check.returncode,0,check.stdout + (case/"verification.log").read_text()[-1000:])
            self.assertIn("COMPLETE",check.stdout)
            self.assertEqual(self.cli("--check-level",str(room)).returncode,0)


if __name__=="__main__":
    unittest.main()
