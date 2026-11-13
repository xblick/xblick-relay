"""durchblick-relay as a Python wheel (``pip install blick[relay]``): the program and libzenohc.so, which a pip
installation does not have. ``blick relay [options]`` starts it; ``binary()`` is where it is."""

from importlib.metadata import version
from pathlib import Path

__version__ = version("blick-relay")


def binary() -> Path:
    """The relay program of this wheel (blick_relay/bin/durchblick-relay)."""
    path = Path(__file__).parent / "bin" / "durchblick-relay"
    if not path.is_file():
        raise FileNotFoundError(f"{path} is missing: blick-relay is installed without its program (an editable or "
                                f"broken install); reinstall it from a wheel")
    return path
