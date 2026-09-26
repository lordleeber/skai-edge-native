#pragma once
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace skai::test {
inline std::string installed_read(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class InstallFixture : public ::testing::Test {
protected:
    void SetUp() override {
        char pattern[] = "/tmp/skai-install space-XXXXXX";
        const auto* directory = mkdtemp(pattern);
        ASSERT_NE(directory, nullptr);
        root = directory;
        std::filesystem::create_directory(root / "outside");
        ASSERT_EQ(install(), 0) << output;
    }
    void TearDown() override {
        if (!root.empty()) std::filesystem::remove_all(root);
    }
    int command(const std::vector<std::string>& args, std::filesystem::path destination = {}) {
        if (destination.empty()) destination = root;
        const auto log = root / "command.log";
        const auto pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            if (setsid() < 0) _exit(126);
            const auto fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
            close(fd);
            if (setenv("DESTDIR", destination.c_str(), 1) != 0 || chdir((root / "outside").c_str()) != 0)
                _exit(126);
            std::vector<char*> argv;
            for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        while (waitpid(pid, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                kill(-pid, SIGKILL); waitpid(pid, &status, 0); break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        output = installed_read(log);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    int install() { return command({SKAI_CMAKE, "--install", SKAI_BUILD_DIR}); }
    std::filesystem::path root;
    std::string output;
};
} // namespace skai::test
