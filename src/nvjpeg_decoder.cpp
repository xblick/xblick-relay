#include "nvjpeg_decoder.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

#include <cuda_runtime_api.h>  // not cuda_runtime.h: it needs crt/, which NVIDIA's runtime package lacks
#include <nvjpeg.h>

namespace xblick::relay
{
    namespace
    {
        void check(cudaError_t status, const char* what)
        {
            if (status != cudaSuccess)
            {
                throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
            }
        }

        void check(nvjpegStatus_t status, const char* what)
        {
            if (status != NVJPEG_STATUS_SUCCESS)
            {
                throw std::runtime_error(std::string(what) + ": nvJPEG status " + std::to_string(int(status)));
            }
        }

        std::string cuda_version(int version)  // 12080 -> "12.8"
        {
            return std::to_string(version / 1000) + "." + std::to_string(version % 1000 / 10);
        }

        // The driver and the device, checked before the first CUDA call that needs them, so that the error names
        // what is missing (cudaSetDevice says "CUDA driver version is insufficient" also when there is no driver).
        void check_device(int device)
        {
            int driver = 0;
            cudaDriverGetVersion(&driver);
            if (driver == 0)
            {
                throw std::runtime_error("no NVIDIA driver (libcuda.so.1 cannot be loaded)");
            }
            if (driver < CUDART_VERSION / 1000 * 1000)
            {
                throw std::runtime_error("the NVIDIA driver supports CUDA " + cuda_version(driver) + ", nvJPEG needs " +
                                         std::to_string(CUDART_VERSION / 1000) + ".0 or newer");
            }
            int count = 0;
            const cudaError_t status = cudaGetDeviceCount(&count);
            if (status == cudaErrorNoDevice or (status == cudaSuccess and count == 0))
            {
                const char* visible = std::getenv("CUDA_VISIBLE_DEVICES");
                throw std::runtime_error(std::string("no CUDA device is visible") +
                                         (visible ? std::string(" (CUDA_VISIBLE_DEVICES='") + visible + "')" : ""));
            }
            check(status, "cudaGetDeviceCount");
            if (device < 0 or device >= count)
            {
                throw std::runtime_error("there is no GPU " + std::to_string(device) + " (" + std::to_string(count) +
                                         " visible)");
            }
        }

        // nvJPEG's output format and bytes per pixel for a ROS encoding
        std::pair<nvjpegOutputFormat_t, int> output_of(const std::string& encoding)
        {
            if (encoding == "rgb8")
            {
                return {NVJPEG_OUTPUT_RGBI, 3};
            }
            if (encoding == "bgr8")
            {
                return {NVJPEG_OUTPUT_BGRI, 3};
            }
            if (encoding == "mono8")
            {
                return {NVJPEG_OUTPUT_Y, 1};
            }
            throw std::invalid_argument("nvJPEG decodes to rgb8, bgr8 or mono8, not " + encoding);
        }

        class Decoder final : public NvJpegDecoder
        {
        public:
            explicit Decoder(int device) : device_(device)
            {
                check_device(device);
                try
                {
                    check(cudaSetDevice(device), "cudaSetDevice");
                    check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreate");
                    check(nvjpegCreateSimple(&handle_), "nvjpegCreateSimple");
                    check(nvjpegJpegStateCreate(handle_, &state_), "nvjpegJpegStateCreate");
                }
                catch (...)
                {
                    release();
                    throw;
                }
            }

            ~Decoder() override { release(); }

            DecodedOnDevice decode(std::span<const std::uint8_t> jpeg, const std::string& encoding) override
            {
                check(cudaSetDevice(device_), "cudaSetDevice");
                const auto [format, pixel_bytes] = output_of(encoding);
                int components = 0;
                nvjpegChromaSubsampling_t subsampling;
                int widths[NVJPEG_MAX_COMPONENT];
                int heights[NVJPEG_MAX_COMPONENT];
                check(nvjpegGetImageInfo(handle_, jpeg.data(), jpeg.size(), &components, &subsampling, widths, heights),
                      "nvjpegGetImageInfo");
                if (widths[0] <= 0 or heights[0] <= 0)
                {
                    throw std::runtime_error("a JPEG of " + std::to_string(widths[0]) + "x" +
                                             std::to_string(heights[0]));
                }
                const auto width = static_cast<std::uint32_t>(widths[0]);
                const auto height = static_cast<std::uint32_t>(heights[0]);
                const std::size_t row = std::size_t(width) * pixel_bytes;
                if (capacity_ < row * height)
                {
                    if (pixels_ != nullptr)
                    {
                        check(cudaStreamSynchronize(stream_), "cudaStreamSynchronize");
                        check(cudaFree(pixels_), "cudaFree");
                        pixels_ = nullptr;
                        capacity_ = 0;
                    }
                    check(cudaMalloc(&pixels_, row * height), "cudaMalloc");
                    capacity_ = row * height;
                }
                nvjpegImage_t out{};
                out.channel[0] = static_cast<unsigned char*>(pixels_);
                out.pitch[0] = row;
                check(nvjpegDecode(handle_, state_, jpeg.data(), jpeg.size(), format, &out, stream_), "nvjpegDecode");
                return {reinterpret_cast<std::uint64_t>(pixels_), height, width};
            }

            std::uint64_t stream() const override { return reinterpret_cast<std::uint64_t>(stream_); }

            void synchronize() override { check(cudaStreamSynchronize(stream_), "cudaStreamSynchronize"); }

            int device() const override { return device_; }

        private:
            void release()
            {
                cudaSetDevice(device_);
                if (stream_ != nullptr)
                {
                    cudaStreamSynchronize(stream_);
                }
                if (pixels_ != nullptr)
                {
                    cudaFree(pixels_);
                }
                if (state_ != nullptr)
                {
                    nvjpegJpegStateDestroy(state_);
                }
                if (handle_ != nullptr)
                {
                    nvjpegDestroy(handle_);
                }
                if (stream_ != nullptr)
                {
                    cudaStreamDestroy(stream_);
                }
            }

            int device_ = 0;
            nvjpegHandle_t handle_ = nullptr;
            nvjpegJpegState_t state_ = nullptr;
            cudaStream_t stream_ = nullptr;
            void* pixels_ = nullptr;
            std::size_t capacity_ = 0;
        };
    }  // namespace

#ifndef XBLICK_RELAY_NVJPEG_MODULE
    // the CMake build: the decoder is part of the program
    std::unique_ptr<NvJpegDecoder> make_nvjpeg_decoder(int device)
    {
        return std::unique_ptr<NvJpegDecoder>(xblick_relay_open_nvjpeg(device));
    }
#endif
}  // namespace xblick::relay

extern "C" __attribute__((visibility("default"))) xblick::relay::NvJpegDecoder* xblick_relay_open_nvjpeg(int device)
{
    return new xblick::relay::Decoder(device);
}
