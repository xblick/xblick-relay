// The wheel's program: nvJPEG from the module libxblick-relay-nvjpeg.so beside libzenohc.so (xblick_relay/lib), opened
// only when the relay decodes with nvJPEG. The module finds libnvjpeg.so.12 and libcudart.so.12 through its RPATH in
// NVIDIA's pip packages nvidia-nvjpeg-cu12 and nvidia-cuda-runtime-cu12 of the same environment
// (site-packages/nvidia/{nvjpeg,cuda_runtime}/lib, CMakeLists.txt). Without them the program still decodes on the CPU.
#include <dlfcn.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#include "nvjpeg_decoder.hpp"

namespace xblick::relay
{
    namespace
    {
        using Open = NvJpegDecoder* (*)(int);

        Open load_module()
        {
            const std::filesystem::path module =
                std::filesystem::read_symlink("/proc/self/exe").parent_path().parent_path() / "lib" /
                "libxblick-relay-nvjpeg.so";
            void* handle = ::dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr)
            {
                const std::string error = ::dlerror();
                if (not std::filesystem::exists(module))
                {
                    throw std::runtime_error(module.string() + " is missing: reinstall xblick-relay from a wheel");
                }
                throw std::runtime_error("cannot load nvJPEG (" + error +
                                         "): the packages nvidia-nvjpeg-cu12 and nvidia-cuda-runtime-cu12 must be "
                                         "installed in the environment of xblick-relay (its dependencies)");
            }
            auto open = reinterpret_cast<Open>(::dlsym(handle, "xblick_relay_open_nvjpeg"));
            if (open == nullptr)
            {
                throw std::runtime_error(module.string() + " has no xblick_relay_open_nvjpeg");
            }
            return open;  // the module stays loaded: the decoders' code is in it
        }
    }  // namespace

    std::unique_ptr<NvJpegDecoder> make_nvjpeg_decoder(int device)
    {
        static const Open open = load_module();  // a failure throws again at the next call
        return std::unique_ptr<NvJpegDecoder>(open(device));
    }
}  // namespace xblick::relay
