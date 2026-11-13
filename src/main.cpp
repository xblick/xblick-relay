// xblick-relay: brings image streams of other PCs into this PC's shared-memory and CUDA rings (relay.hpp).
//
//   xblick-relay [--stream robot/ecam24/left ...] [--cuda-device 0 | --cpu]
//                    [--connect HOST ...] [--listen ENDPOINT ...] [--zenoh-mode peer|client] [--slots 32]
//                    [--idle-ms 2000] [--prefer-raw] [--node NAME] [--param NAME:=VALUE ...] [--params-file FILE ...]
//
// One relay per PC. It relays every stream that a reader on this PC asks for: a subscriber with the transport auto
// holds the liveliness token <stream>/want/<host>/<id> while the stream has no source with a ring on its host
// (Durchblick 0.3.60, protocol.hpp). A stream is relayed from its first such reader until --idle-ms after its last.
// --stream names streams to relay from the start and for as long as the relay runs, also for readers without the
// token (a transport of their own choice, an older Durchblick).
//
// Every option is also a read-only parameter of the node xblick/relay_<host> (xblick/options.hpp; --node
// another name): the terminal interface shows how the relay runs, and --param or --params-file set them at the start.
//
// Without --connect/--listen: the PC's Zenoh router (127.0.0.1:7447), as every Durchblick node. Without --cpu or
// --cuda-device: GPU 0 if there is one (nvJPEG into the CUDA ring), else the CPU (shm ring only).
// Its status, once a second per stream, on <camera>/diagnostics (status relay_<stream>; <camera> = the stream's name
// without its last part).
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <cctype>
#include <unistd.h>

#include <xblick/camera.hpp>
#include <xblick/cuda_ring.hpp>
#include <xblick/options.hpp>
#include <xblick/parameters.hpp>

#include "relay.hpp"

namespace
{
    volatile std::sig_atomic_t stop = 0;
    std::mutex log_mutex;

    void say(const std::string& text)
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%H:%M:%S", std::localtime(&t));
        std::lock_guard lock(log_mutex);
        std::cerr << stamp << " xblick-relay: " << text << std::endl;
    }

    // The second line of --help: what this build decodes with. The tests (tests/test_relay.py) read it to know
    // whether the program can serve a CUDA ring; the wheel xblick-relay is built without nvJPEG.
#ifdef XBLICK_RELAY_CUDA
    constexpr const char* kDecoders = "decoders of this build: nvJPEG (CUDA ring) and CPU";
#else
    constexpr const char* kDecoders = "decoders of this build: CPU only (built without nvJPEG, no --cuda-device)";
#endif

    // Every option is also a read-only parameter of the node (xblick/options.hpp): the relay's streams take
    // their settings when they are made.
    xblick::Options options_of()
    {
        using namespace xblick;
        Options o("xblick-relay", std::string("relays image streams of other PCs into this PC's shared-memory and "
                                              "CUDA rings\n") + kDecoders);
        o.add_list("stream", {}, {.help = "streams to relay from the start, besides those that readers on this PC "
                                          "ask for, e.g. robot/ecam24/left",
                                  .metavar = "NAME", .aliases = {"streams"}});
        o.add_optional("cuda-device", parameter_type::kInteger,
                       {.help = "decode with nvJPEG into the CUDA ring of this GPU (default: GPU 0 if there is one)"});
        o.add_flag("cpu", {.help = "decode on the CPU, shm ring only"});
        o.add("slots", std::int64_t{32}, {.help = "slots of each ring",
                                          .parameter = {.integer_range = IntegerRange{2, 1024, 1}}});
        o.add("idle-ms", std::int64_t{2000}, {.help = "how long after its last reader a stream is still relayed",
                                              .parameter = {.unit = "ms"}});
        o.add_flag("prefer-raw", {.help = "take the raw form where the source offers it"});
        o.add("node", "", {.help = "the parameters' node xblick/<node> (default: relay_<host>)"});
        o.add_session_options();
        o.alias("zenoh-mode", "mode");  // the relay's older name
        o.add_parameter_options();
        return o;
    }

    std::string host_node_name()
    {
        char host[256] = {};
        gethostname(host, sizeof host - 1);
        std::string name = std::string("relay_") + host;
        for (char& c : name)
        {
            if (not std::isalnum(static_cast<unsigned char>(c)) and c != '_')
            {
                c = '_';
            }
        }
        return name;
    }

    // The streams that readers on this PC ask for: their want tokens (<stream>/want/<host>/<id>), by stream.
    struct Wanted
    {
        std::mutex mutex;
        std::map<std::string, std::set<std::string>> tokens;

        void on_token(const zenoh::Sample& sample)
        {
            const std::string key(sample.get_keyexpr().as_string_view());
            std::size_t end = key.size();
            for (int chunk = 0; chunk < 3 and end != std::string::npos; ++chunk)  // want/<host>/<id>
            {
                end = key.rfind('/', end - 1);
            }
            if (end == std::string::npos or end == 0)
            {
                return;
            }
            const std::string base = key.substr(0, end);
            std::lock_guard lock(mutex);
            if (sample.get_kind() == Z_SAMPLE_KIND_DELETE)
            {
                tokens[base].erase(key);
                if (tokens[base].empty())
                {
                    tokens.erase(base);
                }
            }
            else
            {
                tokens[base].insert(key);
            }
        }

        std::set<std::string> streams()
        {
            std::lock_guard lock(mutex);
            std::set<std::string> out;
            for (const auto& [base, keys] : tokens)
            {
                out.insert(base);
            }
            return out;
        }
    };

    // A stream being relayed: named with --stream (for as long as the relay runs) or asked for by a reader.
    struct Relayed
    {
        std::unique_ptr<xblick::relay::RelayStream> stream;
        bool named = false;
        std::chrono::steady_clock::time_point last_wanted;
    };

    std::string camera_of(const std::string& base)
    {
        const auto slash = base.rfind('/');
        if (slash == std::string::npos)
        {
            throw std::invalid_argument("stream " + base + ": a stream name has a camera part (<camera>/<stream>)");
        }
        return base.substr(0, slash);
    }
}  // namespace

int main(int argc, char** argv)
{
    using namespace xblick;
    Options options = options_of();
    options.parse(argc, argv);
    if (options.flag("cpu") and options.has_value("cuda-device"))
    {
        std::cerr << "xblick-relay: --cpu and --cuda-device exclude each other\n" << options.usage();
        return 2;
    }
    std::signal(SIGINT, [](int) { stop = 1; });
    std::signal(SIGTERM, [](int) { stop = 1; });
    try
    {
        zenoh::Session session = open_session(options.session_options());
        const std::string node_name = options.text("node").empty() ? host_node_name() : options.text("node");
        Parameters parameters(session, check_name(node_name), "xblick");
        parameters.configure(options.parameter_startup());
        options.declare(parameters, {}, {"node"});
        parameters.check_overrides();
        std::optional<int> device;
        if (options.has_value("cuda-device"))
        {
            device = static_cast<int>(options.integer("cuda-device"));
        }
#ifdef XBLICK_RELAY_CUDA
        if (not device and not options.flag("cpu") and cuda_device_count() > 0)
        {
            device = 0;
        }
#else
        if (device)
        {
            throw std::invalid_argument("--cuda-device: this build has no nvJPEG (XBLICK_RELAY_CUDA off)");
        }
#endif
        const auto idle = std::chrono::milliseconds(options.integer("idle-ms"));
        std::map<std::string, Relayed> streams;
        std::map<std::string, std::unique_ptr<DiagnosticsPublisher>> diagnostics;
        std::set<std::string> refused;  // streams asked for that cannot be relayed (logged once)
        const auto start = [&](const std::string& name, bool named) {
            const std::string base = check_name(name);
            const std::string camera = camera_of(base);
            relay::RelayOptions relay_options;
            relay_options.base = base;
            relay_options.cuda_device = device;
            relay_options.slots = static_cast<std::uint32_t>(options.integer("slots"));
            relay_options.idle = idle;
            relay_options.prefer_raw = options.flag("prefer-raw");
            Relayed entry;
            entry.stream = std::make_unique<relay::RelayStream>(session, relay_options, say);
            entry.named = named;
            entry.last_wanted = std::chrono::steady_clock::now();
            streams.emplace(base, std::move(entry));
            if (not diagnostics.contains(camera))
            {
                diagnostics.emplace(camera, std::make_unique<DiagnosticsPublisher>(session, camera));
            }
        };
        for (const std::string& name : options.list("stream"))
        {
            start(name, true);
        }
        // the streams that readers on this PC ask for, from now on and those asking already
        Wanted wanted;
        zenoh::Session::LivelinessSubscriberOptions token_options;
        token_options.history = true;
        const std::string want_key = "**/" + std::string(protocol::kWant) + "/" + host_id() + "/*";
        auto tokens = session.liveliness_declare_subscriber(
            zenoh::KeyExpr(want_key), [&wanted](const zenoh::Sample& sample) { wanted.on_token(sample); },
            zenoh::closures::none, std::move(token_options));
        say("relaying the streams that readers on this PC ask for" +
            (streams.empty() ? std::string() : " and " + std::to_string(streams.size()) + " named stream(s)") + ", " +
            (device ? "with nvJPEG on GPU " + std::to_string(*device) : std::string("on the CPU (shm only)")));
        std::cout << "ready" << std::endl;
        // Starts the streams asked for and ends those nobody asked for since --idle-ms.
        const auto follow_readers = [&] {
            const std::set<std::string> asked = wanted.streams();
            const auto now = std::chrono::steady_clock::now();
            for (const std::string& base : asked)
            {
                if (const auto it = streams.find(base); it != streams.end())
                {
                    it->second.last_wanted = now;
                }
                else if (not refused.contains(base))
                {
                    try
                    {
                        start(base, false);
                        say(base + ": a reader on this PC asks for it");
                    }
                    catch (const std::exception& e)
                    {
                        refused.insert(base);
                        say(base + ": asked for by a reader, not relayed: " + e.what());
                    }
                }
            }
            for (auto it = streams.begin(); it != streams.end();)
            {
                if (not it->second.named and now - it->second.last_wanted > idle)
                {
                    say(it->first + ": no reader on this PC asks for it any more");
                    it = streams.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        };
        auto next = std::chrono::steady_clock::now();
        while (not stop)
        {
            next += std::chrono::seconds(1);
            while (not stop and std::chrono::steady_clock::now() < next)
            {
                follow_readers();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            std::map<std::string, DiagnosticArray> arrays;
            for (const auto& [base, relayed] : streams)
            {
                DiagnosticArray& array = arrays[camera_of(base)];
                array.header = Header{Time::from_ns(ns), ""};
                array.status.push_back(relayed.stream->status());
            }
            for (auto& [camera, array] : arrays)
            {
                diagnostics.at(camera)->publish(array);
            }
        }
        say("stopping");
        streams.clear();
    }
    catch (const std::exception& e)
    {
        say(std::string("error: ") + e.what());
        return 1;
    }
    return 0;
}
