# AGENTS.md

`durchblick-relay`: a C++20 Durchblick node (CMake, one executable) that takes image streams of other PCs once over
Zenoh, decodes them once (nvJPEG or CPU) and republishes them under the same name into this PC's host ring and CUDA
ring. Human documentation: `README.md` (problem, options), `docs/design.md` (source discovery, rules),
`docs/performance.md` (measurement, status values), `docs/testing.md`.
Transport-wide traps (Zenoh sessions and routers, CUDA rings, hanging tests) are in the core repository's AGENTS.md
(github.com/xblick/durchblick). Writing rules for all Markdown: the core's `docs/project/writing-docs.md`.

## Commands

```bash
# configure and build; without the two CUDA options: CPU decoding, host ring only
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<durchblick install> \
  -DDURCHBLICK_RELAY_CUDA=ON -DCUDAToolkit_ROOT=<CUDA 12 toolkit with nvJPEG>
cmake --build build && cmake --install build --prefix build/install
# tests: Durchblick's Python env (dev group), Durchblick's C++ build (durchblick-cpp-camera), zenohd 1.10
DURCHBLICK_CPP_BUILD=$DURCHBLICK_DIR/build DURCHBLICK_RELAY=build/install/bin/durchblick-relay \
ZENOHD=<zenohd 1.10> CUDA_VISIBLE_DEVICES=0 $DURCHBLICK_DIR/.venv/bin/python -m pytest tests/test_relay.py
# run
durchblick-relay --stream <stream> [<stream> ...] [--cuda-device N | --cpu]
# reference check (patterns in the script; also in CI, .github/workflows/ci.yml)
tools/check_references.sh
```

- Tests are skipped unless `DURCHBLICK_CPP_BUILD`, `DURCHBLICK_RELAY` and `ZENOHD` are set. `-s` prints the delay.
- CUDA test cases need a GPU and a build with `DURCHBLICK_RELAY_CUDA=ON`.
- Durations are not recorded. Put a timeout on test invocations.
- Requires Durchblick 0.4 (`find_package(durchblick 0.4 REQUIRED)`; the readers too).

## Rules

- Republish under the stream's own name. Offer only this host's rings (`shm`, and `cuda` with a GPU). Never offer
  `raw`, `compressed` or `camera_info`.
- Pass the source's header (capture stamp, `frame_id`) and attachments `seq` and `source` on unchanged
  (`CameraPublisher::publish(image, Origin)`).
- Set `relayed_from` in the info record (`PublisherOptions::relayed_from`, `transports`).
- Never relay a record with `relayed_from` set.
- Never relay a source that writes rings of its own. Log why the relay stands back.
- One relay per stream in the whole network. Stand back for a relay on a host with a lower id. On one host, the lock
  file `/dev/shm/durchblick-relay.<stream>.lock` decides.
- Subscribe to the source only while a local reader takes a ring. Drop it `--idle-ms` after the last reader left.
- Keep a source until three discovery rounds miss it (`kMissesBeforeDrop`). A failed query changes nothing.
- Compile with `-Wall -Wextra`. Guard all nvJPEG code with `#ifdef DURCHBLICK_RELAY_CUDA`.

## Traps

| Symptom | Cause and fix |
| --- | --- |
| A reader keeps `compressed` although the relay is up | The reader has a fixed transport, or a Durchblick older than 0.3.60, so it does not ask. Name the stream with `--stream`, or let the reader use `auto`. Check the relay's log for "a reader on this PC asks for it". |
| The relay relays nothing and logs that it stands back | The source has a ring on this PC (its readers take it), or another relay on this PC holds the stream's lock file. |
| The relay sees only the source or only another relay, at random | Durchblick older than 0.3.50: Zenoh merged the info replies of source and relay into one. |
| `--cuda-device: this build has no nvJPEG (DURCHBLICK_RELAY_CUDA off)` | Rebuild with `-DDURCHBLICK_RELAY_CUDA=ON -DCUDAToolkit_ROOT=...`. |
| `--cpu and --cuda-device exclude each other` | Pass one of them. |
| Relaying a local source in tests | Works for a source without a ring on this host: `durchblick-cpp-camera --transports raw,compressed`, or a Python publisher made while `_core.host_id` is patched to another host. |
| `age_ms_mean` or `transit_ms_mean` look wrong | They compare the clocks of two PCs. Check the clock synchronisation. |
| `overwritten` grows | The relay is too slow for the source's rate. |
| `lost` grows | Gaps in the source's `seq`: the network lost frames or the source dropped them. |
| The relay dies with `std::length_error` (`basic_string::_M_create`) when it drops a stream | Durchblick 0.3.64: `CameraPublisher` answered an info query while its destructor ran (docs/design.md). Build against a Durchblick with the fix. Naming the streams with `--stream` avoids the drop, but the fix makes it unnecessary. |

## Layout

| Path | Content |
| --- | --- |
| `CMakeLists.txt` | one target `durchblick-relay`, option `DURCHBLICK_RELAY_CUDA` (default OFF) |
| `src/main.cpp` | argument parsing, Zenoh session, one `RelayStream` per stream, status on `<camera>/diagnostics` |
| `src/relay.hpp`, `src/relay.cpp` | `RelayOptions`, `RelayStats`, `RelayStream`: discovery, host lock, subscription on demand, decode, publish |
| `src/nvjpeg_decoder.hpp`, `src/nvjpeg_decoder.cpp` | nvJPEG decode into device memory (CUDA builds only) |
| `tests/test_relay.py` | pytest tests against Durchblick's C++ test camera |
| `docs/` | human documentation |

## Defaults

| Item | Value |
| --- | --- |
| Options | `--slots 32`, `--idle-ms 2000`, `--zenoh-mode peer` (also `--mode`), no `--prefer-raw`; `--streams` is an alias of `--stream` |
| Device | GPU 0 if there is one (nvJPEG into the CUDA ring), else CPU (host ring only) |
| Session | the PC's Zenoh router, 127.0.0.1:7447, unless `--connect` or `--listen` |
| Discovery | info query every 5 s (`RelayOptions::rediscover`), every 1 s while no source, query timeout 1 s |
| Status | `<camera>/diagnostics`, name `relay_<stream>`, once per second; `<camera>` is the stream without its last part |
| Keys | `<base>/info`, `<base>/image_raw/shm/<host>`, `<base>/image_raw/cuda/<host>/<gpu>` |
| Example | `durchblick-relay --stream robot/ecam24/left robot/ecam24/right` |
| Environment (tests) | `DURCHBLICK_CPP_BUILD`, `DURCHBLICK_RELAY`, `ZENOHD`, `CUDA_VISIBLE_DEVICES` |
