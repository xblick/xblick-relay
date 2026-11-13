# Design

This page explains how the relay learns which streams to relay, how it finds the source of a stream, how it
republishes the frames and which rules keep two publishers of one stream from colliding.

## Terms

The *source* is the original publisher of a stream on another PC. Every publisher answers queries on `<stream>/info`
with an *info record*, which lists the transports that it offers. A relay's record has the field `relayed_from` set
to the source's host. A source's record has `relayed_from` null.

## Which streams the relay relays

A subscriber with the transport `auto` holds a Zenoh liveliness token `<stream>/want/<host>/<id>` while the stream
has no source with a ring on the subscriber's host. `<host>` is the id of the subscriber's host. Durchblick 0.3.60
added this token.

The relay watches the tokens of its own host. It starts to relay a stream when the first token for it appears, and
it stops `--idle-ms` after the last token disappeared. A token also disappears when its reader crashes. A relay that
starts after the readers sees their tokens at once.

The relay also relays the streams named with `--stream`, from its start until it stops.

## Finding the source

The relay queries `<stream>/info` every 5 s. While it has no source, it queries every 1 s. The source is the record
with `relayed_from` null.

The relay takes the source's `compressed` form. If the source offers none, the relay takes `raw`. With
`--prefer-raw` it takes `raw` whenever the source offers it.

The relay keeps a source for as long as the source exists. If several sources exist, the relay takes the one on the
lowest host id and logs this once. It drops a source only after three rounds of queries without it, or when it must
stand back for another relay. A failed query changes nothing.

A source on the relay's own PC that offers only network transports is relayed too. The tests use this to run on one
machine.

## Republishing

The relay publishes under the stream's own name and offers only the rings of its PC. These are `shm`, and `cuda` if
the relay has a GPU. It never offers `raw`, `compressed` or `camera_info`, because readers receive those once from
the source.

Every relayed frame has the source's header with the capture stamp and the `frame_id`. It also has the source's
attachments `seq` and `source`. Pairing by stamp and pose lookups therefore work on relayed frames exactly as on
the source's frames.

The relay uses two features of Durchblick's C++ publisher for this. `PublisherOptions` has the fields `transports`
and `relayed_from`, and `CameraPublisher::publish(image, Origin)` passes the source's header and attachments on.

## Decoding

| Input | Decoder | Result |
| --- | --- | --- |
| JPEG of `rgb8`, `bgr8` or `mono8`, with a GPU | nvJPEG decodes straight into GPU memory. | The relay copies the frame into the CUDA ring. |
| Other codecs: RVL for depth, PNG | Durchblick's CPU decoder | The relay publishes the decoded image. |
| Any codec with `--cpu` | Durchblick's CPU decoder | The relay writes the decoded image into the host ring. |
| `raw` frames | none | The relay copies the frame as it is. |

With a GPU, the host ring receives a frame through one download from the GPU. The relay downloads only while a
reader of the host ring exists.

## On demand

A stream that no reader asks for and that `--stream` does not name costs nothing: the relay has no publisher, no
thread and no query for it.

For a stream that it relays, the relay subscribes to the source only while a reader on its PC takes one of its
rings. It drops the subscription `--idle-ms` after the last reader left. This matters for the streams named with
`--stream`, which the relay offers without a request.

## Several publishers of one stream

The key of a ring contains the id of its host (`<base>/image_raw/shm/<host>`, Durchblick 0.3.59). The rings of a
source, of a relay and of relays on other PCs therefore never meet, and three rules are enough.

1. A relay never relays a record that has `relayed_from` set.
2. A relay does not relay a source that has a ring on the relay's own PC. The readers on that PC take that ring.
3. On one PC, only one relay publishes a stream. A lock file (`/dev/shm/xblick-relay.<stream>.lock`) decides.
   A second relay waits for the stream and logs the reason.

A source with rings on its own PC is relayed on every other PC. Relays on several PCs relay the same stream side by
side, each for the readers of its PC.

`xblick list` shows the source of a relayed stream with the note "relayed on <host>".

## Status and limits

Version 0.4.0 (2026-11-13). We tested it on one machine with `tests/test_relay.py`. We have not run it between two
PCs, and we have not run relays on two PCs at the same time.

Version notes:

- 0.1.0 (2026-11-09) relayed the streams named with `--stream` and required the relay to start before its readers.
- Durchblick 0.3.49 added the publisher options that a relay needs.
- Durchblick 0.3.50 made info queries return every publisher of a stream.
- `blick list` marks a relayed stream from Durchblick 0.3.51 on.
- Durchblick 0.3.59 put the host into the keys of the rings and lets readers change their transport while they run.
- Durchblick 0.3.60 added the readers' tokens.
- Durchblick 0.3.62 added RVL for depth. The relay requires this version.
- 0.2.1 crashed with `std::length_error` (`basic_string::_M_create`) when it dropped a stream while another program
  asked for the stream's info record. The cause is in Durchblick (seen with 0.3.64): the destructor of `CameraPublisher`
  freed the data of the record before it undeclared the info queryable, and a query in between read freed strings.
  With a CUDA ring the gap lasts up to 200 ms. Naming the streams with `--stream` avoided the drop. 0.2.2 adds the
  test that shows it and requires Durchblick 0.3.68, which has the fix.
- 0.3.0 builds against Durchblick 0.4, the 0.3 line with its Python package renamed to `blick`. The relay itself
  is unchanged.
- 0.3.1 adds the Python wheel `blick-relay`. Durchblick 0.4.2 starts it with `blick relay`. The program's
  `--help` names the decoders of its build. The wheel for PyPI is built in manylinux_2_28 with zenoh-c from source
  (`tools/build_wheel.sh`).
- 0.4.0 (2026-11-13) builds against Durchblick 0.5, in which every name in code, on the wire and on disk is `xblick`.
  The relay takes the same names. It works only with programs of Durchblick 0.5. The relay's behaviour is unchanged.

  | What | 0.3.1 | 0.4.0 |
  | --- | --- | --- |
  | Repository | `github.com/xblick/durchblick-relay` | `github.com/xblick/xblick-relay` |
  | Program | `durchblick-relay` | `xblick-relay` |
  | CMake project | `durchblick_relay` | `xblick_relay` |
  | CMake option and compile definition | `DURCHBLICK_RELAY_CUDA` | `XBLICK_RELAY_CUDA` |
  | Core package | `find_package(durchblick 0.4)`, `durchblick::durchblick`, `<durchblick/...>` | `find_package(xblick 0.5)`, `xblick::xblick`, `<xblick/...>` |
  | C++ namespace | `durchblick::relay` | `xblick::relay` |
  | Parameters node | `durchblick/relay_<host>` | `xblick/relay_<host>` |
  | Lock file | `/dev/shm/durchblick-relay.<stream>.lock` | `/dev/shm/xblick-relay.<stream>.lock` |
  | Python distribution | `blick-relay` | `xblick-relay` |
  | Python module | `blick_relay` | `xblick_relay` |
  | Started by | `blick relay` (Durchblick 0.4.2) | `xblick relay` (Durchblick 0.5) |
  | Test environment | `DURCHBLICK_CPP_BUILD`, `DURCHBLICK_RELAY` | `XBLICK_CPP_BUILD`, `XBLICK_RELAY` |

Open work:

- nvJPEG in the wheel `xblick-relay`. The wheel decodes on the CPU only.
- Rectification per stream on the GPU (`<name>_rect`) for a source that is too slow for it. A Jetson Nano rectifies
  at 1.7 frames per second.
- Point clouds (`PointCloud2` over `raw`).
