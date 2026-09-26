#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Verification {
    int exit_code;
    std::string output;
    std::string unit_diagnostics;
    bool passed() const { return exit_code == 0 && unit_diagnostics.empty(); }
};

Verification verify_unit(std::string text) {
    char pattern[] = "/tmp/skai-systemd-XXXXXX";
    const auto* directory = mkdtemp(pattern);
    if (!directory) return {-1, "mkdtemp failed"};
    const std::filesystem::path root(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    const auto unit = root / "skai-edge.service";
    const auto log = root / "verify.log";
    // Step 37 installs the binary; verify against the actual build artifact now.
    const std::string installed = "/usr/local/bin/skai-edge";
    const auto offset = text.find(installed);
    if (offset == std::string::npos) return {-1, "missing executable path"};
    text.replace(offset, installed.size(), std::string("\"") + SKAI_EDGE_EXECUTABLE + "\"");
    std::ofstream(unit) << text;
    const auto pid = fork();
    if (pid < 0) return {-1, "fork failed"};
    if (pid == 0) {
        FILE* output = freopen(log.c_str(), "w", stdout);
        if (!output || dup2(fileno(output), STDERR_FILENO) < 0) _exit(126);
        // Inherited logging settings must not hide verifier diagnostics.
        if (setenv("SYSTEMD_LOG_LEVEL", "warning", 1) != 0 ||
                setenv("SYSTEMD_LOG_TARGET", "console", 1) != 0 ||
                setenv("SYSTEMD_COLORS", "0", 1) != 0 ||
                setenv("LC_ALL", "C", 1) != 0) _exit(126);
        execl(SKAI_SYSTEMD_ANALYZE, SKAI_SYSTEMD_ANALYZE, "--man=no", "--generators=no",
              "verify", unit.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    const auto waited = waitpid(pid, &status, 0);
    const auto output = read(log);
    std::istringstream lines(output);
    std::string line, diagnostics;
    while (std::getline(lines, line)) {
        // Ignore warnings from unrelated installed units. Include our file,
        // its temporary drop-ins and messages attributed to the unit by name.
        if (line.find(root.string() + '/') != std::string::npos ||
                line.rfind(unit.filename().string() + ':', 0) == 0) {
            diagnostics += line + '\n';
        }
    }
    return {waited == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1, output, diagnostics};
}

class SystemdService : public ::testing::Test {
protected:
    void SetUp() override {
        text = read(SKAI_SYSTEMD_UNIT);
        ASSERT_FALSE(text.empty()) << "missing systemd/skai-edge.service";
        std::string section;
        std::ifstream input(SKAI_SYSTEMD_UNIT);
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line.front() == '#') continue;
            if (line.front() == '[') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const auto equals = line.find('=');
            ASSERT_NE(equals, std::string::npos) << line;
            const auto key = section + '.' + line.substr(0, equals);
            ASSERT_TRUE(settings.emplace(key, line.substr(equals + 1)).second) << key;
        }
    }
    std::map<std::string, std::string> settings;
    std::string text;
};

TEST_F(SystemdService, RunsTheInstalledExecutableWithExplicitConfiguration) {
    EXPECT_EQ(settings["Service.Type"], "exec");
    EXPECT_EQ(settings["Service.ExecStart"],
              "/usr/local/bin/skai-edge --config /etc/skai-edge/config.yaml");
    EXPECT_EQ(settings.count("Service.PIDFile"), 0u);
}

TEST_F(SystemdService, RestartsFailuresWithDelayAndLimitsRepeatedStartupFailures) {
    EXPECT_EQ(settings["Service.Restart"], "on-failure");
    EXPECT_EQ(settings["Service.RestartSec"], "5s");
    EXPECT_EQ(settings["Unit.StartLimitIntervalSec"], "60s");
    EXPECT_EQ(settings["Unit.StartLimitBurst"], "5");
}

TEST_F(SystemdService, StopsTheOwnedProcessGroupGracefully) {
    EXPECT_EQ(settings["Service.KillSignal"], "SIGTERM");
    EXPECT_EQ(settings["Service.KillMode"], "control-group");
    EXPECT_EQ(settings["Service.TimeoutStopSec"], "30s");
    EXPECT_EQ(settings.count("Service.ExecStop"), 0u);
}

TEST_F(SystemdService, SendsBothOutputStreamsToJournal) {
    EXPECT_EQ(settings["Service.StandardOutput"], "journal");
    EXPECT_EQ(settings["Service.StandardError"], "journal");
    EXPECT_EQ(settings["Service.SyslogIdentifier"], "skai-edge");
}

TEST_F(SystemdService, UsesDedicatedIdentityAndPersistentWritableState) {
    EXPECT_EQ(settings["Service.User"], "skai-edge");
    EXPECT_EQ(settings["Service.Group"], "skai-edge");
    EXPECT_EQ(settings["Service.SupplementaryGroups"], "video render");
    EXPECT_EQ(settings["Service.WorkingDirectory"], "/var/lib/skai-edge");
    EXPECT_EQ(settings["Service.StateDirectory"], "skai-edge");
    EXPECT_EQ(settings["Service.StateDirectoryMode"], "0750");
    EXPECT_EQ(settings["Service.UMask"], "0027");
    EXPECT_EQ(settings["Service.NoNewPrivileges"], "true");
    EXPECT_EQ(settings["Service.EnvironmentFile"], "-/etc/skai-edge/skai-edge.env");
    // GPU device access and local network interfaces must remain available.
    EXPECT_EQ(settings.count("Service.PrivateDevices"), 0u);
    EXPECT_EQ(settings.count("Service.PrivateNetwork"), 0u);
}

TEST_F(SystemdService, OrdersStartupAfterNetworkAndSupportsBootEnablement) {
    EXPECT_EQ(settings["Unit.Wants"], "network-online.target");
    EXPECT_EQ(settings["Unit.After"], "network-online.target");
    EXPECT_EQ(settings["Install.WantedBy"], "multi-user.target");
}


TEST_F(SystemdService, PassesSystemdSyntaxAndDependencyVerification) {
    if (std::string(SKAI_SYSTEMD_ANALYZE).empty()) GTEST_SKIP() << "systemd-analyze unavailable";
    const auto result = verify_unit(text);
    EXPECT_TRUE(result.passed()) << result.output;
}

TEST_F(SystemdService, RejectsUnknownDirectiveEvenWhenVerifierExitsSuccessfully) {
    if (std::string(SKAI_SYSTEMD_ANALYZE).empty()) GTEST_SKIP() << "systemd-analyze unavailable";
    const auto result = verify_unit(text + "\n[Unit]\nBogusDirective=yes\n");
    EXPECT_NE(result.output.find("BogusDirective"), std::string::npos) << result.output;
    EXPECT_FALSE(result.passed()) << "exit code: " << result.exit_code << '\n' << result.output;
}

TEST_F(SystemdService, RejectsDirectiveInTheWrongSection) {
    if (std::string(SKAI_SYSTEMD_ANALYZE).empty()) GTEST_SKIP() << "systemd-analyze unavailable";
    const auto result = verify_unit(text + "\n[Unit]\nRestart=on-failure\n");
    EXPECT_NE(result.output.find("Restart"), std::string::npos) << result.output;
    EXPECT_FALSE(result.passed()) << "exit code: " << result.exit_code << '\n' << result.output;
}

} // namespace
