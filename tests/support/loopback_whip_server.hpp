#pragma once
#include "h264_sei_test_support.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <rtc/rtc.hpp>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace skai_test {
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

} // namespace skai_test
