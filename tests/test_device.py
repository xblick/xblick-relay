"""xblick-relay's choice of the decoder at its start: nvJPEG on GPU 0 unless --cpu, and a stop with one line where
nvJPEG cannot run. No Zenoh router and no Durchblick needed; any Python with pytest:

    XBLICK_RELAY=<xblick-relay program> python -m pytest tests/test_device.py

A program of the wheel (xblick_relay/bin/xblick-relay, its module in xblick_relay/lib) is also checked without
NVIDIA's packages: a copy of the package outside its site-packages does not find them."""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

RELAY = os.environ.get("XBLICK_RELAY")
pytestmark = pytest.mark.skipif(not RELAY, reason="XBLICK_RELAY is needed")


def help_text() -> str:
    return subprocess.run([RELAY, "--help"], capture_output=True, text=True, timeout=10, check=True).stdout


NVJPEG = bool(RELAY) and "decoders of this build: nvJPEG" in help_text()
needs_nvjpeg = pytest.mark.skipif(not NVJPEG, reason="the relay is built without nvJPEG")


def start(*args, program=None, env=None, timeout=20):
    """The relay without a router: a peer that listens on loopback only. Returns (ready, returncode, stderr)."""
    cmd = [program or RELAY, "--listen", "tcp/127.0.0.1:0", *args]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            env=dict(os.environ, **(env or {})))
    try:
        line = proc.stdout.readline().strip()
        if line == "ready":
            proc.terminate()
        _, err = proc.communicate(timeout=timeout)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    return line == "ready", proc.returncode, err


def error_line(err: str) -> str:
    lines = [line for line in err.splitlines() if line.strip()]
    assert len(lines) == 1, err
    return lines[0]


def test_cpu_needs_no_gpu():
    ready, _, err = start("--cpu", env={"CUDA_VISIBLE_DEVICES": ""})
    assert ready, err
    assert "on the CPU (shm only)" in err


@needs_nvjpeg
def test_the_default_without_a_device_stops():
    """GPU 0 is the default; without a visible device the relay stops and names --cpu, it does not take the CPU."""
    ready, code, err = start(env={"CUDA_VISIBLE_DEVICES": ""})
    assert not ready and code == 1, err
    line = error_line(err)
    assert "error: nvJPEG on GPU 0: " in line and "pass --cpu to decode on the CPU" in line, line
    assert "no CUDA device is visible (CUDA_VISIBLE_DEVICES='')" in line or "no NVIDIA driver" in line, line


@needs_nvjpeg
def test_a_named_device_without_a_device_stops():
    ready, code, err = start("--cuda-device", "0", env={"CUDA_VISIBLE_DEVICES": ""})
    assert not ready and code == 1, err
    assert "pass --cpu" in error_line(err)


@needs_nvjpeg
def test_the_wheel_without_nvidia_packages_stops(tmp_path):
    """The wheel's program in a copy of its package (with the libraries auditwheel put beside it) outside the
    site-packages that holds NVIDIA's packages: nvJPEG cannot load, the relay stops and names the packages; --cpu
    still works."""
    program = Path(RELAY).resolve()
    package = program.parent.parent
    if not (package.name == "xblick_relay" and (package / "lib" / "libxblick-relay-nvjpeg.so").is_file()):
        pytest.skip("not the wheel's program (no xblick_relay/lib/libxblick-relay-nvjpeg.so beside it)")
    shutil.copytree(package, tmp_path / "xblick_relay")
    libs = package.parent / "xblick_relay.libs"
    if libs.is_dir():
        shutil.copytree(libs, tmp_path / "xblick_relay.libs")
    copy = tmp_path / "xblick_relay" / "bin" / "xblick-relay"
    ready, code, err = start(program=str(copy))
    assert not ready and code == 1, err
    line = error_line(err)
    assert "cannot load nvJPEG" in line and "libnvjpeg.so.12" in line, line
    assert "nvidia-nvjpeg-cu12 and nvidia-cuda-runtime-cu12" in line and "pass --cpu" in line, line
    ready, _, err = start("--cpu", program=str(copy))
    assert ready, err
