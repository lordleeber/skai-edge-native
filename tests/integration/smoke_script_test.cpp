#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
const std::string run_a(32, 'a');
const std::string run_b(32, 'b');

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

pid_t launch(const std::vector<std::string>& args, const std::filesystem::path& log) {
    const auto pid = fork();
    if (pid == 0) {
        setsid();
        const auto fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd);
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

class SmokeScript : public ::testing::Test {
protected:
    void SetUp() override {
        char pattern[] = "/tmp/skai-smoke-script-XXXXXX";
        const auto* directory = mkdtemp(pattern);
        ASSERT_NE(directory, nullptr);
        root = directory;
        std::ofstream(root / "engine") << "fixture engine";
        executable("edge", "exec '" SKAI_PYTHON "' '" SKAI_SMOKE_FIXTURE "' \"$@\"\n");
        executable("ffmpeg", "if [[ \"$(cat '" + (root / "result/recordings/fixture.mp4").string() +
                   "')\" == broken ]]; then exit 1; fi\nexit 0\n");
        executable("ffprobe", "echo '{\"streams\":[{\"codec_name\":\"h264\","
                   "\"width\":640,\"height\":480,\"nb_read_frames\":\"10\"}]}'\n");
        driver = launch({SKAI_PYTHON, SKAI_SMOKE_FIXTURE, "--driver"}, root / "driver.log");
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (read(root / "driver.log").find(':') == std::string::npos &&
               std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
        const auto log = read(root / "driver.log");
        ASSERT_NE(log.find(':'), std::string::npos) << log;
        driver_url = "http://127.0.0.1:" + log.substr(log.find(':') + 1);
        while (!driver_url.empty() && driver_url.back() == '\n') driver_url.pop_back();
    }
    void TearDown() override {
        if (driver > 0) { kill(-driver, SIGKILL); waitpid(driver, nullptr, 0); }
        if (!root.empty()) std::filesystem::remove_all(root);
    }
    void executable(const std::string& name, const std::string& body) {
        std::ofstream(root / name) << "#!/usr/bin/env bash\n" << body;
        std::filesystem::permissions(root / name, std::filesystem::perms::owner_all);
    }
    int run(const std::string& mode) {
        const auto config = root / "config.yaml";
        std::ofstream(config) << "video: {rtsp_url: 'rtsp://fixture/" << mode << "'}\n"
            << "detector: {engine: '" << (root / "engine").string() << "'}\n"
            << "webrtc: {enabled: true, host_interfaces: [lo]}\n"
            << "alerts: [{class: chair, confidence: 0.1}]\n";
        const auto before = read(config);
        const auto pid = launch({SKAI_SMOKE_SCRIPT, config.string(), "--binary",
            (root / "edge").string(), "--output-dir", (root / "result").string(),
            "--webdriver-url", driver_url, "--host", "127.0.0.1", "--timeout", "2",
            "--seconds", "0.1", "--shutdown-timeout", "0.5", "--skip-device-check",
            "--ffmpeg", (root / "ffmpeg").string(), "--ffprobe", (root / "ffprobe").string()},
            root / "runner.log");
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (waitpid(pid, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                kill(-pid, SIGKILL); waitpid(pid, &status, 0); break;
            }
            std::this_thread::sleep_for(20ms);
        }
        EXPECT_EQ(read(config), before);
        output = read(root / "runner.log");
        report = read(root / "result/report.json");
        const auto marker = report.find("\"edge_pid\": ");
        if (marker != std::string::npos) {
            const auto owned_pid = std::stoi(report.substr(marker + 12));
            EXPECT_EQ(kill(owned_pid, 0), -1) << "owned child leaked: " << owned_pid;
            EXPECT_EQ(errno, ESRCH);
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    int confirm(const std::string& url,
                const std::string& report_url = "http://172.16.1.50:8080/#smoke=" + run_b,
                const std::string& run_id = run_b) {
        const auto path = root / "acceptance.json";
        std::ofstream artifact(path);
        artifact << "{\"checks\":{\"jetson\":\"test artifact\"},"
            "\"automated_passed\":true,\"url\":\"" << report_url << "\",\"status\":\"incomplete\"}";
        artifact.close();
        if (!run_id.empty()) {
            auto value = read(path);
            value.insert(1, "\"run_id\":\"" + run_id + "\",");
            std::ofstream(path) << value;
        }
        const auto pid = launch({SKAI_SMOKE_SCRIPT, "--confirm-lan-report", path.string(),
            "--observed-url", url}, root / "confirmation.log");
        int status = 0;
        waitpid(pid, &status, 0);
        report = read(path);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    std::filesystem::path root;
    pid_t driver = -1;
    std::string driver_url, output, report;
};

TEST_F(SmokeScript, VerifiesArtifactsAndMarksLocalBrowserAsIncompleteLanAcceptance) {
    EXPECT_EQ(run("healthy"), 2) << output;
    EXPECT_NE(report.find("\"automated_passed\": true"), std::string::npos) << report;
    EXPECT_NE(report.find("manual_pending"), std::string::npos);
    EXPECT_NE(report.find("sqlite_persistence"), std::string::npos);
    EXPECT_NE(report.find("clean_shutdown"), std::string::npos);
}

TEST_F(SmokeScript, RejectsMissingInference) {
    EXPECT_EQ(run("no-inference"), 1) << output;
    EXPECT_NE(report.find("timed out waiting for TensorRT inference"), std::string::npos) << report;
}
TEST_F(SmokeScript, RequiresANewPersistedAlert) {
    EXPECT_EQ(run("no-alert"), 1) << output;
    EXPECT_NE(report.find("timed out waiting for new persisted alert"), std::string::npos) << report;
}
TEST_F(SmokeScript, RejectsUndecodableRecording) {
    EXPECT_EQ(run("bad-media"), 1) << output;
    EXPECT_NE(report.find("ffmpeg"), std::string::npos) << report;
}
TEST_F(SmokeScript, RejectsBrowserWithoutDecodedFrames) {
    EXPECT_EQ(run("no-browser"), 1) << output;
    EXPECT_NE(report.find("did not render advancing decoded video frames"), std::string::npos) << report;
}
TEST_F(SmokeScript, RejectsForcedShutdown) {
    EXPECT_EQ(run("no-shutdown"), 1) << output;
    EXPECT_NE(report.find("clean shutdown failed"), std::string::npos) << report;
}
TEST_F(SmokeScript, WaitsForFirstRuntimeHealthSample) {
    EXPECT_EQ(run("delayed-health"), 2) << output;
    EXPECT_NE(report.find("\"runtime_health\": true"), std::string::npos) << report;
}
TEST_F(SmokeScript, RecordsExplicitConfirmationForTheMatchingSmokeUrl) {
    EXPECT_EQ(confirm("http://172.16.1.50:8080/#smoke=" + run_b), 0);
    EXPECT_NE(report.find("manual_confirmed"), std::string::npos) << report;
    EXPECT_NE(report.find("\"status\": \"passed\""), std::string::npos);
}
TEST_F(SmokeScript, RejectsConfirmationForADifferentInstance) {
    EXPECT_EQ(confirm("http://172.16.1.50:8081/"), 1);
    EXPECT_EQ(report.find("manual_confirmed"), std::string::npos);
}
TEST_F(SmokeScript, RejectsInvalidWhepSdp) {
    EXPECT_EQ(run("bad-sdp"), 1) << output;
    EXPECT_NE(report.find("browser-accepted SDP"), std::string::npos) << report;
}
TEST_F(SmokeScript, RequiresApiAlertToSurviveDatabaseReopen) {
    EXPECT_EQ(run("no-durable-alert"), 1) << output;
    EXPECT_NE(report.find("not durable in SQLite"), std::string::npos) << report;
}
TEST_F(SmokeScript, RejectsLoopbackAsLanConfirmation) {
    const auto url = "http://127.0.0.1:8080/#smoke=" + run_b;
    EXPECT_EQ(confirm(url, url), 1);
    EXPECT_EQ(report.find("manual_confirmed"), std::string::npos);
}
TEST_F(SmokeScript, UsesConfiguredPythonWhenPathResolvesAnotherInterpreter) {
    executable("python3", "echo wrong Python interpreter >&2\nexit 97\n");
    const auto original = std::string(std::getenv("PATH"));
    const auto poisoned = root.string() + ':' + original;
    ASSERT_EQ(setenv("PATH", poisoned.c_str(), 1), 0);
    EXPECT_EQ(run("healthy"), 2) << output;
    EXPECT_EQ(setenv("PATH", original.c_str(), 1), 0);
}
TEST_F(SmokeScript, ValidatesOwnedLanHostBeforeLaunchingAProcess) {
    const auto pid = launch({SKAI_PYTHON, "-B", SKAI_SMOKE_HOST_POLICY}, root / "host-policy.log");
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status)) << read(root / "host-policy.log");
    EXPECT_EQ(WEXITSTATUS(status), 0) << read(root / "host-policy.log");
}
TEST_F(SmokeScript, RejectsVideoFreezeAfterInitialBrowserVerification) {
    EXPECT_EQ(run("freeze-browser"), 1) << output;
    EXPECT_NE(report.find("video stopped advancing during observation"), std::string::npos) << report;
}
TEST_F(SmokeScript, RejectsPeerDisconnectDuringObservation) {
    EXPECT_EQ(run("disconnect-peer"), 1) << output;
    EXPECT_NE(report.find("peer stopped sending during observation"), std::string::npos) << report;
}
TEST_F(SmokeScript, RejectsFrozenPeerBytesDuringObservation) {
    EXPECT_EQ(run("freeze-bytes"), 1) << output;
    EXPECT_NE(report.find("peer stopped sending during observation"), std::string::npos) << report;
}
TEST_F(SmokeScript, RejectsFrozenRtspFramesDuringObservation) {
    EXPECT_EQ(run("freeze-rtsp"), 1) << output;
    EXPECT_NE(report.find("RTSP stopped advancing during observation"), std::string::npos) << report;
}
TEST_F(SmokeScript, RejectsObservationFromAnEarlierRunAtTheSamePort) {
    EXPECT_EQ(confirm("http://172.16.1.50:8080/#smoke=" + run_a), 1);
    EXPECT_EQ(report.find("manual_confirmed"), std::string::npos);
}
TEST_F(SmokeScript, RejectsLegacyConfirmationWithoutARunId) {
    EXPECT_EQ(confirm("http://172.16.1.50:8080", "http://172.16.1.50:8080", ""), 1);
    EXPECT_EQ(report.find("manual_confirmed"), std::string::npos);
}
TEST_F(SmokeScript, GeneratesDistinctRunIdsAndIdentifiedBrowserUrls) {
    EXPECT_EQ(run("healthy"), 2) << output;
    const auto marker = report.find("\"run_id\": \"");
    ASSERT_NE(marker, std::string::npos) << report;
    const auto first_id = report.substr(marker + 11, 32);
    EXPECT_NE(report.find("/#smoke=" + first_id), std::string::npos) << report;
    EXPECT_NE(report.find("live_observation"), std::string::npos) << report;
    std::filesystem::remove_all(root / "result");
    EXPECT_EQ(run("healthy"), 2) << output;
    EXPECT_EQ(report.find(first_id), std::string::npos) << report;
}
} // namespace
