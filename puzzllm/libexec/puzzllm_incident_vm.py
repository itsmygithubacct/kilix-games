"""Apply a bounded maintenance work order in a fresh, network-isolated Ubuntu VM."""
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
from puzzllm_incident import ACTIONS, atomic

IMAGE = "noble-20261105.img"
DIGEST = "406630c08b5e545fcc7209854b129d47e3af822db128c9f19336593b5a2d3fe0"
URL = ("https://cloud-images.ubuntu.com/minimal/releases/noble/release-20261105/"
       "ubuntu-24.04-minimal-cloudimg-amd64.img")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def image():
    cache = Path(os.environ.get("XDG_CACHE_HOME") or Path.home()/".cache") / "puzzllm/incidents"
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    override = os.getenv("PUZZLLM_INCIDENT_IMAGE")
    path = Path(override).resolve() if override else cache/IMAGE
    with (cache/".download.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not path.exists() and not override:
            with tempfile.TemporaryDirectory(dir=cache) as tmp:
                dest = Path(tmp)/IMAGE
                with urlopen(URL, timeout=45) as response, dest.open("wb") as out:
                    shutil.copyfileobj(response, out)
                if digest(dest) != DIGEST:
                    raise ValueError("Ubuntu image checksum mismatch")
                dest.replace(path)
        elif digest(path) != DIGEST:
            raise ValueError("Ubuntu image checksum mismatch")
    return path


def cloud_seed(directory, kind, actions):
    unit = ("[Unit]\nDescription=Verify the repaired incident after reboot\nAfter=dispatch.service\n"
            "[Service]\nType=oneshot\nExecStart=/usr/local/sbin/puzzllm-response\n"
            "[Install]\nWantedBy=multi-user.target\n")
    files = {"/usr/local/sbin/puzzllm-response": Path(__file__).with_name("puzzllm-incident-guest").read_bytes(),
             "/etc/systemd/system/puzzllm-verify.service": unit.encode(),
             "/var/lib/puzzllm/kind": kind.encode(),
             "/var/lib/puzzllm/actions": ("\n".join(actions)+"\n").encode(),
             "/var/lib/puzzllm/baseline": ("dispatch order "+secrets.token_hex(18)+"\n").encode()}
    config = dict(write_files=[dict(path=name, encoding="b64", permissions="0755" if "sbin/" in name else "0644",
                                    content=base64.b64encode(data).decode()) for name,data in files.items()],
                  runcmd=[["/usr/local/sbin/puzzllm-response"]])
    seed = directory/"seed"
    seed.mkdir()
    (seed/"meta-data").write_text("instance-id: "+secrets.token_hex(12)+"\nlocal-hostname: incident\n")
    (seed/"user-data").write_text("#cloud-config\n"+json.dumps(config)+"\n")
    (seed/"network-config").write_text("version: 2\nethernets:\n  private:\n    match: {name: 'en*'}\n    dhcp4: true\n")
    subprocess.run(["xorriso","-as","mkisofs","-quiet","-V","cidata","-r","-J","-o",
                    str(directory/"seed.iso"),str(seed)], check=True)


def verify(directory, kind, actions):
    base = image()
    cloud_seed(directory, kind, actions)
    subprocess.run(["qemu-img","create","-q","-f","qcow2","-F","qcow2","-b",str(base),
                    "guest.qcow2","4G"], cwd=directory, check=True)
    args = ["qemu-system-x86_64","-machine","accel=kvm:tcg","-m","768","-smp","2","-display","none",
            "-monitor","none","-nic","user,model=virtio-net-pci,restrict=on,ipv6=off","-serial","file:console.log","-device","virtio-serial",
            "-chardev","file,id=proof,path=proof","-device","virtserialport,chardev=proof,name=org.puzzllm.result",
            "-drive","file=guest.qcow2,if=virtio,format=qcow2","-drive","file=seed.iso,media=cdrom,readonly=on"]
    with subprocess.Popen(args, cwd=directory, stdin=subprocess.DEVNULL) as vm:
        try:
            deadline = time.monotonic()+360
            while vm.poll() is None and time.monotonic()<deadline:
                if (directory/"proof").exists() and (directory/"proof").stat().st_size:
                    break
                time.sleep(.4)
        finally:
            if vm.poll() is None:
                vm.kill()
            vm.wait()
    proof = directory/"proof"
    result = proof.read_text().strip() if proof.exists() else ""
    if not result.startswith("PASS "):
        console = directory/"console.log"
        if console.exists():
            print(console.read_bytes()[-5000:].decode(errors="replace"), flush=True)
        raise ValueError(result or "Ubuntu did not finish its live check")
    return result[5:]


if __name__ == "__main__":
    receipt, kind, *actions = sys.argv[1:]
    path = Path(receipt)
    try:
        if kind not in ACTIONS or actions != ACTIONS[kind]:
            raise ValueError("Unknown repair work order")
        with tempfile.TemporaryDirectory(dir=path.parent, prefix="guest-") as temp:
            message = verify(Path(temp), kind, actions)
        outcome = [True, message]
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), flush=True)
        outcome = [False, "VM check pending: " + str(error)[:175]]
    atomic(path, json.dumps(outcome))
