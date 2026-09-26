#include "install_fixture.hpp"
#include "skai/config.hpp"
#include <sqlite3.h>
#include <set>

namespace {
using Install = skai::test::InstallFixture;
using skai::test::installed_read;

TEST_F(Install, InstallsRuntimeLayoutAndValidEmptyDatabase) {
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "usr/local/bin/skai-edge"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "usr/local/lib/systemd/system/skai-edge.service"));
    EXPECT_NE(installed_read(root / "usr/local/share/skai-edge/web/index.html").find("SKAI Edge Console"),
              std::string::npos);
    for (const auto* name : {"recordings", "alerts", "models"})
        EXPECT_TRUE(std::filesystem::is_directory(root / "var/lib/skai-edge" / name));
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2((root / "var/lib/skai-edge/skai-edge.db").c_str(), &db,
                            SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT version FROM schema_migrations", -1, &statement, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 1);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM alerts", -1, &statement, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 0);
    sqlite3_finalize(statement); sqlite3_close(db);
    const auto mode = std::filesystem::status(root / "etc/skai-edge/config.yaml").permissions();
    EXPECT_EQ(mode & std::filesystem::perms::mask, std::filesystem::perms::owner_read |
              std::filesystem::perms::owner_write | std::filesystem::perms::group_read);
}

TEST_F(Install, ConfigurationUsesInstalledPathsAndPreservesTheRtspExample) {
    const auto config = skai::load_config((root / "etc/skai-edge/config.yaml").string());
    ASSERT_TRUE(config.ok) << config.error;
    EXPECT_EQ(config.config.web.root, "/usr/local/share/skai-edge/web");
    EXPECT_EQ(config.config.detector.engine, "/var/lib/skai-edge/models/yolo11s.engine");
    EXPECT_EQ(config.config.storage.database_path, "/var/lib/skai-edge/skai-edge.db");
    EXPECT_EQ(config.config.storage.alert_directory, "/var/lib/skai-edge/alerts");
    EXPECT_EQ(config.config.recording.directory, "/var/lib/skai-edge/recordings");
    const auto example = skai::load_config(SKAI_SOURCE_DIR "/config/config.example.yaml");
    ASSERT_TRUE(example.ok);
    EXPECT_EQ(config.config.video.rtsp_url, example.config.video.rtsp_url);
    EXPECT_FALSE(config.config.whip.enabled);
}

TEST_F(Install, ReinstallPreservesOperatorConfigurationDatabaseAndMedia) {
    const auto config = root / "etc/skai-edge/config.yaml";
    const auto changed = installed_read(config) + "\n# operator configuration\n";
    std::ofstream(config) << changed;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open((root / "var/lib/skai-edge/skai-edge.db").c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE operator_data(value TEXT); INSERT INTO operator_data VALUES('keep');",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
    const auto before = installed_read(root / "var/lib/skai-edge/skai-edge.db");
    std::ofstream(root / "var/lib/skai-edge/recordings/keep.mp4") << "keep recording";
    std::ofstream(root / "var/lib/skai-edge/alerts/keep.jpg") << "keep snapshot";
    ASSERT_EQ(install(), 0) << output;
    EXPECT_EQ(installed_read(config), changed);
    EXPECT_EQ(installed_read(root / "var/lib/skai-edge/skai-edge.db"), before);
    EXPECT_EQ(installed_read(root / "var/lib/skai-edge/recordings/keep.mp4"), "keep recording");
    EXPECT_EQ(installed_read(root / "var/lib/skai-edge/alerts/keep.jpg"), "keep snapshot");
}

TEST_F(Install, InstallsOnlyRuntimeArtifactsAndExactDependencyPins) {
    const auto manifest = installed_read(root / "usr/local/share/skai-edge/dependency-revisions.txt");
    EXPECT_NE(manifest.find("ad6574d97fa62f0aa71cb1138e3e2974abf19dcb"), std::string::npos);
    EXPECT_NE(manifest.find("0d6adc021953d7263fd4503482ea7bde33553724"), std::string::npos);
    const std::set<std::string> expected = {"usr/local/bin/skai-edge", "etc/skai-edge/config.yaml",
        "var/lib/skai-edge/skai-edge.db", "usr/local/lib/systemd/system/skai-edge.service",
        "usr/local/share/skai-edge/dependency-revisions.txt", "usr/local/share/skai-edge/web/index.html",
        "usr/local/share/skai-edge/web/app.js", "usr/local/share/skai-edge/web/style.css",
        "usr/local/share/skai-edge/web/diagnostics.html", "usr/local/share/skai-edge/web/diagnostics.js",
        "usr/local/share/skai-edge/web/diagnostics.css"};
    std::set<std::string> actual;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto relative = entry.path().lexically_relative(root).generic_string();
        if (entry.is_regular_file() && relative != "command.log") actual.insert(relative);
    }
    EXPECT_EQ(actual, expected);
}

TEST_F(Install, PreservesExistingDirectoryModesAndDanglingStateSymlinks) {
    const auto state = root / "var/lib/skai-edge";
    std::filesystem::permissions(state, std::filesystem::perms::owner_all);
    const auto config = root / "etc/skai-edge/config.yaml";
    const auto moved = root / "operator.yaml";
    std::filesystem::rename(config, moved);
    std::filesystem::create_symlink(moved, config);
    const auto db = state / "skai-edge.db";
    std::filesystem::remove(db);
    std::filesystem::create_symlink(root / "future.db", db);
    ASSERT_EQ(install(), 0) << output;
    EXPECT_EQ(std::filesystem::status(state).permissions(), std::filesystem::perms::owner_all);
    EXPECT_TRUE(std::filesystem::is_symlink(config));
    EXPECT_TRUE(std::filesystem::is_symlink(db));
    EXPECT_FALSE(std::filesystem::exists(root / "future.db"));
}

TEST_F(Install, InstalledExecutableRunsOutsideTheSourceCheckout) {
    const auto binary = (root / "usr/local/bin/skai-edge").string();
    ASSERT_EQ(command({binary, "--help"}), 0) << output;
    EXPECT_NE(output.find("Usage:"), std::string::npos);
    ASSERT_EQ(command({binary, "--version"}), 0) << output;
    EXPECT_NE(output.find("skai-edge"), std::string::npos);
}

TEST_F(Install, RejectsAnInstallTimePrefixChangeBeforeCopyingFiles) {
    const auto rejected = root / "rejected";
    EXPECT_NE(command({SKAI_CMAKE, "--install", SKAI_BUILD_DIR, "--prefix", "/opt/skai-edge"}, rejected), 0);
    EXPECT_FALSE(std::filesystem::exists(rejected));
}
} // namespace
