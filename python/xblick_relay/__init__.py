"""xblick-relay as a Python wheel (``pip install xblick-relay``): the program, libzenohc.so and the nvJPEG module,
which a pip installation does not have; nvJPEG and the CUDA runtime come from NVIDIA's packages, dependencies of the
wheel. The command ``xblick-relay [options]`` (``main``) and Durchblick's ``xblick relay [options]`` start it;
``binary()`` is where it is."""

import os
import sys
from importlib.metadata import version
from pathlib import Path

__version__ = version("xblick-relay")


def binary() -> Path:
    """The relay program of this wheel (xblick_relay/bin/xblick-relay)."""
    path = Path(__file__).parent / "bin" / "xblick-relay"
    if not path.is_file():
        raise FileNotFoundError(f"{path} is missing: xblick-relay is installed without its program (an editable or "
                                f"broken install); reinstall it from a wheel")
    return path


def main() -> None:
    """The command xblick-relay: replaces this process with the program, with the same arguments."""
    program = str(binary())
    os.execv(program, [program, *sys.argv[1:]])
