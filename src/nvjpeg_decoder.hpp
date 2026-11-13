// JPEG frames decoded on the GPU with nvJPEG into device memory (rows without padding), for Durchblick's CUDA ring
// without a trip through host memory. After xblick-azure-kinect's decoder, with the output layout chosen per
// stream (rgb8, bgr8, mono8) and the size taken from the JPEG.
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
        explicit NvJpegDecoder(int device);
        ~NvJpegDecoder();
        NvJpegDecoder(const NvJpegDecoder&) = delete;
        NvJpegDecoder& operator=(const NvJpegDecoder&) = delete;

        // True if nvJPEG writes this ROS encoding (rgb8, bgr8, mono8).
        static bool supports(const std::string& encoding);
        // Decodes into the decoder's device buffer as `encoding`, enqueued on stream() (not waited for).
        DecodedOnDevice decode(std::span<const std::uint8_t> jpeg, const std::string& encoding);
        // The CUDA stream (a cudaStream_t, usable as a CUstream) the decoded pixels are ready on.
        std::uint64_t stream() const;
        void synchronize();
        int device() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace xblick::relay
