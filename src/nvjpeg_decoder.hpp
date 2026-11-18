// JPEG frames decoded on the GPU with nvJPEG into device memory (rows without padding), for Durchblick's CUDA ring
// without a trip through host memory. After xblick-azure-kinect's decoder, with the output layout chosen per
// stream (rgb8, bgr8, mono8) and the size taken from the JPEG.
//
// Two builds (CMakeLists.txt). The CMake build with XBLICK_RELAY_CUDA links nvjpeg_decoder.cpp into the program
// against a CUDA toolkit. The wheel builds it as the module libxblick-relay-nvjpeg.so against NVIDIA's pip packages;
// the program opens the module only when it decodes with nvJPEG (nvjpeg_loader.cpp) and needs no CUDA library itself.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace xblick::relay
{
    struct DecodedOnDevice
    {
        std::uint64_t ptr = 0;  // device pointer, valid until the next decode
        std::uint32_t height = 0;
        std::uint32_t width = 0;
    };

    class NvJpegDecoder
    {
    public:
        virtual ~NvJpegDecoder() = default;

        // True if nvJPEG writes this ROS encoding (rgb8, bgr8, mono8).
        static bool supports(const std::string& encoding)
        {
            return encoding == "rgb8" or encoding == "bgr8" or encoding == "mono8";
        }
        // Decodes into the decoder's device buffer as `encoding`, enqueued on stream() (not waited for).
        virtual DecodedOnDevice decode(std::span<const std::uint8_t> jpeg, const std::string& encoding) = 0;
        // The CUDA stream (a cudaStream_t, usable as a CUstream) the decoded pixels are ready on.
        virtual std::uint64_t stream() const = 0;
        virtual void synchronize() = 0;
        virtual int device() const = 0;
    };

    // An nvJPEG decoder on this GPU. Throws std::runtime_error with one line that names what is missing: the module
    // or NVIDIA's libraries (wheel), the driver, the device.
    std::unique_ptr<NvJpegDecoder> make_nvjpeg_decoder(int device);
}  // namespace xblick::relay

// The decoder's entry point (nvjpeg_decoder.cpp). In the wheel the program looks it up in the module by this name.
extern "C" xblick::relay::NvJpegDecoder* xblick_relay_open_nvjpeg(int device);
