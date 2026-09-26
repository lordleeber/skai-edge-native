#include "skai/runtime.hpp"
#include "skai/video/gstreamer_runtime.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace {
class EmptyBackend final : public skai::InferenceBackend {
public:
    bool load(const std::string&, std::string&) override { return true; }
    bool run(const skai::BgrImageView&, std::uint64_t sequence,
             skai::DetectionResult& result, skai::InferenceTiming&, std::string&) override {
        result.frame_sequence = sequence;
        return true;
    }
};
}

TEST(RuntimeFactory, OwnsDistinctEphemeralPortsAndLifecycleStatus) {
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    char pattern[] = "/tmp/skai-runtime-XXXXXX";
    const auto* directory = mkdtemp(pattern);
    ASSERT_NE(directory, nullptr);
    struct RemoveDirectory {
        std::filesystem::path path;
        ~RemoveDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    std::ostringstream logs;
    skai::Logger logger(logs);
    std::vector<std::unique_ptr<skai::Runtime>> runtimes;
    for (int index = 0; index < 2; ++index) {
        const auto path = cleanup.path / (std::to_string(index) + ".yaml");
        std::ofstream config(path);
        config << "video: {rtsp_url: 'rtsp://127.0.0.1:9/test'}\n"
               << "web: {bind: '127.0.0.1', port: 0}\n"
               << "storage: {database_path: '" << path.string() << ".db'}\n"
               << "recording: {enabled: false}\n"
               << "webrtc: {host_interfaces: [lo]}\n";
        config.close();
        auto runtime = skai::create_runtime(path.string(), logger, {},
            [](skai::Logger&, const skai::DetectorConfig&) {
                return std::make_unique<EmptyBackend>();
            });
        ASSERT_TRUE(runtime->initialize()) << runtime->last_error();
        EXPECT_GT(runtime->http_port(), 0);
        EXPECT_EQ(runtime->status()->snapshot().status, "starting");
        ASSERT_TRUE(runtime->start()) << runtime->last_error();
        EXPECT_EQ(runtime->status()->snapshot().status, "degraded");
        runtimes.push_back(std::move(runtime));
    }
    EXPECT_NE(runtimes[0]->http_port(), runtimes[1]->http_port());
    for (auto& runtime : runtimes) {
        runtime->stop(); runtime->wait();
        EXPECT_EQ(runtime->status()->snapshot().status, "starting");
    }
}
