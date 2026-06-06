#include "DownloadManager.hpp"

#include "ProcessRunner.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

class CompletionGuard {
public:
    CompletionGuard(std::atomic_bool& running,
                    DownloadManager::FinishedCallback& onFinished)
        : running_(running), onFinished_(onFinished)
    {
    }

    ~CompletionGuard()
    {
        running_ = false;
        if (onFinished_) {
            onFinished_();
        }
    }

    CompletionGuard(const CompletionGuard&) = delete;
    CompletionGuard& operator=(const CompletionGuard&) = delete;

private:
    std::atomic_bool& running_;
    DownloadManager::FinishedCallback& onFinished_;
};

bool validSource(const std::string& value)
{
    return !value.empty() && value.find('\0') == std::string::npos;
}

std::string normalizedFormat(std::string format)
{
    std::transform(format.begin(), format.end(), format.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    static constexpr std::string_view supported[] = {
        "aac", "alac", "flac", "m4a", "mp3", "opus", "vorbis", "wav",
    };
    for (const auto candidate : supported) {
        if (format == candidate) {
            return format;
        }
    }
    return "m4a";
}

std::string downloadedFilePath(const std::string& output)
{
    std::vector<std::string> lines;
    std::stringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        std::string candidate = ProcessRunner::trim(line);
        if (!candidate.empty()) {
            lines.push_back(std::move(candidate));
        }
    }

    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (fs::exists(*it)) {
            return *it;
        }
    }
    if (!lines.empty()) {
        return lines.back();
    }
    return {};
}

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

bool youtubeNeedsCookies(const std::string& output)
{
    std::string lower = lowerCopy(output);
    return lower.find("sign in to confirm") != std::string::npos ||
           lower.find("not a bot") != std::string::npos ||
           lower.find("use --cookies-from-browser") != std::string::npos ||
           lower.find("--cookies for the authentication") != std::string::npos;
}

std::string compactError(std::string output)
{
    output = ProcessRunner::trim(std::move(output));
    if (output.empty()) {
        return "Download failed";
    }
    constexpr size_t max_length = 320;
    if (output.size() > max_length) {
        output = output.substr(output.size() - max_length);
        output = "..." + output;
    }
    return output;
}

std::vector<std::string> ytDlpArguments(const std::string& executable,
                                        const std::string& source,
                                        const std::string& outputDirectory,
                                        const std::string& format,
                                        const std::optional<std::string>& cookiesBrowser)
{
    fs::path output_template = fs::path(outputDirectory) / "%(title)s.%(ext)s";
    std::vector<std::string> args = {
        executable,
        "-f", "bestaudio",
        "--extract-audio",
        "--audio-format", format,
        "--embed-thumbnail",
        "--embed-metadata",
        "--replace-in-metadata", "title", "(?i)\\([^)]*official[^)]*\\)", "",
        "--replace-in-metadata", "title", "\\[.*?\\]", "",
        "--replace-in-metadata", "title", "\\s+", " ",
        "--replace-in-metadata", "title", "^\\s+|\\s+$", "",
        "--print", "after_move:filepath",
        "-o", output_template.string(),
    };
    if (cookiesBrowser && !cookiesBrowser->empty()) {
        args.push_back("--cookies-from-browser");
        args.push_back(*cookiesBrowser);
    }
    args.push_back("--");
    args.push_back(source);
    return args;
}

}  // namespace

DownloadManager::~DownloadManager()
{
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool DownloadManager::start(const std::string& source,
                            const std::string& outputDirectory,
                            const std::string& format,
                            std::vector<std::string> cookiesFromBrowser,
                            FinishedCallback on_finished)
{
    if (!validSource(source)) {
        setState(DownloadState::Error, "Error", "Download source is empty");
        return false;
    }

    if (!fs::is_directory(outputDirectory)) {
        setState(DownloadState::Error, "Error", "Current directory is not writable");
        return false;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        setState(DownloadState::Error, "Error", "Download already running");
        return false;
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    std::string selected_format = normalizedFormat(format);
    setState(DownloadState::Running,
             "Downloading",
             "To: " + outputDirectory + " | " + selected_format,
             {},
             0.2f);
    DownloadRequest request{
        source,
        outputDirectory,
        selected_format,
        std::move(cookiesFromBrowser),
        std::move(on_finished),
    };
    worker_ = std::thread(&DownloadManager::run, this, std::move(request));
    return true;
}

DownloadSnapshot DownloadManager::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void DownloadManager::run(DownloadRequest request)
{
    CompletionGuard completion(running_, request.onFinished);
    auto fail = [&](std::string detail,
                    std::string file_path = {},
                    float progress = 0.0f) {
        setState(DownloadState::Error, "Error", std::move(detail),
                 std::move(file_path), progress);
    };

    auto yt_dlp = ProcessRunner::findExecutable("yt-dlp");
    if (!yt_dlp) {
        fail("yt-dlp not found");
        return;
    }

    std::string output;
    std::string file_path;
    int exit_code = -1;
    bool retried_for_cookies = false;

    auto attempt = [&](const std::optional<std::string>& browser) {
        output.clear();
        if (browser && !browser->empty()) {
            setState(DownloadState::Running,
                     "Downloading",
                     "Trying cookies: " + *browser,
                     {},
                     0.25f);
        }
        exit_code = ProcessRunner::runWithCombinedOutput(
            ytDlpArguments(*yt_dlp, request.source, request.outputDirectory,
                           request.format, browser),
            &output);
        file_path = downloadedFilePath(output);
        return exit_code == 0 && !file_path.empty() && fs::exists(file_path);
    };

    if (!attempt(std::nullopt)) {
        if (youtubeNeedsCookies(output)) {
            for (const auto& browser : request.cookiesFromBrowser) {
                retried_for_cookies = true;
                if (attempt(browser)) {
                    break;
                }
            }
        }
    }

    if (exit_code != 0 || file_path.empty() || !fs::exists(file_path)) {
        if (youtubeNeedsCookies(output) && !retried_for_cookies) {
            fail("YouTube requires cookies. Add [ytdlp] cookies_from_browser = [\"chrome\", \"safari\"]");
        } else {
            fail(compactError(output));
        }
        return;
    }

    const std::string filename = fs::path(file_path).filename().string();
    setState(DownloadState::Running, "Analyzing", filename, file_path, 0.72f);
    AudioMetadata metadata = analyzer_.analyzeWithEssentia(file_path);
    if (!metadata.succeeded) {
        fail(metadata.error, file_path, 1.0f);
        return;
    }

    setState(DownloadState::Running, "Writing tags", filename, file_path, 0.9f);
    std::string error;
    if (!metadataWriter_.write(file_path, metadata, error)) {
        fail(error, file_path, 1.0f);
        return;
    }

    std::string detail = filename + " | " +
        std::to_string((int)metadata.bpm) + " BPM | " + metadata.key;
    setState(DownloadState::Done, "Success", detail, file_path, 1.0f);
}

void DownloadManager::setState(DownloadState state,
                               std::string message,
                               std::string detail,
                               std::string filePath,
                               float progress)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = state;
    snapshot_.message = std::move(message);
    snapshot_.detail = std::move(detail);
    snapshot_.filePath = std::move(filePath);
    snapshot_.progress = std::clamp(progress, 0.0f, 1.0f);
}
