#pragma once

#include "AudioAnalyzer.hpp"
#include "MetadataWriter.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

enum class DownloadState {
    Idle,
    Running,
    Done,
    Error
};

struct DownloadSnapshot {
    DownloadState state = DownloadState::Idle;
    std::string message;
    std::string detail;
    std::string filePath;
    float progress = 0.0f;
};

class DownloadManager {
public:
    using FinishedCallback = std::function<void()>;

    DownloadManager() = default;
    ~DownloadManager();

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    bool start(const std::string& source,
               const std::string& outputDirectory,
               const std::string& format,
               std::vector<std::string> cookiesFromBrowser = {},
               FinishedCallback on_finished = {});
    DownloadSnapshot snapshot() const;

private:
    struct DownloadRequest {
        std::string source;
        std::string outputDirectory;
        std::string format;
        std::vector<std::string> cookiesFromBrowser;
        FinishedCallback onFinished;
    };

    void run(DownloadRequest request);
    void setState(DownloadState state,
                  std::string message,
                  std::string detail = {},
                  std::string filePath = {},
                  float progress = 0.0f);

    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic_bool running_{false};
    DownloadSnapshot snapshot_;
    AudioAnalyzer analyzer_;
    MetadataWriter metadataWriter_;
};
