#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

namespace {

struct ProcessResult {
    int status;
    std::string output;
};

struct SignalResult {
    bool ready;
    bool exited_in_time;
    int status;
};

pid_t launch(const std::vector<std::string>& arguments, int output_fd) {
    const pid_t pid = fork();
    if (pid == 0) {
        if (output_fd >= 0) {
            dup2(output_fd, STDOUT_FILENO);
            dup2(output_fd, STDERR_FILENO);
            close(output_fd);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(SKAI_EDGE_EXECUTABLE));
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execv(SKAI_EDGE_EXECUTABLE, argv.data());
        _exit(127);
    }
    return pid;
}

bool wait_for_exit(pid_t pid, int& status) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return true;
        if (result < 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return false;
}

ProcessResult run_command(const std::vector<std::string>& arguments) {
    int descriptors[2];
    if (pipe(descriptors) != 0) return {-1, "pipe failed"};
    const pid_t pid = launch(arguments, descriptors[1]);
    close(descriptors[1]);
    if (pid < 0) {
        close(descriptors[0]);
        return {-1, "fork failed"};
    }
    int status = -1;
    if (!wait_for_exit(pid, status)) status = -1;
    std::string output;
    char buffer[256];
    ssize_t count;
    while ((count = read(descriptors[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(descriptors[0]);
    return {status, output};
}

SignalResult signal_and_wait(int signal_number) {
    int descriptors[2];
    if (pipe(descriptors) != 0) return {false, false, -1};
    const pid_t pid = launch({}, descriptors[1]);
    close(descriptors[1]);
    if (pid < 0) {
        close(descriptors[0]);
        return {false, false, -1};
    }

    pollfd ready_pipe{descriptors[0], POLLIN, 0};
    const bool ready_output = poll(&ready_pipe, 1, 2000) > 0 &&
                              (ready_pipe.revents & POLLIN);
    char buffer[256]{};
    const ssize_t count = ready_output ? read(descriptors[0], buffer, sizeof(buffer)) : 0;
    const bool ready = count > 0 &&
                       std::string(buffer, static_cast<std::size_t>(count)).find("skai-edge ready") !=
                           std::string::npos;

    if (!ready) {
        kill(pid, SIGKILL);
        int status = -1;
        waitpid(pid, &status, 0);
        close(descriptors[0]);
        return {false, false, status};
    }

    if (kill(pid, signal_number) != 0) {
        int status = -1;
        wait_for_exit(pid, status);
        close(descriptors[0]);
        return {true, false, status};
    }
    int status = -1;
    const bool exited_in_time = wait_for_exit(pid, status);
    close(descriptors[0]);
    return {true, exited_in_time, status};
}

} // namespace

TEST(Process, HelpExitsSuccessfullyWithUsage) {
    const auto result = run_command({"--help"});
    ASSERT_TRUE(WIFEXITED(result.status));
    EXPECT_EQ(WEXITSTATUS(result.status), 0);
    EXPECT_NE(result.output.find("Usage:"), std::string::npos);
}

TEST(Process, VersionExitsSuccessfully) {
    const auto result = run_command({"--version"});
    ASSERT_TRUE(WIFEXITED(result.status));
    EXPECT_EQ(WEXITSTATUS(result.status), 0);
    EXPECT_NE(result.output.find("skai-edge"), std::string::npos);
}

TEST(Process, UnknownOptionFailsWithUsage) {
    const auto result = run_command({"--unknown"});
    ASSERT_TRUE(WIFEXITED(result.status));
    EXPECT_NE(WEXITSTATUS(result.status), 0);
    EXPECT_NE(result.output.find("Usage:"), std::string::npos);
}

TEST(Process, SigintExitsSuccessfullyWithinTimeout) {
    const auto result = signal_and_wait(SIGINT);
    ASSERT_TRUE(result.ready);
    ASSERT_TRUE(result.exited_in_time);
    ASSERT_TRUE(WIFEXITED(result.status));
    EXPECT_EQ(WEXITSTATUS(result.status), 0);
}

TEST(Process, SigtermExitsSuccessfullyWithinTimeout) {
    const auto result = signal_and_wait(SIGTERM);
    ASSERT_TRUE(result.ready);
    ASSERT_TRUE(result.exited_in_time);
    ASSERT_TRUE(WIFEXITED(result.status));
    EXPECT_EQ(WEXITSTATUS(result.status), 0);
}
