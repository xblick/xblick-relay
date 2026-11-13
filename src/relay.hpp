// One stream relayed: a remote publisher's image stream (compressed, else raw) taken once over Zenoh, decoded once
// (nvJPEG straight into the CUDA ring, or on the CPU) and republished under the same name, offering only this host's
// rings (shm, and cuda with a GPU), with the source's header, seq and source (Durchblick >= 0.3.49: PublisherOptions
// transports/relayed_from, CameraPublisher::publish(image, Origin)).
//
// On demand: the source is subscribed only while a reader on this host takes the relay's rings, and dropped again
// some seconds after the last one left (no network traffic for a stream nobody here reads).
//
// A source with rings on another host is relayed as any other; a source with a ring on this host is not (its readers
// here take that ring). Relays on several hosts relay one stream side by side, each for the readers of its host.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <xblick/camera.hpp>

namespace xblick::relay
{
    struct RelayOptions
    {
        std::string base;                 // the stream, e.g. robot/ecam24/left
        std::optional<int> cuda_device;   // nvJPEG and a CUDA ring on this GPU; empty: CPU decoding, shm only
        std::uint32_t slots = 32;         // ring slots (PublisherOptions::slots)
        std::chrono::milliseconds idle{2000};   // the source is dropped this long after the last local reader left
        std::chrono::milliseconds rediscover{5000};  // the source's info record is read again this often
        bool prefer_raw = false;          // take the source's raw form even if it offers compressed
    };

    // Counts since the start; RelayStream::status() reports them as a DiagnosticStatus once a second.
    struct RelayStats
    {
        std::uint64_t received = 0;     // samples from the source
        std::uint64_t overwritten = 0;  // replaced by a newer one before they were relayed (the relay too slow)
        std::uint64_t relayed = 0;      // frames put into the rings
        std::uint64_t lost = 0;         // gaps in the source's seq (lost on the network or dropped by the source)
        std::uint64_t errors = 0;       // samples that could not be decoded or published
        double decode_ms_sum = 0.0, decode_ms_max = 0.0;
        double age_ms_sum = 0.0, age_ms_max = 0.0;  // capture stamp (the source's clock) to the ring (this PC's clock)
        double transit_ms_sum = 0.0;    // source's publish_ns to arrival here (clocks of two PCs)
        std::uint64_t transit_count = 0;
    };

    class RelayStream
    {
    public:
        using Log = std::function<void(const std::string&)>;

        RelayStream(zenoh::Session& session, RelayOptions options, Log log);
        ~RelayStream();
        RelayStream(const RelayStream&) = delete;
        RelayStream& operator=(const RelayStream&) = delete;

        const std::string& base() const;
        RelayStats stats() const;
        // A status for <camera>/diagnostics (name relay_<stream>): rate and times since the last call.
        DiagnosticStatus status();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace xblick::relay
