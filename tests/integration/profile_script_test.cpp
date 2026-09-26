#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void check(const char* scenario) {
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        execl(SKAI_PYTHON, SKAI_PYTHON, SKAI_PROFILE_CHECK, scenario, nullptr);
        _exit(127);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
}
TEST(ProfileScript, ExcludesWarmupAndKeepsUnmeasuredStagesNull) { check("timings"); }
TEST(ProfileScript, RejectsCounterResetsAndNonFiniteValues) { check("invalid"); }
TEST(ProfileScript, CpuUsesTaskIdentityAndOneCoreScale) { check("cpu"); }
TEST(ProfileScript, RejectsFrozenMediaAndChangedPeers) { check("freeze"); }
TEST(ProfileScript, RetainsPerFramePercentilesAndRawCounts) { check("summary"); }
TEST(ProfileScript, StopsOwnedProcessOnFailure) { check("cleanup"); }
TEST(ProfileScript, SeparatesReceiverDelayFromEndToEndAndRejectsResets) { check("receiver"); }
TEST(ProfileScript, WritesFailureEvidenceAndStopsBothOwnedProcesses) { check("runner"); }
TEST(ProfileScript, RecordsCompilerFlagsAndRequiresReleaseForHardwareEvidence) { check("build"); }
