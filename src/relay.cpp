#include "relay.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <durchblick/image.hpp>
#include <durchblick/protocol.hpp>
#include <durchblick/ring.hpp>

#ifdef DURCHBLICK_RELAY_CUDA
#include "nvjpeg_decoder.hpp"
#endif

namespace durchblick::relay
{
    namespace p = durchblick::protocol;

    namespace
    {
        constexpr int kMissesBeforeDrop = 3;  // discovery rounds without the source before its relay is torn down

        std::int64_t now_ns()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        std::string last_token(const std::string& base)
        {
            const auto slash = base.rfind('/');
            return slash == std::string::npos ? base : base.substr(slash + 1);
        }

        std::string fixed(double value, int digits = 1)
        {
            char text[64];
            std::snprintf(text, sizeof text, "%.*f", digits, value);
            return text;
        }

        // What a JPEG of a stream of this encoding decodes to (Durchblick's decompress and the relay's nvJPEG agree):
        // JPEG has no alpha. Other codecs (PNG) keep the encoding.
        std::string jpeg_decoded(const std::string& encoding)
        {
            if (encoding == "rgb8" or encoding == "rgba8")
            {
                return "rgb8";
            }
            if (encoding == "bgr8" or encoding == "bgra8")
            {
                return "bgr8";
            }
            if (encoding == "mono8" or encoding == "8UC1")
            {
                return "mono8";
            }
            return encoding;
        }

        // The publisher a relay takes its frames from: one record of the stream, not a relay's.
        struct Source
        {
            std::string host;
            std::string transport;  // "compressed" or "raw"
            std::string encoding;
            std::string frame_id;
            std::string source;

            bool operator==(const Source&) const = default;
        };

        // A sample as received from the source, kept until the worker takes it (only the newest).
        struct Sample
        {
            std::vector<std::uint8_t> payload;
            std::string attachment;
            std::string encoding;
            std::int64_t receive_ns = 0;
        };

        // One relay of a stream per host: an advisory lock on a file in /dev/shm named after the stream, held while
        // this relay lives (released by the kernel when the process ends, also on a crash).
        class HostLock
        {
        public:
            explicit HostLock(const std::string& base)
            {
                std::string name = base;
                std::replace(name.begin(), name.end(), '/', '.');
                path_ = "/dev/shm/durchblick-relay." + name + ".lock";
            }
            ~HostLock()
            {
                if (fd_ >= 0)
                {
                    ::close(fd_);
                }
            }
            // true once this relay holds the lock (tries again on each call until it does)
            bool held()
            {
                if (fd_ >= 0)
                {
                    return true;
                }
                const int fd = ::open(path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
                if (fd < 0)
                {
                    throw std::runtime_error("cannot open " + path_ + ": " + std::strerror(errno));
                }
                if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
                {
                    ::close(fd);
                    return false;
                }
                fd_ = fd;
                return true;
            }

        private:
            std::string path_;
            int fd_ = -1;
        };

        // A discovery round's outcome: a source to relay, or why not
        enum class Verdict
        {
            Relay,      // `source` is set
            StandBack,  // this relay must not publish (another relay, a source with its own ring, ...): at once
            NoSource,   // no source seen in this round: torn down only after kMissesBeforeDrop rounds
        };
    }  // namespace

    struct RelayStream::Impl
    {
        zenoh::Session& session;
        RelayOptions o;
        Log log;
        std::string host;  // this PC's host id
        HostLock lock;

        std::atomic<bool> running{true};
        std::thread discoverer, worker;

        // discovery -> worker
        std::mutex source_mutex;
        std::condition_variable source_cv;
        std::optional<Source> found;  // the source to relay; empty: none
        std::string state = "looking for the source";  // what the relay does, for status()

        // the worker's own
        std::optional<Source> current;
        std::string published_encoding;  // the publisher's encoding (what the frames decode to)
        std::unique_ptr<CameraPublisher> publisher;
        std::optional<zenoh::Subscriber<void>> subscriber;
        std::chrono::steady_clock::time_point last_wanted{};
        std::int64_t own_seq = 0;
        bool nvjpeg = false;
#ifdef DURCHBLICK_RELAY_CUDA
        std::unique_ptr<NvJpegDecoder> decoder;
#endif

        // Zenoh's thread -> worker
        std::mutex sample_mutex;
        std::condition_variable sample_cv;
        std::optional<Sample> sample;
        std::optional<std::int64_t> arrived_seq;  // the source's seq of the last sample that arrived (gap count)

        mutable std::mutex stats_mutex;
        RelayStats stats;
        RelayStats reported;  // stats at the last status()
        std::chrono::steady_clock::time_point reported_at = std::chrono::steady_clock::now();
        std::string last_error;
        std::string decoder_name = "none";  // under stats_mutex (status() reads it)

        Impl(zenoh::Session& s, RelayOptions options, Log l)
            : session(s), o(std::move(options)), log(std::move(l)), lock(o.base)
        {
            host = host_id();
        }

        void set_state(const std::string& text)
        {
            std::lock_guard guard(source_mutex);
            if (state != text)
            {
                log(o.base + ": " + text);
            }
            state = text;
        }

        void set_decoder_name(const std::string& name)
        {
            std::lock_guard guard(stats_mutex);
            decoder_name = name;
        }

        // ---------------------------------------------------------------------------------- discovery thread
        void discover()
        {
            int misses = 0;
            std::string noted;  // a remark logged once (several sources)
            while (running.load())
            {
                bool searching = true;
                bool standing_back = false;
                try
                {
                    const auto records = stream_infos(session, o.base, std::chrono::milliseconds(1000));
                    std::optional<Source> keep;
                    {
                        std::lock_guard guard(source_mutex);
                        keep = found;
                    }
                    std::optional<Source> choice;
                    std::string remark;
                    const Verdict verdict = choose(records, keep, choice, remark);
                    if (not remark.empty() and remark != noted)
                    {
                        log(o.base + ": " + remark);
                    }
                    noted = remark;
                    std::lock_guard guard(source_mutex);
                    if (verdict == Verdict::Relay)
                    {
                        misses = 0;
                        searching = false;
                        if (choice != found)
                        {
                            found = choice;
                            source_cv.notify_all();
                        }
                    }
                    else if (verdict == Verdict::StandBack or ++misses >= kMissesBeforeDrop or not found)
                    {
                        standing_back = verdict == Verdict::StandBack;
                        if (found)
                        {
                            found.reset();
                            source_cv.notify_all();
                        }
                    }
                    else
                    {
                        searching = false;  // missed once or twice: keep relaying, look again at the usual pace
                    }
                }
                catch (const std::exception& e)
                {
                    // a query that failed or a record of another version: the source stays as it was
                    set_state(std::string("cannot read the stream's info records: ") + e.what());
                }
                // a stream this relay stands back from (not an image stream, a source with a ring here) is looked at
                // again at the usual pace, not every second
                const auto wait = searching and not standing_back ? std::chrono::milliseconds(1000) : o.rediscover;
                const auto until = std::chrono::steady_clock::now() + wait;
                while (running.load() and std::chrono::steady_clock::now() < until)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }

        Verdict stand_back(const std::string& why)
        {
            set_state(why);
            return Verdict::StandBack;
        }

        Verdict choose(const std::vector<StreamInfo>& records, const std::optional<Source>& keep,
                       std::optional<Source>& choice, std::string& remark)
        {
            if (not lock.held())
            {
                return stand_back("another relay of this stream runs on this host: nothing relayed");
            }
            std::vector<const StreamInfo*> sources;
            for (const StreamInfo& r : records)
            {
                if (r.kind != "image")
                {
                    return stand_back("the stream is of kind '" + r.kind + "', not image: nothing relayed");
                }
                if (r.relayed_from.empty())
                {
                    sources.push_back(&r);
                }
                // our own record (this host holds the lock) and the relays of other hosts: no matter. Each relay
                // serves the readers of its own host (the rings' keys name the host, Durchblick 0.3.59).
            }
            if (sources.empty())
            {
                set_state("looking for the source (no publisher's info record yet)");
                return Verdict::NoSource;
            }
            // a source with a ring on this host: its readers here take that ring. (A source with rings on another
            // host is relayed: those rings serve the readers of its own host only.)
            const auto here = std::find_if(sources.begin(), sources.end(), [this](const StreamInfo* r) {
                return r->host == host and (r->offers("shm") or r->offers("cuda"));
            });
            if (here != sources.end())
            {
                return stand_back("the source publishes a ring on this host: nothing to relay (its readers here "
                                  "take it)");
            }
            // stable: the source being relayed while it is there, else the one on the lowest host id
            std::sort(sources.begin(), sources.end(),
                      [](const StreamInfo* a, const StreamInfo* b) { return a->host < b->host; });
            const StreamInfo* s = sources.front();
            if (keep)
            {
                const auto same = std::find_if(sources.begin(), sources.end(),
                                               [&](const StreamInfo* r) { return r->host == keep->host; });
                if (same != sources.end())
                {
                    s = *same;
                }
            }
            if (sources.size() > 1)
            {
                remark = std::to_string(sources.size()) + " publishers of this stream: relaying the one on host " +
                         s->host.substr(0, 8);
            }
            Source out{s->host, "", s->encoding, s->frame_id, s->source};
            if (s->offers("compressed") and not o.prefer_raw)
            {
                out.transport = "compressed";
            }
            else if (s->offers("raw"))
            {
                out.transport = "raw";
            }
            else
            {
                return stand_back("the source offers neither compressed nor raw: nothing to relay");
            }
            choice = out;
            return Verdict::Relay;
        }

        // ------------------------------------------------------------------------------------ worker thread
        void work()
        {
            while (running.load())
            {
                try
                {
                    step();
                }
                catch (const std::exception& e)
                {
                    count_error(std::string("relay: ") + e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
            subscriber.reset();
        }

        void step()
        {
            std::optional<Source> wanted_source;
            {
                std::lock_guard guard(source_mutex);
                wanted_source = found;
            }
            if (wanted_source != current)
            {
                switch_source(wanted_source);
            }
            if (not publisher)
            {
                std::unique_lock guard(source_mutex);
                source_cv.wait_for(guard, std::chrono::milliseconds(200));
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            const bool wanted = publisher->subscribed();
            if (wanted)
            {
                last_wanted = now;
                if (not subscriber)
                {
                    subscribe();
                }
            }
            else if (subscriber and now - last_wanted > o.idle)
            {
                subscriber.reset();
                {
                    std::lock_guard guard(sample_mutex);
                    sample.reset();
                }
                set_state("idle: no reader of the relay's rings on this host, the source is not taken");
            }
            std::optional<Sample> taken;
            {
                std::unique_lock guard(sample_mutex);
                sample_cv.wait_for(guard, std::chrono::milliseconds(50), [this] { return sample.has_value(); });
                taken.swap(sample);
            }
            if (taken and wanted)
            {
                relay(*taken);
            }
        }

        std::unique_ptr<CameraPublisher> make_publisher(const Source& s, const std::string& encoding)
        {
            PublisherOptions options;
            options.encoding = encoding;
            options.frame_id = s.frame_id;
            options.source = s.source;
            options.slots = o.slots;
            options.cuda_device = o.cuda_device;
            options.transports =
                o.cuda_device ? std::vector<std::string>{"shm", "cuda"} : std::vector<std::string>{"shm"};
            options.relayed_from = s.host;
            return std::make_unique<CameraPublisher>(session, o.base, options);
        }

        void switch_source(const std::optional<Source>& next)
        {
            subscriber.reset();
            publisher.reset();
            if (not next)
            {
                current.reset();
                set_state("looking for the source");
                return;
            }
            // a JPEG source's frames decode without alpha (rgba8 -> rgb8); a PNG source's keep their encoding
            // (corrected at its first frame, adopt_encoding)
            const std::string encoding =
                next->transport == "compressed" ? jpeg_decoded(next->encoding) : next->encoding;
            bool use_nvjpeg = false;
            std::string name = next->transport == "raw" ? "none (raw)" : "cpu";
#ifdef DURCHBLICK_RELAY_CUDA
            if (o.cuda_device and next->transport == "compressed" and NvJpegDecoder::supports(encoding))
            {
                if (not decoder or decoder->device() != *o.cuda_device)
                {
                    decoder = std::make_unique<NvJpegDecoder>(*o.cuda_device);  // throws: retried, current unchanged
                }
                use_nvjpeg = true;
                name = "nvjpeg";
            }
#endif
            publisher = make_publisher(*next, encoding);  // throws: retried at the next step (current unchanged)
            published_encoding = encoding;
            nvjpeg = use_nvjpeg;
            set_decoder_name(name);
            current = next;  // only now: a failed setup above is tried again
            log(o.base + ": relaying from host " + next->host.substr(0, 8) + " (" + next->transport + ", " +
                encoding + ", decoded by " + name + ") into this host's " +
                (o.cuda_device ? "shm and CUDA rings (GPU " + std::to_string(*o.cuda_device) + ")" : "shm ring") +
                ", on demand");
            set_state("idle: no reader of the relay's rings on this host, the source is not taken");
        }

        // The frames decode to another encoding than the publisher has (a PNG source of rgba8, say): the publisher
        // is made anew with it, once; readers of its ring choose again.
        void adopt_encoding(const std::string& encoding)
        {
            log(o.base + ": the source's frames decode to " + encoding + ", not " + published_encoding +
                ": the relay publishes " + encoding);
            subscriber.reset();
            publisher = make_publisher(*current, encoding);
            published_encoding = encoding;
            subscribe();
        }

        void subscribe()
        {
            {
                std::lock_guard guard(sample_mutex);
                arrived_seq.reset();  // no gap counted across an idle time
                sample.reset();
            }
            const std::string key = o.base + "/" +
                                    std::string(current->transport == "compressed" ? p::kCompressed : p::kImageRaw);
            subscriber.emplace(session.declare_subscriber(
                zenoh::KeyExpr(key), [this](const zenoh::Sample& s) { on_sample(s); }, zenoh::closures::none));
            set_state("relaying: a reader on this host takes the relay's rings");
        }

        // Zenoh's thread: keep the newest sample; count the gaps in the source's seq as the samples arrive
        void on_sample(const zenoh::Sample& s)
        {
            Sample in;
            in.payload = s.get_payload().as_vector();
            if (auto a = s.get_attachment())
            {
                in.attachment = a->get().as_string();
            }
            in.encoding = s.get_encoding().as_string();
            in.receive_ns = now_ns();
            std::optional<std::int64_t> seq;
            try
            {
                seq = parse_frame_attachment(in.attachment).seq;
            }
            catch (const std::exception&)
            {
                // reported by relay() when the sample is taken
            }
            std::uint64_t lost = 0;
            bool overwritten = false;
            {
                std::lock_guard guard(sample_mutex);
                if (seq and arrived_seq and *seq > *arrived_seq + 1)
                {
                    lost = static_cast<std::uint64_t>(*seq - *arrived_seq - 1);
                }
                if (seq)
                {
                    arrived_seq = seq;  // a source that restarted counts from its new seq on
                }
                overwritten = sample.has_value();
                sample = std::move(in);
            }
            {
                std::lock_guard guard(stats_mutex);
                ++stats.received;
                stats.lost += lost;
                stats.overwritten += overwritten ? 1 : 0;
            }
            sample_cv.notify_all();
        }

        void relay(const Sample& in)
        {
            const bool compressed = current->transport == "compressed";
            const p::MessageType& type = compressed ? p::kCompressedImageType : p::kImageType;
            if (in.encoding != type.encoding())
            {
                throw ProtocolError(o.base + ": the source sends '" + in.encoding + "', the relay reads '" +
                                    type.encoding() + "'");
            }
            const FrameMeta meta = parse_frame_attachment(in.attachment);
            const std::int64_t seq = meta.seq.value_or(++own_seq);
            if (meta.publish_ns)
            {
                std::lock_guard guard(stats_mutex);
                stats.transit_ms_sum += static_cast<double>(in.receive_ns - *meta.publish_ns) / 1e6;
                ++stats.transit_count;
            }
            const auto t0 = std::chrono::steady_clock::now();
            std::int64_t stamp = 0;
            if (compressed)
            {
                const CompressedImage message = CompressedImage::deserialize(in.payload);
                stamp = message.header.stamp.to_ns();
                const Origin origin{message.header, seq, meta.source};
#ifdef DURCHBLICK_RELAY_CUDA
                if (nvjpeg and message.format.find("jpeg") != std::string::npos)
                {
                    const DecodedOnDevice d = decoder->decode(message.data, published_encoding);
                    // the copy into the ring is ordered after the decode on the decoder's stream; the next decode
                    // into the same buffer comes after it on that stream as well
                    publisher->publish(DeviceImage{d.ptr, d.height, d.width, decoder->stream()}, origin);
                }
                else
#endif
                {
                    const DecodedImage image = decompress(message);
                    if (image.encoding != published_encoding)
                    {
                        adopt_encoding(image.encoding);
                    }
                    publisher->publish(HostImage{image.pixels.data(), image.height, image.width, image.step}, origin);
                }
            }
            else
            {
                const Image message = Image::deserialize(in.payload);
                stamp = message.header.stamp.to_ns();
                if (message.encoding != published_encoding)
                {
                    adopt_encoding(message.encoding);
                }
                const std::uint32_t row =
                    message.width * static_cast<std::uint32_t>(encoding_info(message.encoding).pixel_bytes());
                const Origin origin{message.header, seq, meta.source};
                if (message.step != row and o.cuda_device)
                {
                    // the CUDA ring takes rows without padding
                    std::vector<std::uint8_t> packed(static_cast<std::size_t>(row) * message.height);
                    for (std::uint32_t y = 0; y < message.height; ++y)
                    {
                        std::memcpy(packed.data() + static_cast<std::size_t>(y) * row,
                                    message.data.data() + static_cast<std::size_t>(y) * message.step, row);
                    }
                    publisher->publish(HostImage{packed.data(), message.height, message.width, 0}, origin);
                }
                else
                {
                    publisher->publish(HostImage{message.data.data(), message.height, message.width, message.step},
                                       origin);
                }
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const double age = stamp > 0 ? static_cast<double>(now_ns() - stamp) / 1e6 : 0.0;
            std::lock_guard guard(stats_mutex);
            ++stats.relayed;
            stats.decode_ms_sum += ms;
            stats.decode_ms_max = std::max(stats.decode_ms_max, ms);
            stats.age_ms_sum += age;
            stats.age_ms_max = std::max(stats.age_ms_max, age);
        }

        void count_error(const std::string& text)
        {
            bool fresh = false;
            {
                std::lock_guard guard(stats_mutex);
                ++stats.errors;
                fresh = text != last_error;
                last_error = text;
            }
            if (fresh)
            {
                log(o.base + ": " + text);
            }
        }
    };

    RelayStream::RelayStream(zenoh::Session& session, RelayOptions options, Log log)
        : impl_(std::make_unique<Impl>(session, std::move(options), std::move(log)))
    {
        check_name(impl_->o.base);
        impl_->discoverer = std::thread([this] { impl_->discover(); });
        impl_->worker = std::thread([this] { impl_->work(); });
    }

    RelayStream::~RelayStream()
    {
        impl_->running.store(false);
        impl_->source_cv.notify_all();
        impl_->sample_cv.notify_all();
        if (impl_->worker.joinable())
        {
            impl_->worker.join();  // undeclares the subscriber: no sample callback after this
        }
        if (impl_->discoverer.joinable())
        {
            impl_->discoverer.join();
        }
        impl_->publisher.reset();
    }

    const std::string& RelayStream::base() const { return impl_->o.base; }

    RelayStats RelayStream::stats() const
    {
        std::lock_guard guard(impl_->stats_mutex);
        return impl_->stats;
    }

    DiagnosticStatus RelayStream::status()
    {
        Impl& m = *impl_;
        RelayStats now, before;
        std::string error, decoder;
        double seconds;
        {
            std::lock_guard guard(m.stats_mutex);
            now = m.stats;
            before = m.reported;
            m.reported = now;
            error = m.last_error;
            decoder = m.decoder_name;
            const auto t = std::chrono::steady_clock::now();
            seconds = std::chrono::duration<double>(t - m.reported_at).count();
            m.reported_at = t;
        }
        std::string state;
        std::optional<Source> source;
        {
            std::lock_guard guard(m.source_mutex);
            state = m.state;
            source = m.found;
        }
        const double frames = static_cast<double>(now.relayed - before.relayed);
        const double transits = static_cast<double>(now.transit_count - before.transit_count);
        DiagnosticStatus s;
        s.name = "relay_" + last_token(m.o.base);
        s.hardware_id = m.host.substr(0, 8);
        s.level = now.errors > before.errors ? DiagnosticStatus::kError
                  : source                   ? DiagnosticStatus::kOk
                                             : DiagnosticStatus::kWarn;
        s.message = state + (now.errors > before.errors ? " (errors: " + error + ")" : "");
        auto value = [&s](const std::string& key, const std::string& v) { s.values.push_back(KeyValue{key, v}); };
        value("rate_hz", fixed(seconds > 0 ? frames / seconds : 0.0));
        value("source_host", source ? source->host.substr(0, 8) : "");
        value("source_transport", source ? source->transport : "");
        value("decoder", decoder);
        value("decode_publish_ms_mean", fixed(frames > 0 ? (now.decode_ms_sum - before.decode_ms_sum) / frames : 0.0, 2));
        value("decode_publish_ms_max_total", fixed(now.decode_ms_max, 2));
        value("age_ms_mean", fixed(frames > 0 ? (now.age_ms_sum - before.age_ms_sum) / frames : 0.0, 2));
        value("age_ms_max_total", fixed(now.age_ms_max, 2));
        value("transit_ms_mean", fixed(transits > 0 ? (now.transit_ms_sum - before.transit_ms_sum) / transits : 0.0, 2));
        value("received", std::to_string(now.received));
        value("relayed", std::to_string(now.relayed));
        value("overwritten", std::to_string(now.overwritten));
        value("lost", std::to_string(now.lost));
        value("errors", std::to_string(now.errors));
        return s;
    }
}  // namespace durchblick::relay
