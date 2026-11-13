#!/usr/bin/env bash
# The wheel blick-relay for PyPI, tagged manylinux_2_28_x86_64: what the CI's wheel job and the publish workflow build.
# Runs in the image quay.io/pypa/manylinux_2_28_x86_64 (AlmaLinux 8, glibc 2.28):
#
#   dnf install -y --setopt=tsflags= libjpeg-turbo-devel libpng-devel   # as root; with docs: README.ijg
#   tools/build_wheel.sh <durchblick checkout> <out dir> <work dir>
#
# Needs on PATH cargo and rustc of the version zenoh-c's rust-toolchain.toml names (1.97.1 for zenoh-c 1.10.1), and
# network access: the zenoh-c and zenoh-cpp sources (GitHub), Rust crates, the pip packages nvidia-cuda-runtime (cuda.h
# for Durchblick's C++ library) and the build requirements. <work dir> must not exist or be empty; <out dir> gets the
# repaired wheel only.
#
# 1. zenoh-c from source: its prebuilt release needs glibc 2.34 (GLIBC_2.34 symbols), more than manylinux_2_28 allows.
# 2. zenoh-cpp's headers. 3. Durchblick's C++ library (camera API, static) from <durchblick checkout>.
# 4. The wheel (pyproject.toml). 5. auditwheel repair: libjpeg and libpng of AlmaLinux 8 go into blick_relay.libs/
#    (their licences: licenses/), libzenohc.so stays in blick_relay/lib. 6. Checks: the platform tag, every library
#    resolves without LD_LIBRARY_PATH, --help names a CPU-only build.
set -euo pipefail

if [ $# -ne 3 ]; then
  echo "usage: $0 <durchblick checkout> <out dir> <work dir>" >&2
  exit 2
fi
CORE="$(cd "$1" && pwd)"
OUT="$2"
WORK="$3"
RELAY="$(cd "$(dirname "$0")/.." && pwd)"
ZENOH_VERSION="${ZENOH_VERSION:-1.10.1}"
PLAT=manylinux_2_28_x86_64
PYTHON=/opt/python/cp312-cp312/bin/python

if [ "${AUDITWHEEL_PLAT:-}" != "$PLAT" ]; then
  echo "run this in quay.io/pypa/${PLAT} (AUDITWHEEL_PLAT is '${AUDITWHEEL_PLAT:-}')" >&2
  exit 1
fi
for package in libjpeg-turbo-devel libpng-devel; do
  if ! rpm -q "$package" > /dev/null; then
    echo "$package is missing: dnf install -y --setopt=tsflags= libjpeg-turbo-devel libpng-devel (as root; tsflags: with README.ijg)" >&2
    exit 1
  fi
done
command -v cargo > /dev/null || { echo "cargo is missing (Rust ${ZENOH_VERSION}: see zenoh-c's rust-toolchain.toml)" >&2; exit 1; }
if [ -e "$WORK" ] && [ -n "$(ls -A "$WORK")" ]; then
  echo "work dir $WORK is not empty" >&2
  exit 1
fi
mkdir -p "$WORK" "$OUT"
WORK="$(cd "$WORK" && pwd)"
ZENOH="$WORK/zenoh"
JOBS="$(nproc)"

echo "== zenoh-c ${ZENOH_VERSION} from source"
curl -sSfL -o "$WORK/zenoh-c.tar.gz" \
  "https://github.com/eclipse-zenoh/zenoh-c/archive/refs/tags/${ZENOH_VERSION}.tar.gz"
tar xzf "$WORK/zenoh-c.tar.gz" -C "$WORK"
ZENOHC_SRC="$WORK/zenoh-c-${ZENOH_VERSION}"
toolchain="$(sed -n 's/^channel = "\(.*\)"$/\1/p' "$ZENOHC_SRC/rust-toolchain.toml")"
rustc_version="$(rustc --version | cut -d' ' -f2)"
if [ "$rustc_version" != "$toolchain" ]; then
  echo "rustc is $rustc_version, zenoh-c ${ZENOH_VERSION} names $toolchain (rust-toolchain.toml)" >&2
  exit 1
fi
# the features of zenoh-c's prebuilt release (its zenoh_configure.h): shared memory and the unstable API
cmake -S "$ZENOHC_SRC" -B "$WORK/zenoh-c-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ZENOH" \
  -DZENOHC_BUILD_WITH_SHARED_MEMORY=TRUE -DZENOHC_BUILD_WITH_UNSTABLE_API=TRUE > /dev/null
cmake --build "$WORK/zenoh-c-build" --config Release -j "$JOBS"
cmake --install "$WORK/zenoh-c-build" > /dev/null
# the Rust crates in libzenohc.so and their licence texts: the wheel carries licenses/zenoh-c-crates.txt, which must
# be what this build links
"$PYTHON" "$RELAY/tools/zenoh_c_crates.py" "$WORK/zenoh-c-build/release" > "$WORK/zenoh-c-crates.txt"
if ! cmp -s "$WORK/zenoh-c-crates.txt" "$RELAY/licenses/zenoh-c-crates.txt"; then
  diff "$RELAY/licenses/zenoh-c-crates.txt" "$WORK/zenoh-c-crates.txt" | head -20 >&2 || true
  echo "licenses/zenoh-c-crates.txt is not the list of this build: regenerate it with tools/zenoh_c_crates.py" >&2
  exit 1
fi
check_licence() {  # <file of this build> <file in licenses/>: the wheel carries the second, which must be the first
  if ! cmp -s "$1" "$RELAY/licenses/$2"; then
    echo "licenses/$2 differs from $1: copy it again" >&2
    exit 1
  fi
}
check_licence "$ZENOHC_SRC/LICENSE" zenoh-c-LICENSE
check_licence "$ZENOHC_SRC/NOTICE.md" zenoh-c-NOTICE.md
# libjpeg-turbo and libpng of this image go into the wheel (auditwheel): their licences (NOTICE names the versions)
echo "   libjpeg-turbo $(rpm -q --qf "%{VERSION}" libjpeg-turbo), libpng $(rpm -q --qf "%{VERSION}" libpng)"
check_licence /usr/share/licenses/libjpeg-turbo/LICENSE.md libjpeg-turbo-LICENSE.md
check_licence /usr/share/doc/libjpeg-turbo/README.ijg libjpeg-turbo-README.ijg
check_licence /usr/share/licenses/libpng/LICENSE libpng-LICENSE

echo "== zenoh-cpp ${ZENOH_VERSION} (headers)"
curl -sSfL -o "$WORK/zenoh-cpp.tar.gz" "https://github.com/eclipse-zenoh/zenoh-cpp/archive/refs/tags/${ZENOH_VERSION}.tar.gz"
tar xzf "$WORK/zenoh-cpp.tar.gz" -C "$WORK"
cmake -S "$WORK/zenoh-cpp-${ZENOH_VERSION}" -B "$WORK/zenoh-cpp-build" -DCMAKE_INSTALL_PREFIX="$ZENOH" \
  -DCMAKE_PREFIX_PATH="$ZENOH" -DZENOHCXX_ZENOHPICO=OFF > /dev/null
cmake --install "$WORK/zenoh-cpp-build" > /dev/null

echo "== Durchblick's C++ library"
# cuda.h from the pip package nvidia-cuda-runtime, as Durchblick's own build takes it (libcuda.so.1 is opened at run time)
"$PYTHON" -m venv "$WORK/cuda"
"$WORK/cuda/bin/pip" install --quiet "nvidia-cuda-runtime>=13,<14"
CUDA_INCLUDE="$("$WORK/cuda/bin/python" -c "import nvidia, os; print(os.path.join(list(nvidia.__path__)[0], 'cu13', 'include'))")"
cmake -S "$CORE" -B "$WORK/core-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$ZENOH" \
  -DDURCHBLICK_CAMERA=ON -DDURCHBLICK_CUDA_INCLUDE_DIR="$CUDA_INCLUDE" -DBUILD_TESTING=OFF > /dev/null
cmake --build "$WORK/core-build" -j "$JOBS"
cmake --install "$WORK/core-build" --prefix "$WORK/core-install" > /dev/null

echo "== the wheel"
CMAKE_PREFIX_PATH="$WORK/core-install:$ZENOH" "$PYTHON" -m pip wheel --no-deps --no-cache-dir "$RELAY" \
  -w "$WORK/wheelhouse" -C build-dir="$WORK/relay-build"
wheel="$(ls "$WORK"/wheelhouse/blick_relay-*.whl)"
auditwheel repair --plat "$PLAT" -w "$WORK/repaired" "$wheel"
repaired="$(ls "$WORK"/repaired/blick_relay-*-py3-none-${PLAT}.whl)"
auditwheel show "$repaired"

echo "== checks"
mkdir "$WORK/check"
(cd "$WORK/check" && "$PYTHON" -m zipfile -e "$repaired" .)
program="$WORK/check/blick_relay/bin/durchblick-relay"
chmod +x "$program"
env -u LD_LIBRARY_PATH ldd "$program"
if env -u LD_LIBRARY_PATH ldd "$program" | grep -q "not found"; then
  echo "$program: a library does not resolve" >&2
  exit 1
fi
env -u LD_LIBRARY_PATH ldd "$program" | grep -q "blick_relay/bin/../lib/libzenohc.so"
if env -u LD_LIBRARY_PATH ldd "$program" | grep -E "=> /(usr/)?lib.*/lib(jpeg|png)"; then
  echo "$program takes libjpeg or libpng from the system" >&2
  exit 1
fi
env -u LD_LIBRARY_PATH "$program" --help | grep -q "decoders of this build: CPU only"
cp "$repaired" "$OUT/"
echo "== $OUT/$(basename "$repaired")"
