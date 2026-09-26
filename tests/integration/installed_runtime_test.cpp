#include "install_fixture.hpp"
#include "rtsp_test_server.hpp"
#include "skai/video/gstreamer_runtime.hpp"

using InstalledRuntime = skai::test::InstallFixture;
TEST_F(InstalledRuntime, ServesInstalledUiAndStateFromAnUnrelatedWorkingDirectory) {
    if (SKAI_INSTALL_GPU && !std::filesystem::is_regular_file(SKAI_INSTALL_ENGINE))
        GTEST_SKIP() << "Jetson TensorRT engine unavailable";
    std::string error;
    ASSERT_TRUE(skai::gst::initialize_once(error)) << error;
    skai::test::RtspTestServer server;
    ASSERT_TRUE(server.start(error)) << error;
    EXPECT_EQ(command({SKAI_PYTHON, "-B", SKAI_INSTALL_RUNTIME_PROBE, root.string(),
        server.url(), SKAI_INSTALL_ENGINE, std::to_string(SKAI_INSTALL_GPU)}), 0) << output;
}
