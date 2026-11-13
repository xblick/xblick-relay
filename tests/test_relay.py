"""durchblick-relay end to end on one machine: a Zenoh router, Durchblick's C++ test camera as the source offering only
network forms (--transports raw,compressed, as a camera on another PC looks from here), the relay, and Python readers.

Run with Durchblick's Python environment:

    DURCHBLICK_CPP_BUILD=<durchblick build dir with durchblick-cpp-camera> \\
    DURCHBLICK_RELAY=<durchblick-relay binary> ZENOHD=<zenohd 1.10 binary> \\
    <durchblick>/.venv/bin/python -m pytest tests/test_relay.py

The CUDA cases need a GPU and a relay built with DURCHBLICK_RELAY_CUDA."""

import json
import os
import socket
import subprocess
import threading
import time
from pathlib import Path

import numpy as np
import pytest

from blick import _core, diagnostics, topics
from blick.camera import CameraPublisher, ImageSubscriber, query_info
from blick.codecs import decompress
from blick.msg import CompressedImage
from blick.session import open_session

BUILD = os.environ.get("DURCHBLICK_CPP_BUILD")
RELAY = os.environ.get("DURCHBLICK_RELAY")
ZENOHD = os.environ.get("ZENOHD")
pytestmark = pytest.mark.skipif(not (BUILD and RELAY and ZENOHD),
                                reason="DURCHBLICK_CPP_BUILD, DURCHBLICK_RELAY and ZENOHD are needed")
needs_cuda = pytest.mark.skipif(_core.cuda_device_count() == 0, reason="no CUDA device")

W, H = 640, 480
BASE = "t/relay/color"
CAMERA = "t/relay"


def pattern(seq: int) -> np.ndarray:
    """Durchblick's cpp/tools/tools.hpp rgb8 test pattern of frame seq."""
    y, x = np.mgrid[0:H, 0:W]
    return np.stack([(x + 2 * y + 3 * c + seq) & 0xFF for c in range(3)], axis=-1).astype(np.uint8)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Setup:
    """zenohd on a free port; the source, the relay and the test's session are peers connected to it."""

    def __init__(self):
        self.port = free_port()
        self.procs = []
        self.relay_log = []
        self.router = self._start([ZENOHD, "--no-multicast-scouting", "-l", f"tcp/127.0.0.1:{self.port}"])
        time.sleep(1.0)
        self.session = open_session(connect=[f"127.0.0.1:{self.port}"], listen=["127.0.0.1:0"])

    def _start(self, cmd, **kw):
        proc = subprocess.Popen(cmd, **kw)
        self.procs.append(proc)
        return proc

    def source(self, transports="raw,compressed", codec="jpeg", fps=30, encoding="rgb8", base=BASE):
        cmd = [str(Path(BUILD) / "durchblick-cpp-camera"), "--connect", f"127.0.0.1:{self.port}", "--listen",
               "127.0.0.1:0", "--base", base, "--encoding", encoding, "--codec", codec, "--size", str(W), str(H),
               "--fps", str(fps), "--seconds", "120", "--transports", transports]
        proc = self._start(cmd, stdout=subprocess.PIPE, text=True)
        assert proc.stdout.readline().strip() == "ready", "the source did not start"
        return proc

    def relay(self, *extra, stream=True):
        """A relay; ``stream``: with BASE named (--stream), else only the streams that readers ask for."""
        cmd = [RELAY, *(["--stream", BASE] if stream else []), "--connect", f"127.0.0.1:{self.port}", "--listen",
               "127.0.0.1:0", *extra]
        proc = self._start(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert proc.stdout.readline().strip() == "ready", "the relay did not start: " + proc.stderr.read()
        threading.Thread(target=lambda: [self.relay_log.append(line.rstrip()) for line in proc.stderr],
                         daemon=True).start()
        return proc

    def close(self):
        self.session.close()
        for proc in reversed(self.procs):
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()


@pytest.fixture
def setup():
    s = Setup()
    yield s
    s.close()


def wait_for(condition, timeout=15.0, what="condition"):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = condition()
        if value:
            return value
        time.sleep(0.05)
    raise TimeoutError(what)


def frames(sub, n, timeout=20.0):
    out = []
    end = time.monotonic() + timeout
    while len(out) < n and time.monotonic() < end:
        frame = sub.latest(timeout=0.1)
        if frame is not None:
            out.append(frame)
    if len(out) < n:
        raise TimeoutError(f"{len(out)} of {n} frames from {sub.base} ({sub.transport_reason})")
    return out


def relay_status(setup, timeout=5.0):
    """The newest relay_color status on t/relay/diagnostics, as {key: value} plus level and message."""
    got = {}

    def on_sample(sample):
        _, array = diagnostics.read(sample)
        for status in array.status:
            if status.name == "relay_color":
                got["status"] = status

    sub = setup.session.declare_subscriber(f"{CAMERA}/{topics.DIAGNOSTICS}", on_sample)
    try:
        status = wait_for(lambda: got.get("status"), timeout, "a relay status")
    finally:
        sub.undeclare()
    values = {kv.key: kv.value for kv in status.values}
    return dict(values, level=status.level, message=status.message)


def assert_decoded_like(relayed: np.ndarray, reference: np.ndarray, seq: int):
    """The relay's frame (nvJPEG) is as good a decode of the source's JPEG as libjpeg-turbo's of the same bytes: close to
    it (they upsample chroma differently, which shows at the test pattern's hard wrap edges) and as close to the
    pattern the source encoded."""
    relayed, reference, truth = relayed.astype(int), reference.astype(int), pattern(seq).astype(int)
    assert np.abs(relayed - reference).mean() < 2.0, np.abs(relayed - reference).mean()
    relay_error, reference_error = np.abs(relayed - truth).mean(), np.abs(reference - truth).mean()
    assert relay_error < 1.25 * reference_error + 0.5, (relay_error, reference_error)


class Compressed:
    """The source's compressed samples by seq (not decoded): what the relay decoded, decoded here as well."""

    def __init__(self, session):
        self.by_seq = {}
        self.sub = session.declare_subscriber(f"{BASE}/{topics.COMPRESSED}", self._on)

    def _on(self, sample):
        seq = json.loads(sample.attachment.to_bytes())["seq"]
        self.by_seq[seq] = CompressedImage.deserialize(sample.payload.to_bytes())

    def close(self):
        self.sub.undeclare()


def test_relay_jpeg_into_shm(setup):
    """The source's JPEG decoded once by the relay, read here from its shared-memory ring: the source's header, seq and
    source; pixels as a JPEG decoder makes them from the same bytes."""
    setup.source()
    setup.relay()
    records = wait_for(lambda: (r := query_info(setup.session, BASE)) and len(r) == 2 and r, what="two records")
    relay = next(r for r in records if r.get("relayed_from"))
    source = next(r for r in records if not r.get("relayed_from"))
    assert relay["transports"][0] == "shm" and "compressed" not in relay["transports"]
    assert relay["relayed_from"] == source["host"] and relay["frame_id"] == "TestCamera"
    compressed = Compressed(setup.session)
    sub = ImageSubscriber(setup.session, BASE, "auto")
    try:
        got = frames(sub, 20)
        assert sub.transport_reason.startswith("shm")
        for frame in got[-5:]:
            assert frame.source == "test" and frame.header.frame_id == "TestCamera" and frame.encoding == "rgb8"
            message = wait_for(lambda: compressed.by_seq.get(frame.seq), what=f"compressed frame {frame.seq}")
            assert message.header.stamp.to_ns() == frame.header.stamp.to_ns()
            assert_decoded_like(frame.image, decompress(message)[0], frame.seq)
        seqs = [f.seq for f in got]
        assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs)
        status = relay_status(setup)
        assert status["level"] == 0 and status["message"].startswith("relaying"), status
        assert float(status["rate_hz"]) > 20, status
    finally:
        sub.close()
        compressed.close()


@needs_cuda
def test_relay_jpeg_into_cuda(setup):
    """A reader on the GPU takes the relay's CUDA ring: the frames never left the device on this host."""
    setup.source()
    setup.relay("--cuda-device", "0")
    # auto chooses once, when the reader starts: the relay's record must be there by then
    wait_for(lambda: any(r.get("relayed_from") for r in query_info(setup.session, BASE)), what="the relay's record")
    compressed = Compressed(setup.session)
    sub = ImageSubscriber(setup.session, BASE, "auto", device="cuda:0")
    try:
        got = frames(sub, 20)
        assert sub.transport_reason.startswith("cuda"), sub.transport_reason
        frame = got[-1]
        message = wait_for(lambda: compressed.by_seq.get(frame.seq), what="its compressed frame")
        image = frame.image.numpy() if hasattr(frame.image, "numpy") else np.asarray(frame.image)
        assert_decoded_like(image, decompress(message)[0], frame.seq)
        assert frame.source == "test" and frame.header.stamp.to_ns() == message.header.stamp.to_ns()
    finally:
        sub.close()
        compressed.close()


def test_relay_png_on_the_cpu_is_exact(setup):
    """PNG (lossless) is decoded on the CPU: the frames are the source's pixels exactly."""
    setup.source(codec="png")
    setup.relay()
    sub = ImageSubscriber(setup.session, BASE, "shm")
    try:
        for frame in frames(sub, 10)[-3:]:
            np.testing.assert_array_equal(frame.image, pattern(frame.seq))
    finally:
        sub.close()


def test_relay_rvl_depth_is_exact(setup):
    """Depth as RVL (lossless, Durchblick 0.3.62) is decoded on the CPU: the frames are the source's pixels exactly."""
    setup.source(codec="rvl", encoding="16UC1")
    setup.relay()
    sub = ImageSubscriber(setup.session, BASE, "shm")
    try:
        y, x = np.mgrid[0:H, 0:W]
        for frame in frames(sub, 10)[-3:]:  # cpp/tools/tools.hpp's 16-bit test pattern
            assert frame.encoding == "16UC1"
            np.testing.assert_array_equal(frame.image, ((7 * x + 13 * y + 5 * frame.seq) & 0xFFFF).astype(np.uint16))
        assert relay_status(setup)["source_transport"] == "compressed"
    finally:
        sub.close()


def test_relay_raw_is_exact(setup):
    """A source without a compressed form: the relay takes raw and copies it into the ring."""
    setup.source(transports="raw")
    setup.relay("--cpu")
    sub = ImageSubscriber(setup.session, BASE, "shm")
    try:
        for frame in frames(sub, 10)[-3:]:
            np.testing.assert_array_equal(frame.image, pattern(frame.seq))
        assert relay_status(setup)["source_transport"] == "raw"
    finally:
        sub.close()


def test_relay_takes_the_source_only_on_demand(setup):
    """Without a reader of its rings the relay does not subscribe to the source (nothing crosses the network for it);
    with one it relays; after the last reader left it stops again."""
    setup.source()
    setup.relay("--idle-ms", "1000")
    wait_for(lambda: len(query_info(setup.session, BASE)) == 2, what="the relay's record")
    time.sleep(2.0)
    idle = relay_status(setup)
    assert idle["message"].startswith("idle") and idle["received"] == "0", idle
    sub = ImageSubscriber(setup.session, BASE, "shm")
    frames(sub, 5)
    sub.close()
    time.sleep(3.0)
    after = relay_status(setup)
    assert after["message"].startswith("idle"), after
    received = int(after["received"])
    time.sleep(1.5)
    assert int(relay_status(setup)["received"]) == received  # nothing taken while idle


def test_relay_refuses_a_local_source_with_a_ring(setup):
    """A source that publishes a ring on this host is read here directly: the relay offers nothing."""
    setup.source(transports="raw,compressed,shm")
    setup.relay()
    time.sleep(3.0)
    records = query_info(setup.session, BASE)
    assert len(records) == 1 and not records[0].get("relayed_from")
    status = relay_status(setup)
    assert "nothing to relay" in status["message"], status


def test_a_second_relay_on_the_host_stands_back(setup):
    setup.source()
    setup.relay("--cpu")
    wait_for(lambda: len(query_info(setup.session, BASE)) == 2, what="the first relay's record")
    setup.relay("--cpu")
    time.sleep(3.0)
    assert len(query_info(setup.session, BASE)) == 2  # still only the source and the first relay
    assert any("another relay of this stream runs on this host" in line for line in setup.relay_log)


def test_relay_latency_report(setup):
    """Not a pass/fail bound: the relay's own time per frame and the age of its frames at 30 Hz, printed for the
    README (pytest -s)."""
    setup.source()
    setup.relay()
    sub = ImageSubscriber(setup.session, BASE, "shm")
    try:
        got = frames(sub, 60)
        ages = [(f.receive_ns - f.header.stamp.to_ns()) / 1e6 for f in got[10:]]
        status = relay_status(setup)
        print(f"\nrelay {W}x{H} rgb8 JPEG at 30 Hz: decode+publish {status['decode_publish_ms_mean']} ms, "
              f"age at the reader median {np.median(ages):.2f} ms, max {max(ages):.2f} ms, status {status}")
        assert np.median(ages) < 50
    finally:
        sub.close()


def test_relay_started_before_its_source(setup):
    """The relay waits for the source and starts relaying once it appears."""
    setup.relay("--cpu")
    time.sleep(2.0)
    assert query_info(setup.session, BASE) == []  # no record of the relay before it has a source
    setup.source()
    wait_for(lambda: any(r.get("relayed_from") for r in query_info(setup.session, BASE)), what="the relay's record")
    sub = ImageSubscriber(setup.session, BASE, "shm")
    try:
        frames(sub, 5)
    finally:
        sub.close()


def test_relay_survives_a_source_restart(setup):
    """The source stops and starts again (its seq starts again at 1): the relay goes on with the new frames."""
    source = setup.source()
    setup.relay("--cpu")
    wait_for(lambda: any(r.get("relayed_from") for r in query_info(setup.session, BASE)), what="the relay's record")
    sub = ImageSubscriber(setup.session, BASE, "shm")
    try:
        before = frames(sub, 5)[-1]
        source.terminate()
        source.wait(10)
        time.sleep(1.0)
        setup.source()
        after = wait_for(lambda: (f := sub.latest(timeout=0.1)) is not None and f.header.stamp.to_ns() >
                         before.header.stamp.to_ns() + 1_000_000_000 and f, 20.0, "frames after the restart")
        assert after.seq < before.seq or after.seq < 100  # the new source counts from 1 again
        np.testing.assert_array_equal(frames(sub, 3)[-1].image.shape, (H, W, 3))
    finally:
        sub.close()


def test_two_relays_started_together_settle_on_one(setup):
    """Two relays of the stream on one host, started at the same moment: one relays, the other stands back, and it
    stays so (no turn-taking that would rebuild the rings)."""
    setup.source()
    setup.relay("--cpu")
    setup.relay("--cpu")
    wait_for(lambda: any(r.get("relayed_from") for r in query_info(setup.session, BASE)), what="a relay's record")
    seen = []
    end = time.monotonic() + 8.0
    while time.monotonic() < end:
        seen.append(sum(1 for r in query_info(setup.session, BASE) if r.get("relayed_from")))
        time.sleep(0.5)
    assert set(seen) == {1}, seen


def test_the_relays_options_are_its_parameters(setup):
    """Every option is a read-only parameter of durchblick/<node> (durchblick::Options): the session's and the
    parameters' own are not; a value given with --param is the one in effect."""
    from blick.parameters import ParameterClient

    setup.source()
    setup.relay("--cpu", "--node", "relay_test", "--param", "slots:=16", "--mode", "peer")  # --mode: the older name
    client = ParameterClient(setup.session, "durchblick/relay_test")
    end = time.monotonic() + 15
    while True:  # the relay's process has just started: ask until it answers
        try:
            descriptors, values = client.all()
            break
        except TimeoutError:
            if time.monotonic() > end:
                raise
    by_name = {d.name: (d, v.value()) for d, v in zip(descriptors, values, strict=True)}
    assert {"stream", "slots", "idle_ms", "cpu", "prefer_raw", "cuda_device"} <= set(by_name)
    assert not {"connect", "listen", "zenoh_mode", "param", "node"} & set(by_name)
    assert by_name["slots"][1] == 16 and by_name["stream"][1] == [BASE] and by_name["cpu"][1] is True
    assert by_name["cuda_device"][1] == ""  # not given
    assert all(d.read_only for d, _ in by_name.values())
    assert by_name["idle_ms"][0].description.startswith("--idle-ms: ")
    refused = client.set({"slots": 8})[0]
    assert not refused.successful and "read-only" in refused.reason
    client.close()


@pytest.mark.parametrize("stop", ["terminate", "kill"])
def test_a_reader_that_started_first_moves_to_the_relay_and_back(setup, stop):
    """The order of starting does not matter (Durchblick 0.3.59): a reader that reads the source over the network moves
    to the ring of a relay that starts later, and back to the network when the relay stops or crashes; every frame
    once."""
    setup.source()
    sub = ImageSubscriber(setup.session, BASE, "auto")
    try:
        got = frames(sub, 10)
        assert sub.transport_reason.startswith("compressed"), sub.transport_reason
        relay = setup.relay("--cpu")
        wait_for(lambda: sub.transport_reason.startswith("shm"), what=f"the relay's ring ({sub.transport_reason})")
        got += frames(sub, 30)
        assert got[-1].key.split("/")[-2] == "shm"
        getattr(relay, stop)()
        relay.wait(10)
        wait_for(lambda: sub.transport_reason.startswith("compressed"),
                 what=f"the source again ({sub.transport_reason})")
        got += frames(sub, 20)
        assert got[-1].key.endswith("/compressed")
        seqs = [f.seq for f in got]
        assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs), "a frame twice or out of order"
    finally:
        sub.close()


def test_a_stream_is_relayed_while_a_reader_on_the_host_asks_for_it(setup):
    """One relay per PC, no stream named: it relays a stream from the first reader with the transport auto on its
    host until --idle-ms after the last. Before and after, the stream has no relay."""
    setup.source()
    setup.relay("--cpu", "--idle-ms", "500", stream=False)
    time.sleep(1.5)
    assert len(query_info(setup.session, BASE)) == 1, "a relay of a stream nobody asked for"
    sub = ImageSubscriber(setup.session, BASE, "auto")
    try:
        wait_for(lambda: sub.transport_reason.startswith("shm"), what=f"the relay's ring ({sub.transport_reason})")
        got = frames(sub, 20)
        assert got[-1].key.split("/")[-2] == "shm" and got[-1].source == "test"
        records = query_info(setup.session, BASE)
        assert sorted(bool(r.get("relayed_from")) for r in records) == [False, True]
        seqs = [f.seq for f in got]
        assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs)
    finally:
        sub.close()
    wait_for(lambda: len(query_info(setup.session, BASE)) == 1, what="the relay lets go of the stream")
    assert any("no reader on this PC asks for it any more" in line for line in setup.relay_log)
    # a second reader: relayed again
    again = ImageSubscriber(setup.session, BASE, "auto")
    try:
        wait_for(lambda: again.transport_reason.startswith("shm"), what="the relay's ring for the second reader")
        frames(again, 5)
    finally:
        again.close()


@pytest.mark.parametrize("device", [pytest.param(("--cpu",), id="cpu"),
                                    pytest.param(("--cuda-device", "0"), id="cuda", marks=needs_cuda)])
def test_two_streams_dropped_together_while_others_ask_for_their_records(setup, device):
    """Two streams asked for by readers with auto (a stereo pair), whose readers leave together: the relay drops both
    at once, while other programs keep querying the streams' info records (readers and relays of other PCs, the
    terminal interface). It must not crash. With Durchblick 0.3.64 it died with std::length_error
    (basic_string::_M_create) in the first round: the core's CameraPublisher answered an info query while its
    destructor had already freed the data of the record."""
    bases = [f"{CAMERA}/left", f"{CAMERA}/right"]
    for base in bases:
        setup.source(base=base)
    relay = setup.relay(*device, "--idle-ms", "500", stream=False)
    stop = threading.Event()
    errors = []

    def ask(base):  # as often as several programs on several PCs together
        while not stop.is_set():
            try:
                query_info(setup.session, base, timeout=0.5)
            except Exception as e:  # a record that is not one: the relay answered with garbage
                errors.append(repr(e))

    askers = [threading.Thread(target=ask, args=(base,), daemon=True) for base in bases for _ in range(4)]
    for thread in askers:
        thread.start()
    try:
        for cycle in range(3):  # each cycle builds both streams' rings and drops them together
            subs = [ImageSubscriber(setup.session, base, "auto") for base in bases]
            try:
                for sub in subs:
                    wait_for(lambda: sub.transport_reason.startswith(("shm", "cuda")),
                             what=f"{sub.base}: the relay's ring ({sub.transport_reason})")
                    frames(sub, 5)
            finally:
                for sub in subs:
                    sub.close()
            wait_for(lambda: all(len(query_info(setup.session, base)) == 1 for base in bases) or relay.poll(),
                     what=f"cycle {cycle}: the relay lets go of both streams")
            assert relay.poll() is None, f"cycle {cycle}: the relay died:\n" + "\n".join(setup.relay_log)
    finally:
        stop.set()
        for thread in askers:
            thread.join()
    assert not errors, errors[:3]
    assert sum("no reader on this PC asks for it any more" in line for line in setup.relay_log) == 2 * 3


def test_a_reader_that_comes_back_within_the_idle_time_keeps_the_stream(setup):
    """A reader that leaves and a new one that asks before --idle-ms passed: the relay keeps the stream (no drop and
    rebuild of its rings), and the new reader takes its ring."""
    setup.source()
    setup.relay("--cpu", "--idle-ms", "3000", stream=False)
    sub = ImageSubscriber(setup.session, BASE, "auto")
    try:
        wait_for(lambda: sub.transport_reason.startswith("shm"), what=f"the relay's ring ({sub.transport_reason})")
        frames(sub, 5)
    finally:
        sub.close()
    time.sleep(1.0)
    again = ImageSubscriber(setup.session, BASE, "auto")
    try:
        wait_for(lambda: again.transport_reason.startswith("shm"), what=f"the relay's ring ({again.transport_reason})")
        frames(again, 5)
    finally:
        again.close()
    assert not any("no reader on this PC asks for it any more" in line for line in setup.relay_log)
    assert sum("a reader on this PC asks for it" in line for line in setup.relay_log) == 1


def test_a_source_with_rings_on_another_host_is_relayed(setup, monkeypatch):
    """A source that writes rings of its own on its host (any Durchblick camera node on another PC) is relayed: the
    rings' keys name their host, so its rings and the relay's do not meet."""
    with monkeypatch.context() as patched:  # as a publisher on another PC is seen from here
        patched.setattr(_core, "host_id", lambda: "0c9a1f6e-5b0d-4a52-9d3e-7f1b2c3d4e5f")
        source = CameraPublisher(setup.session, BASE, encoding="rgb8", frame_id="FarCamera", source="far", codec="png")
    assert "shm" in source.info_record()["transports"]
    stop = threading.Event()

    def publish():
        while not stop.is_set():
            if source.subscribed:
                source.publish(pattern(source.seq + 1))
            time.sleep(1 / 30)

    thread = threading.Thread(target=publish, daemon=True)
    thread.start()
    setup.relay("--cpu", stream=False)
    sub = ImageSubscriber(setup.session, BASE, "auto")
    try:
        wait_for(lambda: sub.transport_reason.startswith("shm"), what=f"the relay's ring ({sub.transport_reason})")
        for frame in frames(sub, 10)[-3:]:
            assert frame.source == "far" and frame.header.frame_id == "FarCamera"
            np.testing.assert_array_equal(frame.image, pattern(frame.seq))  # PNG: exact
    finally:
        sub.close()
        stop.set()
        thread.join()
        source.close()
