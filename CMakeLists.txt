# xblick-relay: image streams of other PCs into this PC's shared-memory and CUDA rings (README.md).
#
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<xblick install> [-DXBLICK_RELAY_CUDA=ON
#         -DCUDAToolkit_ROOT=<CUDA toolkit with nvJPEG>]
#
# The Python wheel xblick-relay (pyproject.toml, scikit-build-core; pip install xblick-relay) builds the same program
# with nvJPEG from NVIDIA's pip packages and installs it with libzenohc.so into the package xblick_relay (below).
cmake_minimum_required(VERSION 3.22)
project(xblick_relay VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

option(XBLICK_RELAY_CUDA "decode JPEG with nvJPEG straight into the CUDA ring (CUDA toolkit)" OFF)

# Durchblick 0.5: every name in code, on the wire and on disk is xblick (CMake package xblick). Its version file
# accepts the same minor version only, so an older core is refused. The relay needs: PublisherOptions
# transports/relayed_from, publish(image, Origin), stream_infos (0.3.49); info queries that see every publisher of a
# stream (0.3.50); ring keys with the host (0.3.59); the readers' want tokens (0.3.60); RVL depth (0.3.62); publishers
# that undeclare their info queryable first, without which the relay could die when it dropped a stream (0.3.68,
# docs/design.md)
find_package(xblick 0.5 REQUIRED)

add_executable(xblick-relay src/main.cpp src/relay.cpp)
target_link_libraries(xblick-relay PRIVATE xblick::xblick)
target_compile_options(xblick-relay PRIVATE -Wall -Wextra)

if(SKBUILD)
  # The wheel: xblick_relay/bin/xblick-relay, xblick_relay/lib/libzenohc.so and the module
  # xblick_relay/lib/libxblick-relay-nvjpeg.so. The program finds libzenohc.so through $ORIGIN/../lib only: no path of
  # the build machine in its RPATH. It needs no CUDA library: it opens the module when it decodes with nvJPEG
  # (src/nvjpeg_loader.cpp). The module needs libnvjpeg.so.12 and libcudart.so.12, which the wheel does not contain:
  # they come from NVIDIA's pip packages nvidia-nvjpeg-cu12 and nvidia-cuda-runtime-cu12, dependencies of the wheel,
  # installed in the same site-packages (nvidia/nvjpeg/lib, nvidia/cuda_runtime/lib). The module's RPATH names these
  # two directories relative to itself, as DT_RPATH, which goes before LD_LIBRARY_PATH: a CUDA toolkit on
  # LD_LIBRARY_PATH does not replace them. The build takes headers and libraries from the same packages in the build
  # environment (pyproject.toml, build-system.requires), and crt/host_defines.h, which cuda_runtime_api.h includes,
  # from nvidia-cuda-nvcc-cu12 (build only). libjpeg, libpng, libstdc++ and libc come from the system.
  if(XBLICK_RELAY_CUDA)
    message(FATAL_ERROR "the wheel takes nvJPEG from NVIDIA's pip packages (pyproject.toml), not from a CUDA "
                        "toolkit: build it without XBLICK_RELAY_CUDA")
  endif()
  find_package(Python 3.10 REQUIRED COMPONENTS Interpreter)
  execute_process(
    COMMAND "${Python_EXECUTABLE}" -c
            "import nvidia.nvjpeg, nvidia.cuda_runtime, nvidia.cuda_nvcc; print(*(list(m.__path__)[0] for m in (nvidia.nvjpeg, nvidia.cuda_runtime, nvidia.cuda_nvcc)), sep=chr(10))"
    OUTPUT_VARIABLE nvidia_dirs OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE nvidia_found ERROR_VARIABLE nvidia_error)
  if(NOT nvidia_found EQUAL 0)
    message(FATAL_ERROR "the packages nvidia-nvjpeg-cu12, nvidia-cuda-runtime-cu12 and nvidia-cuda-nvcc-cu12 are not "
                        "in the build environment (${Python_EXECUTABLE}, pyproject.toml build-system.requires): ${nvidia_error}")
  endif()
  string(REPLACE "\n" ";" nvidia_dirs "${nvidia_dirs}")
  list(GET nvidia_dirs 0 NVJPEG_DIR)
  list(GET nvidia_dirs 1 CUDART_DIR)
  list(GET nvidia_dirs 2 NVCC_DIR)
  foreach(file "${NVJPEG_DIR}/include/nvjpeg.h" "${NVJPEG_DIR}/lib/libnvjpeg.so.12"
               "${CUDART_DIR}/include/cuda_runtime_api.h" "${CUDART_DIR}/lib/libcudart.so.12"
               "${NVCC_DIR}/include/crt/host_defines.h")
    if(NOT EXISTS "${file}")
      message(FATAL_ERROR "${file} is missing: NVIDIA's pip packages have another layout than this build expects")
    endif()
  endforeach()

  add_library(xblick-relay-nvjpeg MODULE src/nvjpeg_decoder.cpp)
  target_compile_definitions(xblick-relay-nvjpeg PRIVATE XBLICK_RELAY_NVJPEG_MODULE)
  target_compile_options(xblick-relay-nvjpeg PRIVATE -Wall -Wextra)
  target_include_directories(xblick-relay-nvjpeg SYSTEM PRIVATE "${NVJPEG_DIR}/include" "${CUDART_DIR}/include"
                                                                   "${NVCC_DIR}/include")
  target_link_libraries(xblick-relay-nvjpeg PRIVATE "${NVJPEG_DIR}/lib/libnvjpeg.so.12"
                                                    "${CUDART_DIR}/lib/libcudart.so.12")
  target_link_options(xblick-relay-nvjpeg PRIVATE -Wl,--no-undefined -Wl,--disable-new-dtags)
  set_target_properties(xblick-relay-nvjpeg PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    INSTALL_RPATH "$ORIGIN/../../nvidia/nvjpeg/lib:$ORIGIN/../../nvidia/cuda_runtime/lib"
    INSTALL_RPATH_USE_LINK_PATH OFF)

  target_sources(xblick-relay PRIVATE src/nvjpeg_loader.cpp)
  target_compile_definitions(xblick-relay PRIVATE XBLICK_RELAY_CUDA XBLICK_RELAY_NVJPEG_MODULE)
  target_link_libraries(xblick-relay PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(xblick-relay PROPERTIES INSTALL_RPATH_USE_LINK_PATH OFF)
  message(STATUS "xblick-relay: nvJPEG from ${NVJPEG_DIR}, CUDA runtime from ${CUDART_DIR} (module)")
  install(TARGETS xblick-relay RUNTIME DESTINATION xblick_relay/bin)
  install(TARGETS xblick-relay-nvjpeg LIBRARY DESTINATION xblick_relay/lib)
  install(FILES $<TARGET_FILE:zenohc::lib> DESTINATION xblick_relay/lib)
else()
  if(XBLICK_RELAY_CUDA)
    find_package(CUDAToolkit 12 REQUIRED)
    target_sources(xblick-relay PRIVATE src/nvjpeg_decoder.cpp)
    target_compile_definitions(xblick-relay PRIVATE XBLICK_RELAY_CUDA)
    target_link_libraries(xblick-relay PRIVATE CUDA::cudart CUDA::nvjpeg)
  endif()
  message(STATUS "xblick-relay: nvJPEG ${XBLICK_RELAY_CUDA}")
  install(TARGETS xblick-relay RUNTIME DESTINATION bin)
endif()

enable_testing()
# tests/test_relay.py: the relay between a source of the C++ test camera (xblick-cpp-camera of Durchblick's build,
# XBLICK_CPP_BUILD) and Python readers; run with Durchblick's Python environment (README.md, Tests).
# tests/test_device.py: the choice of the decoder at the start (README.md, Tests).
