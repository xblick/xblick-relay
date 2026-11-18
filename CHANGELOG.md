# Changelog

## 0.4.1

The wheel decodes with nvJPEG on the GPU by default and needs only the NVIDIA driver for it.

### Changed

- Behaviour change: the wheel decodes JPEG with nvJPEG on GPU 0 into the CUDA ring by default, as the CMake build
  with `XBLICK_RELAY_CUDA` does. Up to 0.4.0 the wheel decoded on the CPU only. Pass `--cpu` for CPU decoding.
- Behaviour change: without `--cpu`, a relay with nvJPEG (the wheel, or the CMake build with `XBLICK_RELAY_CUDA`)
  stops at its start with one line if it cannot decode on the GPU: no NVIDIA driver, no such GPU, or NVIDIA's
  packages missing. Up to 0.4.0 the CMake build decoded on the CPU when the PC had no GPU.
- The wheel depends on NVIDIA's packages `nvidia-nvjpeg-cu12` (12.3.5 or newer) and `nvidia-cuda-runtime-cu12`
  (12.0 or newer), about 9 MB. They are not part of the wheel.

### Added

- The command `xblick-relay` in the wheel, so that `uv tool install xblick-relay` installs a relay.
