# Performance

This page states the delay that the relay adds to a stream and explains the status values with which you measure
it on your own PCs.

## Measurement

Setup (2026-11-09): one machine with an RTX PRO 6000, a 640 x 480 `rgb8` stream as JPEG at 30 Hz, source and relay
on the same PC.

| Quantity | Value |
| --- | --- |
| Decode and publish per frame | 3.3 ms |
| Capture stamp to the frame at the reader, median | 5.1 ms |
| Capture stamp to the frame at the reader, maximum | 8 ms |
| Frames lost or overwritten | 0 |

`tests/test_relay.py -s` prints these delays, see [testing.md](testing.md).

## What keeps the delay small

**One decode for all readers.** The cost of a remote stream no longer grows with the number of readers on the PC.

**nvJPEG into GPU memory.** nvJPEG is NVIDIA's JPEG decoder on the GPU. It writes the decoded frame into GPU
memory, and the relay copies it into the CUDA ring. A reader on that GPU receives a device array, which is an array
in GPU memory, without any upload.

**One download, and only on demand.** The host ring receives the frame through one download from the GPU. The relay
skips the download while no reader of the host ring exists.

**The newest frame wins.** If a new frame arrives before the relay has decoded the previous one, the relay drops
the previous one and counts it as `overwritten`. The delay does not accumulate.

## Status values

The relay publishes one status per stream once per second on `<camera>/diagnostics`. Its name is `relay_<stream>`.
Its message states what the relay does: relaying, idle, looking for the source, or why it relays nothing.

| Value | Meaning |
| --- | --- |
| `rate_hz` | Relayed frames per second |
| `decode_publish_ms_mean` | Time to decode a frame and write it into the rings |
| `age_ms_mean` | Capture stamp to the ring, on this PC's clock. It includes the delay of the source and of the network. |
| `transit_ms_mean` | The source's publish time to the arrival here. It compares the clocks of two PCs. |
| `received` | Frames received from the source |
| `relayed` | Frames written into the rings |
| `overwritten` | Frames that a newer frame replaced before the relay relayed them. The relay was too slow for them. |
| `lost` | Gaps in the source's `seq`. The network lost these frames, or the source dropped them. |
| `errors` | Frames that the relay could not decode or publish |

`age_ms_mean` and `transit_ms_mean` are only as exact as the clocks of the two PCs agree.

## Status and limits

We have not measured the relay between two PCs, with larger frames or with several streams.
