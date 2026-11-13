"""xblick-relay as a Python wheel (``pip install xblick[relay]``): the program and libzenohc.so, which a pip
installation does not have. ``xblick relay [options]`` starts it; ``binary()`` is where it is."""

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
