#include "skai/inference/tensorrt_engine.hpp"
#include "inference/tensorrt_logger.hpp"

#include <gtest/gtest.h>

#include <cuda_runtime_api.h>

#include <cstdio>
#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

class QuietLogger final : public nvinfer1::ILogger {
public:
    void log(Severity, const char*) noexcept override {}
};

class TemporaryEngine {
public:
    TemporaryEngine() {
        char pattern[] = "/tmp/skai-pr8-engine-XXXXXX";
        const int fd = mkstemp(pattern);
        if (fd >= 0) {
            path_ = pattern;
            close(fd);
        }
    }
    ~TemporaryEngine() { if (!path_.empty()) std::remove(path_.c_str()); }
    const std::string& path() const { return path_; }
    void write(const void* data, std::size_t size) {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

private:
    std::string path_;
};

} // namespace

TEST(TensorRtEngine, LoggerAcceptsConcurrentErrorsAndDiagnosticReads) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::detail::TensorRtLogger trt_logger(logger);
    std::atomic<bool> writers_done{false};
    std::atomic<bool> malformed{false};
    std::thread reader([&] {
        while (!writers_done.load()) {
            const auto error = trt_logger.last_error();
            if (!error.empty() && error != "engine incompatible" &&
                error != "plugin unavailable") {
                malformed.store(true);
            }
        }
    });
    std::vector<std::thread> writers;
    for (int index = 0; index < 4; ++index) {
        writers.emplace_back([&, index] {
            for (int attempt = 0; attempt < 100; ++attempt) {
                trt_logger.log(nvinfer1::ILogger::Severity::kERROR,
                               index % 2 == 0 ? "engine incompatible" : "plugin unavailable");
            }
        });
    }
    for (auto& writer : writers) writer.join();
    writers_done.store(true);
    reader.join();
    EXPECT_FALSE(malformed.load());
    EXPECT_FALSE(trt_logger.last_error().empty());
}

TEST(TensorRtEngine, StandardPluginsRequireExplicitBootstrap) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtBootstrap bootstrap(logger);
    std::string error;
    EXPECT_TRUE(bootstrap.initialize_standard_plugins(error)) << error;
    EXPECT_TRUE(bootstrap.initialize_standard_plugins(error)) << error;
}

TEST(TensorRtEngine, ReportsMissingEmptyAndIncompatibleFiles) {
    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtEngine loader(logger);
    std::string error;
    EXPECT_FALSE(loader.load("/tmp/skai-pr8-engine-does-not-exist", error));
    EXPECT_NE(error.find("cannot open"), std::string::npos);
    TemporaryEngine file;
    ASSERT_FALSE(file.path().empty());
    EXPECT_FALSE(loader.load(file.path(), error));
    EXPECT_NE(error.find("empty"), std::string::npos);
    const std::string junk = "not a serialized TensorRT plan";
    file.write(junk.data(), junk.size());
    EXPECT_FALSE(loader.load(file.path(), error));
    EXPECT_NE(error.find("incompatible"), std::string::npos);
    EXPECT_FALSE(loader.loaded());
    EXPECT_TRUE(loader.tensors().empty());
    EXPECT_EQ(loader.stream(), nullptr);
}

TEST(TensorRtEngine, LoadsSerializedEngineAndOwnsBuffersAndStream) {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        GTEST_SKIP() << "CUDA device unavailable";
    }

    std::ostringstream logs;
    skai::Logger logger(logs);
    skai::TensorRtBootstrap bootstrap(logger);
    std::string error;
    ASSERT_TRUE(bootstrap.initialize_standard_plugins(error)) << error;

    QuietLogger trt_logger;
    std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(trt_logger));
    ASSERT_NE(builder, nullptr);
    std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
    std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
    ASSERT_NE(network, nullptr);
    ASSERT_NE(config, nullptr);
    auto* images = network->addInput("images", nvinfer1::DataType::kFLOAT,
                                     nvinfer1::Dims4{1, 3, 2, 2});
    ASSERT_NE(images, nullptr);
    auto* identity = network->addIdentity(*images);
    ASSERT_NE(identity, nullptr);
    identity->getOutput(0)->setName("detections");
    network->markOutput(*identity->getOutput(0));
    std::unique_ptr<nvinfer1::IHostMemory> serialized(
        builder->buildSerializedNetwork(*network, *config));
    ASSERT_NE(serialized, nullptr) << "tiny TensorRT engine build failed";

    TemporaryEngine file;
    ASSERT_FALSE(file.path().empty());
    file.write(serialized->data(), serialized->size());
    skai::TensorRtEngine loader(logger);
    ASSERT_TRUE(loader.load(file.path(), error)) << error;
    EXPECT_TRUE(loader.loaded());
    EXPECT_NE(loader.native_engine(), nullptr);
    EXPECT_NE(loader.stream(), nullptr);
    EXPECT_FALSE(loader.tensorrt_version().empty());
    ASSERT_EQ(loader.tensors().size(), 2U);
    EXPECT_EQ(loader.tensors()[0].name, "images");
    EXPECT_EQ(loader.tensors()[0].bytes, 48U);
    EXPECT_EQ(loader.tensors()[0].data_type, "FP32");
    EXPECT_NE(loader.device_buffer("images"), nullptr);
    EXPECT_NE(loader.device_buffer("detections"), nullptr);
    EXPECT_EQ(loader.device_buffer("unknown"), nullptr);
    loader.unload();
    EXPECT_FALSE(loader.loaded());
    EXPECT_EQ(loader.stream(), nullptr);
    EXPECT_TRUE(loader.tensors().empty());
    ASSERT_TRUE(loader.load(file.path(), error)) << error;
}
