# durchblick-relay: image streams of other PCs into this PC's shared-memory and CUDA rings (README.md).
#
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<durchblick install> [-DDURCHBLICK_RELAY_CUDA=ON
#         -DCUDAToolkit_ROOT=<CUDA toolkit with nvJPEG>]
#
# The Python wheel blick-relay (pyproject.toml, scikit-build-core; pip install blick[relay]) builds the same program
# without nvJPEG and installs it with libzenohc.so into the package blick_relay (below).
cmake_minimum_required(VERSION 3.22)
project(durchblick_relay VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

option(DURCHBLICK_RELAY_CUDA "decode JPEG with nvJPEG straight into the CUDA ring (CUDA toolkit)" OFF)

# Durchblick 0.4: the 0.3 line up to 0.3.68 with its Python package renamed to blick. Its version file accepts the
# same minor version only, so a 0.3.x core is refused. The relay needs: PublisherOptions transports/relayed_from,
# publish(image, Origin), stream_infos (0.3.49); info queries that see every publisher of a stream (0.3.50); ring keys
# with the host (0.3.59); the readers' want tokens (0.3.60); RVL depth (0.3.62); publishers that undeclare their info
# queryable first, without which the relay could die when it dropped a stream (0.3.68, docs/design.md)
find_package(durchblick 0.4 REQUIRED)

add_executable(durchblick-relay src/main.cpp src/relay.cpp)
target_link_libraries(durchblick-relay PRIVATE durchblick::durchblick)
target_compile_options(durchblick-relay PRIVATE -Wall -Wextra)
if(DURCHBLICK_RELAY_CUDA)
  find_package(CUDAToolkit 12 REQUIRED)
  target_sources(durchblick-relay PRIVATE src/nvjpeg_decoder.cpp)
  target_compile_definitions(durchblick-relay PRIVATE DURCHBLICK_RELAY_CUDA)
  target_link_libraries(durchblick-relay PRIVATE CUDA::cudart CUDA::nvjpeg)
endif()
message(STATUS "durchblick-relay: nvJPEG ${DURCHBLICK_RELAY_CUDA}")

if(SKBUILD)
  # The wheel: blick_relay/bin/durchblick-relay and blick_relay/lib/libzenohc.so, which a pip installation does not
  # have. The program finds the library through $ORIGIN/../lib only: no path of the build machine in its RPATH.
  # libjpeg, libpng, libstdc++ and libc come from the system.
  if(DURCHBLICK_RELAY_CUDA)
    message(FATAL_ERROR "the wheel has no nvJPEG: build it without DURCHBLICK_RELAY_CUDA (README.md, Python package)")
  endif()
  set_target_properties(durchblick-relay PROPERTIES INSTALL_RPATH_USE_LINK_PATH OFF)
  install(TARGETS durchblick-relay RUNTIME DESTINATION blick_relay/bin)
  install(FILES $<TARGET_FILE:zenohc::lib> DESTINATION blick_relay/lib)
else()
  install(TARGETS durchblick-relay RUNTIME DESTINATION bin)
endif()

enable_testing()
# tests/test_relay.py: the relay between a source of the C++ test camera (durchblick-cpp-camera of Durchblick's build,
# DURCHBLICK_CPP_BUILD) and Python readers; run with Durchblick's Python environment (README.md, Tests)
