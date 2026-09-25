#include "h264_sei_test_support.hpp"
#include "skai/health.hpp"
#include "skai/webrtc/detection_sei.hpp"
#include "skai/webrtc/ice_runtime_module.hpp"
#include "skai/webrtc/whip_publisher.hpp"

#include <rtc/rtc.hpp>

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace skai_test;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct ReceivedFrame {
    Bytes bytes;
    std::uint32_t rtp_timestamp = 0;
};

// Minimal WHIP endpoint: answers one POST with a libdatachannel peer that
// depacketizes H.264, and acknowledges DELETE.
class LoopbackWhipServer {
public:
    explicit LoopbackWhipServer(std::string location = "/whip/session-1",
                                http::status post_status = http::status::created)
        : acceptor_(context_, {asio::ip::make_address("127.0.0.1"), 0}),
          location_(std::move(location)), post_status_(post_status) {
        worker_ = std::thread([this] { serve(); });
    }
    ~LoopbackWhipServer() {
        stopping_ = true;
        // close() does not wake a blocked accept(); a throwaway connection does.
        beast::error_code ignored;
        tcp::socket wake(context_);
        wake.connect(acceptor_.local_endpoint(), ignored);
        if (worker_.joinable()) worker_.join();
        acceptor_.close(ignored);
        if (peer_) peer_->close();
    }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) +
               "/whip";
    }
    bool wait_for_open_track(std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return track_open_; });
    }
    std::vector<ReceivedFrame> frames() {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }
    int posts() const { return posts_; }
    int deletes() const { return deletes_; }

private:
    void serve() {
        while (!stopping_) {
            beast::error_code error;
            tcp::socket socket(context_);
            acceptor_.accept(socket, error);
            if (error || stopping_) return;
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, error);
            if (error) continue;
            http::response<http::string_body> response;
            response.version(request.version());
            if (request.method() == http::verb::post) {
                ++posts_;
                response.result(post_status_);
                if (!location_.empty() && post_status_ == http::status::created) {
                    response.set(http::field::location, location_);
                    response.set(http::field::content_type, "application/sdp");
                    response.body() = answer(request.body());
                }
            } else if (request.method() == http::verb::delete_) {
                ++deletes_;
                response.result(http::status::ok);
            } else {
                response.result(http::status::method_not_allowed);
            }
            response.prepare_payload();
            http::write(socket, response, error);
        }
    }

    std::string answer(const std::string& offer) {
        auto peer = std::make_shared<rtc::PeerConnection>();
        std::mutex gather_mutex;
        std::condition_variable gathered;
        bool complete = false;
        peer->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete) return;
            std::lock_guard<std::mutex> lock(gather_mutex);
            complete = true;
            gathered.notify_all();
        });
        peer->onTrack([this](std::shared_ptr<rtc::Track> track) {
            track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(
                rtc::NalUnit::Separator::StartSequence));
            track->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
                std::lock_guard<std::mutex> lock(mutex_);
                ReceivedFrame frame;
                frame.bytes.resize(data.size());
                std::memcpy(frame.bytes.data(), data.data(), data.size());
                frame.rtp_timestamp = info.timestamp;
                frames_.push_back(std::move(frame));
            });
            // Set under the lock so the waiter cannot miss the wakeup.
            track->onOpen([this] {
                std::lock_guard<std::mutex> lock(mutex_);
                track_open_ = true;
                changed_.notify_all();
            });
            std::lock_guard<std::mutex> lock(mutex_);
            track_ = std::move(track);
        });
        peer->setRemoteDescription(rtc::Description(offer, rtc::Description::Type::Offer));
        std::unique_lock<std::mutex> lock(gather_mutex);
        gathered.wait_for(lock, std::chrono::seconds(5), [&] { return complete; });
        lock.unlock();
        const auto local = peer->localDescription();
        peer->onGatheringStateChange(nullptr);
        peer_ = peer;
        return local ? std::string(*local) : std::string();
    }

    asio::io_context context_;
    tcp::acceptor acceptor_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> posts_{0};
    std::atomic<int> deletes_{0};
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<rtc::Track> track_;
    bool track_open_ = false;
    std::vector<ReceivedFrame> frames_;
    std::string location_;
    http::status post_status_;
};

struct SourceUnit {
    skai::EncodedAccessUnit unit;
    std::vector<Nal> nals;
};

std::vector<SourceUnit> source_segment(std::uint64_t generation, int frames) {
    std::vector<SourceUnit> units;
    for (auto& encoded : encode_test_stream(frames)) {
        SourceUnit source;
        source.unit.has_pts = true;
        source.unit.pts_ns = encoded.pts_ns;
        source.nals = split_annex_b(encoded.bytes);
        source.unit.keyframe = std::any_of(source.nals.begin(), source.nals.end(),
                                           [](const Nal& nal) { return nal.type == 5; });
        source.unit.discontinuity = units.empty();
        source.unit.source_generation = generation;
        source.unit.bytes = encoded.bytes;
        units.push_back(std::move(source));
    }
    return units;
}

bool same_nals(const std::vector<Nal>& a, const std::vector<Nal>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].type != b[i].type || a[i].ebsp != b[i].ebsp) return false;
    }
    return true;
}

} // namespace

TEST(WhipMetrics, FatalLocationErrorRemainsVisibleAfterWorkerExits) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::Config config;
    config.webrtc.host_interfaces = {"lo"};
    skai::IceRuntimeModule runtime(logger);
    ASSERT_TRUE(runtime.initialize(config)) << runtime.last_error();
    ASSERT_EQ(setenv("WHIP_TOKEN", "loopback-test", 1), 0);
    LoopbackWhipServer server("");
    config.whip.enabled = true;
    config.whip.url = server.url();
    skai::WhipPublisher publisher(logger);
    ASSERT_TRUE(publisher.initialize(config)) << publisher.last_error();
    ASSERT_TRUE(publisher.start()) << publisher.last_error();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           publisher.metrics().peer_state != "failed") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto metrics = publisher.metrics();
    EXPECT_EQ(metrics.peer_state, "failed") << logs.str();
    EXPECT_EQ(metrics.ice_state, "closed");
    EXPECT_NE(metrics.last_error.find("Location"), std::string::npos);
    skai::WebRtcDiagnostics lan;
    lan.enabled = true;
    EXPECT_EQ(skai::webrtc_health(lan, metrics).state, skai::HealthState::Failed);
    EXPECT_EQ(server.posts(), 1);
    publisher.stop();
    publisher.wait();
    ASSERT_EQ(unsetenv("WHIP_TOKEN"), 0);
}

TEST(WhipMetrics, RetryDiscardedAccessUnitsAreReportedSeparatelyFromOverflow) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::Config config;
    config.webrtc.host_interfaces = {"lo"};
    skai::IceRuntimeModule runtime(logger);
    ASSERT_TRUE(runtime.initialize(config)) << runtime.last_error();
    ASSERT_EQ(setenv("WHIP_TOKEN", "loopback-test", 1), 0);
    LoopbackWhipServer server("", http::status::service_unavailable);
    config.whip.enabled = true;
    config.whip.url = server.url();
    skai::WhipPublisher publisher(logger);
    ASSERT_TRUE(publisher.initialize(config)) << publisher.last_error();
    skai::EncodedAccessUnit unit;
    for (int index = 0; index < 4; ++index) publisher.publish_access_unit(unit);
    ASSERT_TRUE(publisher.start()) << publisher.last_error();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           publisher.metrics().media_queue_discarded < 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto metrics = publisher.metrics();
    EXPECT_EQ(metrics.media_queue_drops, 0U);
    EXPECT_EQ(metrics.media_queue_discarded, 4U) << logs.str();
    publisher.stop();
    publisher.wait();
    ASSERT_EQ(unsetenv("WHIP_TOKEN"), 0);
}

TEST(WhipSei, ViewerRecoversEveryResultAcrossAnRtspRestartAndMediaIsUnchanged) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::Config config;
    config.webrtc.host_interfaces = {"lo"};
    skai::IceRuntimeModule runtime(logger);
    ASSERT_TRUE(runtime.initialize(config)) << runtime.last_error();
    ASSERT_EQ(setenv("WHIP_TOKEN", "loopback-test", 1), 0);

    LoopbackWhipServer server;
    config.whip.enabled = true;
    config.whip.url = server.url();
    skai::WhipPublisher publisher(logger);
    ASSERT_TRUE(publisher.initialize(config)) << publisher.last_error();
    ASSERT_TRUE(publisher.start()) << publisher.last_error();
    ASSERT_TRUE(server.wait_for_open_track(std::chrono::seconds(10))) << logs.str();

    // Two pipeline generations; the second restarts PTS at zero like a reconnect.
    std::vector<SourceUnit> sent = source_segment(1, 40);
    const auto restart_index = sent.size();
    for (auto& unit : source_segment(2, 40)) sent.push_back(std::move(unit));
    ASSERT_EQ(sent.size(), 80U);
    const auto originals = sent;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        publisher.publish_access_unit(sent[i].unit);
        // Result i identifies its source unit through the class name.
        skai::SeiSourceResult result{sent[i].unit.pts_ns, sent[i].unit.source_generation, {}};
        result.boxes.push_back(skai::normalize_sei_box(10, 20, 110, 220, 320, 240,
                                                       "u" + std::to_string(i), 0.9f));
        publisher.publish_detections(std::move(result));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    publisher.stop();
    publisher.wait();
    ASSERT_EQ(unsetenv("WHIP_TOKEN"), 0);

    // The caller's units, i.e. the copies LAN WHEP and recording receive, are untouched.
    for (std::size_t i = 0; i < sent.size(); ++i) {
        ASSERT_EQ(sent[i].unit.bytes, originals[i].unit.bytes) << "unit " << i;
    }
    EXPECT_EQ(server.posts(), 1);
    EXPECT_EQ(server.deletes(), 1);
    const auto frames = server.frames();
    ASSERT_GE(frames.size(), 76U) << logs.str();

    std::vector<std::size_t> source_of_frame;
    std::size_t next_source = 0;
    std::size_t results_seen = 0;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        // Removing the one detection SEI must give back the source unit's NALs.
        std::vector<Nal> media;
        std::optional<ParsedSei> sei;
        bool seen_vcl = false;
        for (auto& nal : split_annex_b(frames[f].bytes)) {
            if (nal.type >= 1 && nal.type <= 5) seen_vcl = true;
            auto parsed = parse_user_data_sei(nal);
            if (parsed && parsed->uuid == skai::kDetectionSeiUuid) {
                ASSERT_FALSE(sei) << "frame " << f << " carries two detection SEI";
                EXPECT_FALSE(seen_vcl) << "SEI after a slice in frame " << f;
                EXPECT_TRUE(parsed->trailing_bits_ok);
                sei = std::move(parsed);
                continue;
            }
            media.push_back(std::move(nal));
        }
        while (next_source < sent.size() && !same_nals(media, sent[next_source].nals)) {
            ++next_source;
        }
        ASSERT_LT(next_source, sent.size()) << "frame " << f << " differs from every source unit";
        source_of_frame.push_back(next_source++);
        if (f > 0) {
            const auto step = static_cast<std::int32_t>(frames[f].rtp_timestamp -
                                                         frames[f - 1].rtp_timestamp);
            EXPECT_GT(step, 0) << "RTP timestamp moved backward at frame " << f;
            // Exactly one frame, when the unit sent just before the restart arrived.
            if (source_of_frame[f] == restart_index &&
                source_of_frame[f - 1] == restart_index - 1) {
                EXPECT_EQ(step, 3600);
            }
        }
        if (!sei) continue;

        // Viewer rule: the source frame is the one whose RTP ts equals ts - dt.
        for (const auto& result : parse_results(sei->payload)) {
            ASSERT_EQ(result.boxes.size(), 1U);
            const auto index = std::stoul(result.boxes[0].class_name.substr(1));
            const std::uint32_t source_ts = frames[f].rtp_timestamp - result.dt;
            std::optional<std::size_t> source_frame;
            for (std::size_t k = 0; k <= f; ++k) {
                if (frames[k].rtp_timestamp == source_ts) source_frame = k;
            }
            ASSERT_TRUE(source_frame) << "result u" << index << " points at no frame";
            EXPECT_EQ(source_of_frame[*source_frame], index) << "frame " << f;
            EXPECT_EQ(index >= restart_index, source_of_frame[f] >= restart_index)
                << "result crossed the RTSP restart at frame " << f;
            ++results_seen;
        }
    }
    EXPECT_GE(results_seen, 72U) << logs.str();
    EXPECT_NE(logs.str().find("sei units="), std::string::npos) << logs.str();
    EXPECT_NE(logs.str().find("dt p50="), std::string::npos) << logs.str();
}
