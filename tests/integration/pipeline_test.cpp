#include "rtsp_test_server.hpp"
#include "skai/application.hpp"
#include "skai/alerts/alert_manager.hpp"
#include "skai/gps/gps_module.hpp"
#include "skai/storage/database.hpp"
#include "skai/video/recording.hpp"
#include "skai/video/rtsp_video_module.hpp"
#include "skai/web/http_server.hpp"
#include "skai/webrtc/ice_runtime_module.hpp"
#include "skai/inference/yolo_inference_module.hpp"

#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <opencv2/imgcodecs.hpp>
#include <rtc/rtc.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

// Only inference is substituted; frame consumption, commits, events and async
// alert persistence all run through the production YoloInferenceModule.
class FixtureBackend final : public skai::InferenceBackend {
public:
    explicit FixtureBackend(std::shared_ptr<std::atomic<bool>> fail) : fail_(fail) {}
    bool load(const std::string&, std::string&) override { return true; }
    bool run(const skai::BgrImageView& image, std::uint64_t sequence,
             skai::DetectionResult& result, skai::InferenceTiming& timing,
             std::string& error) override {
        if (*fail_) { error = "injected inference failure"; return false; }
        int min_x = image.width, min_y = image.height, max_x = -1, max_y = -1;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const auto offset = y * image.stride + x * 3;
                if (image.data[offset] > 200 && image.data[offset + 1] > 200 &&
                    image.data[offset + 2] > 200) {
                    min_x = std::min(min_x, x); min_y = std::min(min_y, y);
                    max_x = std::max(max_x, x); max_y = std::max(max_y, y);
                }
            }
        }
        result.frame_sequence = sequence;
        if (max_x >= 0) result.detections.push_back({0, 1.0f, float(min_x),
            float(min_y), float(max_x + 1), float(max_y + 1)});
        timing.inference_wall_ms = 0.1;
        return true;
    }
private:
    std::shared_ptr<std::atomic<bool>> fail_;
};

#ifdef SKAI_PIPELINE_TENSORRT
#define PipelineSuite PipelineJetson
#else
#define PipelineSuite Pipeline
#endif

class PipelineSuite : public ::testing::Test {
protected:
    void SetUp() override {
        char pattern[] = "/tmp/skai-pipeline-XXXXXX";
        const auto* directory = mkdtemp(pattern);
        ASSERT_NE(directory, nullptr);
        root_ = directory;
        std::string error;
        ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
        cv::Mat image(640, 640, CV_8UC3);
        for (int y = 0; y < image.rows; ++y) {
            for (int x = 0; x < image.cols; ++x) {
                image.at<cv::Vec3b>(y, x) = cv::Vec3b(
                    (x + y) % 256, (2 * x + y) % 256, (x + 2 * y) % 256);
            }
        }
#ifndef SKAI_PIPELINE_TENSORRT
        image.setTo(cv::Scalar(0, 0, 0));
        image(cv::Rect(160, 160, 160, 160)).setTo(cv::Scalar(255, 255, 255));
#endif
        ASSERT_TRUE(cv::imwrite((root_ / "frame.png").string(), image));
        ASSERT_TRUE(rtsp_.set_image((root_ / "frame.png").string()));
        ASSERT_TRUE(rtsp_.start(error)) << error;
        const auto path = root_ / "config.yaml";
        std::ofstream config(path);
        config << "video: {rtsp_url: '" << rtsp_.url()
               << "', transport: tcp, latency_ms: 50}\n"
               << "web: {bind: '127.0.0.1', port: 0}\n"
               << "detector: {engine: '/var/lib/skai-edge/models/yolo11s_fp16.engine', confidence: 0.1}\n"
               << "storage: {database_path: '" << (root_ / "alerts.db").string()
               << "', alert_directory: '" << (root_ / "alerts").string() << "'}\n"
               << "recording: {enabled: true, directory: '"
               << (root_ / "recordings").string()
               << "', segment_seconds: 1, min_free_space_mb: 1}\n"
               << "webrtc: {enabled: true, host_interfaces: [lo]}\n"
               << "whip: {enabled: false}\n"
#ifdef SKAI_PIPELINE_TENSORRT
               << "alerts: [{class: umbrella, confidence: 0.1, consecutive_frames: 2, cooldown_seconds: 60}]\n";
#else
               << "alerts: [{class: person, confidence: 0.9, consecutive_frames: 2, cooldown_seconds: 60}]\n";
#endif
        config.close();
        ASSERT_TRUE(config);
        skai::Application::Modules modules;
        auto database = std::make_unique<skai::Database>();
        repository_ = std::make_shared<skai::AlertRepository>(*database);
        modules.storage = std::move(database);
        modules.webrtc = std::make_unique<skai::IceRuntimeModule>(logger_);
        modules.gps = std::make_unique<skai::GpsModule>(gps_);
        auto alerts = std::make_shared<skai::AlertManager>(gps_, events_, repository_, &logger_);
#ifdef SKAI_PIPELINE_TENSORRT
        modules.detector = std::make_unique<skai::YoloInferenceModule>(
            frames_, logger_, status_, api_, events_, alerts);
#else
        modules.detector = std::make_unique<skai::YoloInferenceModule>(
            frames_, logger_, status_, api_, events_, alerts,
            [fail = fail_](skai::Logger&, const skai::DetectorConfig&) {
                return std::make_unique<FixtureBackend>(fail);
            });
#endif
        modules.recording = std::make_unique<skai::RecordingModule>(units_, logger_, recording_, events_);
        modules.video = std::make_unique<skai::RtspVideoModule>(frames_, units_, logger_, status_,
            [manager = manager_](const auto& unit) { manager->publish_access_unit(unit); },
            [manager = manager_, recording = recording_](bool available, const auto& reason) {
                manager->set_media_available(available, reason);
                recording->set_media_available(available, reason);
            });
        auto web = std::make_unique<skai::web::HttpServer>(logger_, status_, api_, events_,
                                                        repository_, recording_, manager_);
        web_ = web.get();
        modules.web = std::move(web);
        app_ = std::make_unique<skai::Application>(path.string(), logger_, std::move(modules));
        ASSERT_TRUE(app_->initialize()) << app_->last_error() << logs_.str();
        ASSERT_TRUE(app_->start()) << app_->last_error() << logs_.str();
        status_->set_running(true);
        events_->subscribe([this](const std::string& event) {
            std::lock_guard<std::mutex> lock(event_mutex_);
            published_.push_back(event);
        });
    }
    void TearDown() override {
        if (app_) { app_->stop(); app_->wait(); }
        rtsp_.stop();
        app_.reset();
        std::error_code ignored;
        if (!root_.empty()) std::filesystem::remove_all(root_, ignored);
    }
    http::response<http::string_body> request(const std::string& path,
            http::verb method = http::verb::get, const std::string& body = {}) {
        asio::io_context context;
        tcp::socket socket(context);
        socket.connect({asio::ip::make_address("127.0.0.1"), web_->port()});
        // Bound synchronous reads/writes even if a broken server stops responding.
        timeval timeout{5, 0};
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        http::request<http::string_body> outgoing{method, path, 11};
        outgoing.set(http::field::host, "127.0.0.1");
        if (!body.empty()) outgoing.set(http::field::content_type, "application/sdp");
        outgoing.body() = body;
        outgoing.prepare_payload();
        http::write(socket, outgoing);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> incoming;
        http::read(socket, buffer, incoming);
        return incoming;
    }
    template <typename Predicate> bool wait_until(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        do {
            if (predicate()) return true;
            std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
    std::vector<skai::AlertEvent> alerts() {
        std::string error;
        const auto result = repository_->find_recent(10, error);
        EXPECT_TRUE(error.empty()) << error;
        return result;
    }
    std::shared_ptr<std::atomic<bool>> fail_ = std::make_shared<std::atomic<bool>>(false);
    std::mutex event_mutex_;
    std::vector<std::string> published_;
    std::filesystem::path root_;
    skai::test::RtspTestServer rtsp_;
    std::ostringstream logs_;
    skai::Logger logger_{logs_};
    skai::BoundedQueue<skai::Frame> frames_{2};
    skai::BoundedQueue<skai::EncodedAccessUnit> units_{120};
    std::shared_ptr<skai::RuntimeStatus> status_ = std::make_shared<skai::RuntimeStatus>();
    std::shared_ptr<skai::GpsState> gps_ = std::make_shared<skai::GpsState>();
    std::shared_ptr<skai::ApiState> api_ = std::make_shared<skai::ApiState>(status_, gps_);
    std::shared_ptr<skai::EventChannel> events_ = std::make_shared<skai::EventChannel>();
    std::shared_ptr<skai::RecordingController> recording_ = std::make_shared<skai::RecordingController>();
    std::shared_ptr<skai::WebRtcManager> manager_ = std::make_shared<skai::WebRtcManager>(logger_);
    std::shared_ptr<skai::AlertRepository> repository_;
    skai::web::HttpServer* web_ = nullptr;
    std::unique_ptr<skai::Application> app_;
};

TEST_F(PipelineSuite, PersistsRtspDetectionsAlertsSnapshotsAndPlayableRecording) {
    ASSERT_TRUE(wait_until([&] {
        return !alerts().empty() && recording_->status().active &&
               api_->latest_detections().frame_sequence >= 20 &&
               status_->snapshot().status == "running";
    })) << "alerts=" << alerts().size()
        << " detections=" << api_->latest_detections().detections.size()
        << " units=" << recording_->status().access_units_written
        << " status=" << status_->snapshot().status << '\n' << logs_.str();
    const auto persisted = alerts();
    ASSERT_EQ(persisted.size(), 1U); // cooldown prevents a new alert every frame
    const auto& alert = persisted.front();
    ASSERT_FALSE(alert.detections.empty());
#ifdef SKAI_PIPELINE_TENSORRT
    EXPECT_EQ(alert.detections.front().class_name, "umbrella");
#else
    EXPECT_EQ(alert.detections.front().class_name, "person");
    EXPECT_NEAR(alert.detections.front().x1, 160, 2);
    EXPECT_NEAR(alert.detections.front().x2, 320, 2);
#endif
    EXPECT_GT(alert.frame_sequence, 0U);
    ASSERT_TRUE(alert.gps.has_value());
    EXPECT_NEAR(alert.gps->latitude, 25.033964, 0.000001);
    EXPECT_NEAR(alert.gps->longitude, 121.564468, 0.000001);
    const auto snapshot = cv::imread(alert.snapshot_path);
    ASSERT_FALSE(snapshot.empty());
    EXPECT_EQ(snapshot.cols, 640);
    EXPECT_EQ(snapshot.rows, 640);
    const auto status = request("/api/v1/status");
    EXPECT_EQ(status.result(), http::status::ok);
    EXPECT_NE(status.body().find("\"status\":\"running\""), std::string::npos);
    const auto detections = request("/api/v1/detections/latest");
    EXPECT_EQ(detections.result(), http::status::ok);
    EXPECT_NE(detections.body().find("\"available\":true"), std::string::npos);
    const auto recent = request("/api/v1/alerts");
    EXPECT_EQ(recent.result(), http::status::ok);
    EXPECT_NE(recent.body().find(alert.id), std::string::npos);
    const auto detail = request("/api/v1/alerts/" + alert.id);
    EXPECT_EQ(detail.result(), http::status::ok);
    EXPECT_NE(detail.body().find(alert.snapshot_path), std::string::npos);
    const auto recording = request("/api/v1/recordings");
    EXPECT_EQ(recording.result(), http::status::ok);
    EXPECT_NE(recording.body().find("\"active\":true"), std::string::npos);
    EXPECT_EQ(request("/api/v1/recording/stop", http::verb::post).result(), http::status::ok);
    ASSERT_TRUE(wait_until([&] { return recording_->status().state == "stopped"; }));
    EXPECT_GT(recording_->status().access_units_written, 0U);
    app_->stop(); app_->wait();

    // Reopen SQLite after application shutdown; the API result must be durable.
    skai::Database database;
    const auto config = app_->config();
    ASSERT_TRUE(database.initialize(config));
    skai::AlertRepository reopened(database);
    std::string error;
    const auto durable = reopened.find_by_id(alert.id, error);
    ASSERT_TRUE(durable.has_value()) << error;
    EXPECT_EQ(durable->frame_sequence, alert.frame_sequence);
    EXPECT_EQ(durable->snapshot_path, alert.snapshot_path);

    int playable = 0;
    for (const auto& file : std::filesystem::directory_iterator(root_ / "recordings")) {
        if (file.path().extension() != ".mp4") continue;
        EXPECT_GT(file.file_size(), 0U);
        // Decode each finalized MP4 to EOS, rather than accepting a header-only file.
        std::atomic<int> decoded{0};
        auto pipeline = skai::gst::Pipeline::from_launch(
            "filesrc location=\"" + file.path().string() +
            "\" ! qtdemux ! h264parse ! avdec_h264 ! "
            "fakesink name=decoded sync=false signal-handoffs=true", logger_, error);
        ASSERT_NE(pipeline, nullptr) << error;
        auto* sink = gst_bin_get_by_name(GST_BIN(pipeline->element()), "decoded");
        ASSERT_NE(sink, nullptr);
        g_signal_connect(sink, "handoff", G_CALLBACK(+[](
                GstElement*, GstBuffer*, GstPad*, gpointer data) {
            ++*static_cast<std::atomic<int>*>(data);
        }), &decoded);
        gst_object_unref(sink);
        ASSERT_TRUE(pipeline->start(2s)) << pipeline->last_error();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        skai::gst::BusEvent event;
        do {
            event = pipeline->poll(100ms);
        } while (event.type != skai::gst::BusEventType::Eos &&
                 event.type != skai::gst::BusEventType::Error &&
                 std::chrono::steady_clock::now() < deadline);
        EXPECT_EQ(event.type, skai::gst::BusEventType::Eos) << event.detail;
        pipeline->stop();
        EXPECT_GT(decoded.load(), 0);
        if (event.type == skai::gst::BusEventType::Eos && decoded > 0) ++playable;
    }
    EXPECT_GE(playable, 1);
}

#ifndef SKAI_PIPELINE_TENSORRT
TEST_F(PipelineSuite, ProductionInferenceInvalidatesFailuresAndPublishesRecovery) {
    ASSERT_TRUE(wait_until([&] { return api_->latest_detections().available; }));
    *fail_ = true;
    ASSERT_TRUE(wait_until([&] { return !api_->latest_detections().available; }));
    EXPECT_FALSE(status_->snapshot().last_inference_ms.has_value());
    ASSERT_TRUE(wait_until([&] {
        std::lock_guard<std::mutex> lock(event_mutex_);
        return std::any_of(published_.begin(), published_.end(), [](const auto& event) {
            return event.find("\"type\":\"detection\"") != std::string::npos &&
                   event.find("\"available\":false") != std::string::npos;
        });
    }));
    *fail_ = false;
    ASSERT_TRUE(wait_until([&] { return api_->latest_detections().available; }));
    ASSERT_TRUE(wait_until([&] { return !alerts().empty(); }));
    std::lock_guard<std::mutex> lock(event_mutex_);
    EXPECT_TRUE(std::any_of(published_.begin(), published_.end(), [](const auto& event) {
        return event.find("\"type\":\"detection\"") != std::string::npos &&
               event.find("\"class_name\":\"person\"") != std::string::npos;
    }));
}
#endif

#ifndef SKAI_PIPELINE_TENSORRT
TEST_F(PipelineSuite, WhepRouteDeliversRtspH264AndDeletesConnectedPeer) {
    ASSERT_TRUE(wait_until([&] { return manager_->diagnostics().keyframe_cached; }));
    // The receiver uses the same skai-ice library as the application.
    auto peer = std::make_shared<rtc::PeerConnection>();
    rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
    video.addH264Codec(96);
    auto track = peer->addTrack(video);
    track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::StartSequence));
    std::atomic<int> received{0};
    std::atomic<std::size_t> bytes{0};
    track->onFrame([&](rtc::binary frame, rtc::FrameInfo) {
        bytes += frame.size();
        ++received;
    });
    std::mutex mutex;
    std::condition_variable changed;
    bool gathered = false;
    peer->onGatheringStateChange([&](auto state) {
        if (state != rtc::PeerConnection::GatheringState::Complete) return;
        std::lock_guard<std::mutex> lock(mutex);
        gathered = true;
        changed.notify_all();
    });
    // Declare callback data before this guard so teardown cannot outlive it.
    struct ClosePeer {
        std::shared_ptr<rtc::PeerConnection> peer;
        std::shared_ptr<rtc::Track> track;
        ~ClosePeer() { track->resetCallbacks(); peer->resetCallbacks(); peer->close(); }
    } cleanup{peer, track};
    peer->setLocalDescription(rtc::Description::Type::Offer);
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] { return gathered; }));
    }
    const auto offer = peer->localDescription();
    ASSERT_TRUE(offer.has_value());
    const auto response = request("/api/v1/webrtc/whep", http::verb::post, std::string(*offer));
    ASSERT_EQ(response.result(), http::status::created) << response.body();
    EXPECT_EQ(response[http::field::content_type], "application/sdp");
    ASSERT_NO_THROW(peer->setRemoteDescription(
        rtc::Description(response.body(), rtc::Description::Type::Answer)));
    ASSERT_TRUE(wait_until([&] { return received >= 3; })) << logs_.str();
    EXPECT_EQ(peer->state(), rtc::PeerConnection::State::Connected);
    EXPECT_GT(bytes.load(), 100U);
    EXPECT_EQ(manager_->session_count(), 1U);
    EXPECT_EQ(request("/api/v1/status").result(), http::status::ok);
    const std::string location(response[http::field::location]);
    ASSERT_FALSE(location.empty());
    EXPECT_EQ(request(location, http::verb::delete_).result(), http::status::no_content);
    EXPECT_EQ(manager_->session_count(), 0U);
    EXPECT_EQ(request(location, http::verb::delete_).result(), http::status::not_found);
}
#endif
} // namespace
