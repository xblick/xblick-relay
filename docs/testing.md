# Testing

This page states how to run the tests of xblick-relay and what they check.

## What you need

- Durchblick's Python environment with its `dev` group,
- Durchblick's C++ build, which contains the test camera `xblick-cpp-camera`,
- `zenohd` 1.10,
- for the CUDA cases, a GPU and a relay built with `XBLICK_RELAY_CUDA=ON`. The tests read the relay's `--help`,
  which names the decoders of the build. They skip the CUDA cases for a relay without nvJPEG, such as the wheel's.

The test camera acts as a source that offers only network transports. The relay therefore relays it on the same
machine. One test uses a Python publisher that reports another host, to act as a source with rings on another PC.

## Run

```bash
XBLICK_CPP_BUILD=$XBLICK_DIR/build XBLICK_RELAY=build/install/bin/xblick-relay \
ZENOHD=<zenohd 1.10> CUDA_VISIBLE_DEVICES=0 $XBLICK_DIR/.venv/bin/python -m pytest tests/test_relay.py
```

pytest skips all tests when `XBLICK_CPP_BUILD`, `XBLICK_RELAY` or `ZENOHD` is missing. Add `-s` to see the
measured delay.

## What the tests check

| Test | Checks |
| --- | --- |
| JPEG into the host ring and into the CUDA ring | The relay decodes as well as libjpeg-turbo decodes the same bytes. Frames have the source's header, `seq` and `source`, arrive in order and have no duplicates. |
| PNG, RVL depth and `raw` | The relayed frames equal the source's frames exactly. |
| On demand | The relay takes the source only while a reader exists. |
| A reader asks | Without `--stream`, the relay relays a stream from its first reader until `--idle-ms` after its last, and again for a later reader. |
| A reader that comes back | A reader that asks again within `--idle-ms` keeps the stream: the relay does not drop it. |
| Two streams dropped together | The readers of two streams leave together while other programs query the streams' info records. The relay drops both and keeps running, three times in a row, on the CPU and with a CUDA ring. With Durchblick 0.3.64 it dies in the first round. |
| A reader that starts first | It moves to the relay's ring when the relay starts, and back when the relay stops or is killed. It gets every frame once. |
| A source with rings on another host | The relay relays it. |
| A local source with a ring | The relay does not relay it. |
| A second relay on the same PC | It stands back. This also holds for two relays that start together. |
| Start order | A relay that starts before its source finds it later. |
| A source restart | The relay continues after the source restarts. |
| Latency report | The test prints the delay. |
