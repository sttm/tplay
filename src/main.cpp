#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <cstdio>
#include <regex>
#include <thread>
#include <unordered_map>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "core/AppController.hpp"
#include "core/Config.hpp"
#include "core/ProcessRunner.hpp"
#include "Library/LibraryDatabase.h"
#include "Serato/SeratoCueWriter.h"
#include "Telegram/TelegramBotClient.h"
#include "Telegram/TelegramInboxService.h"
#include "Telegram/TelegramRepository.h"
#include "Traktor/TraktorMetadataWriter.h"
#include "ui/BrowserState.hpp"
#include "ui/KeyBindings.hpp"
#include "ui/ManualCueEditor.hpp"
#include "ui/TrimEditor.hpp"

using namespace ftxui;
namespace fs = std::filesystem;

std::string formatTime(double seconds)
{
    if (seconds <= 0.0) {
        return "--:--";
    }

    int total = (int)seconds;
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

std::string formatPlaybackTime(double seconds)
{
    int total = std::max(0, (int)seconds);
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

std::string formatBitrate(double kbps)
{
    return kbps > 0.0 ? std::to_string((int)(kbps + 0.5)) + "k" : "-";
}

std::string formatSampleRate(double hz)
{
    if (hz <= 0.0) {
        return "-";
    }
    double khz = hz / 1000.0;
    if (std::abs(khz - std::round(khz)) < 0.05) {
        return std::to_string((int)std::round(khz)) + "k";
    }
    return std::format("{:.1f}k", khz);
}

std::string formatSize(std::uintmax_t bytes)
{
    if (bytes == 0) {
        return "-";
    }
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        return std::format("{:.1f}G", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
    if (bytes >= 1024ULL * 1024ULL) {
        return std::format("{:.1f}M", (double)bytes / (1024.0 * 1024.0));
    }
    if (bytes >= 1024ULL) {
        return std::format("{:.0f}K", (double)bytes / 1024.0);
    }
    return std::to_string(bytes) + "B";
}

std::string playbackStateToString(PlaybackState state)
{
    switch (state)
    {
    case PlaybackState::Stopped:
        return "⏹";
    case PlaybackState::Playing:
        return "▶";
    case PlaybackState::Paused:
        return "⏸";
    case PlaybackState::Error:
        return "!";
    }
    return "unknown";
}

std::string playbackModeToString(PlaybackMode mode)
{
    switch (mode)
    {
    case PlaybackMode::Shuffle:
        return "⇄";
    case PlaybackMode::RepeatOne:
        return "↻1";
    case PlaybackMode::RepeatAll:
        return "↻A";
    }
    return "↻A";
}

std::string truncateEnd(const std::string& value, int width)
{
    if (width <= 0) {
        return "";
    }
    if ((int)value.size() <= width) {
        return value;
    }
    if (width <= 3) {
        return value.substr(0, width);
    }
    return value.substr(0, width - 3) + "...";
}

std::vector<std::string> wrapText(const std::string& value, int width)
{
    std::vector<std::string> lines;
    if (width <= 0) {
        return lines;
    }

    auto flushParagraph = [&](std::string paragraph) {
        size_t first = paragraph.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            paragraph.clear();
        } else {
            size_t last = paragraph.find_last_not_of(" \t\r");
            paragraph = paragraph.substr(first, last - first + 1);
        }
        if (paragraph.empty()) {
            lines.emplace_back("");
            return;
        }

        std::string current;
        size_t start = 0;
        while (start < paragraph.size()) {
            while (start < paragraph.size() && paragraph[start] == ' ') {
                start++;
            }
            size_t end = paragraph.find(' ', start);
            std::string word = paragraph.substr(
                start,
                end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? paragraph.size() : end + 1;

            while ((int)word.size() > width) {
                if (!current.empty()) {
                    lines.push_back(current);
                    current.clear();
                }
                lines.push_back(word.substr(0, width));
                word.erase(0, width);
            }

            if (word.empty()) {
                continue;
            }
            if (current.empty()) {
                current = std::move(word);
            } else if ((int)(current.size() + 1 + word.size()) <= width) {
                current += " " + word;
            } else {
                lines.push_back(current);
                current = std::move(word);
            }
        }

        if (!current.empty()) {
            lines.push_back(current);
        }
    };

    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('\n', start);
        flushParagraph(value.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (lines.empty()) {
        lines.emplace_back("");
    }
    return lines;
}

std::optional<std::string> firstUrlInText(const std::string& value)
{
    size_t start = value.find("https://");
    if (start == std::string::npos) {
        start = value.find("http://");
    }
    if (start == std::string::npos) {
        return std::nullopt;
    }

    size_t end = start;
    while (end < value.size()) {
        char c = value[end];
        if (std::isspace((unsigned char)c) ||
            c == '"' || c == '\'' || c == '<' || c == '>') {
            break;
        }
        end++;
    }

    std::string url = value.substr(start, end - start);
    while (!url.empty() &&
           (url.back() == '.' || url.back() == ',' || url.back() == ';' ||
            url.back() == ':' || url.back() == ')' || url.back() == ']')) {
        url.pop_back();
    }
    if (url.empty()) {
        return std::nullopt;
    }
    return url;
}

bool looksLikeUrl(const std::string& value)
{
    return value.starts_with("http://") || value.starts_with("https://");
}

std::string trimInput(std::string value)
{
    size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

std::string downloadSourceFromInput(const std::string& input)
{
    if (looksLikeUrl(input)) {
        return input;
    }
    const std::string prefix = "download ";
    if (input.starts_with(prefix)) {
        return trimInput(input.substr(prefix.size()));
    }
    return "";
}

bool isPlaylistSource(const std::string& source)
{
    std::string normalized = source;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return normalized.find("list=") != std::string::npos ||
           normalized.find("/sets/") != std::string::npos;
}

std::optional<std::string> youtubeSingleVideoUrl(const std::string& source)
{
    std::smatch match;
    static const std::regex watch_regex(
        R"(^(https?://(?:www\.)?youtube\.com/watch\?)([^#]*?)(?:&|^)list=[^&#]+.*$)",
        std::regex::icase);
    if (std::regex_match(source, match, watch_regex)) {
        std::string query = match[2].str();
        std::smatch video;
        static const std::regex v_regex(R"((?:^|&)v=([^&#]+))",
                                        std::regex::icase);
        if (std::regex_search(query, video, v_regex)) {
            return std::string("https://www.youtube.com/watch?v=") + video[1].str();
        }
    }

    static const std::regex short_regex(
        R"(^https?://(?:www\.)?youtu\.be/([^?&#/]+)(?:[?&].*)?$)",
        std::regex::icase);
    if (std::regex_match(source, match, short_regex)) {
        return std::string("https://youtu.be/") + match[1].str();
    }
    return std::nullopt;
}

bool copyToClipboard(const std::string& value)
{
#ifdef __APPLE__
    FILE* pipe = popen("pbcopy", "w");
    if (pipe == nullptr) {
        return false;
    }
    fwrite(value.data(), 1, value.size(), pipe);
    return pclose(pipe) == 0;
#else
    (void)value;
    return false;
#endif
}

std::optional<std::string> readClipboard()
{
#ifdef __APPLE__
    FILE* pipe = popen("pbpaste", "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }
    std::string value;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr) {
        value += buffer.data();
    }
    if (pclose(pipe) != 0) {
        return std::nullopt;
    }
    return value;
#else
    return std::nullopt;
#endif
}

fs::path activeConfigPath()
{
    fs::path config_path = fs::path(ProcessRunner::executableDirectory()) / "config.toml";
    std::error_code ec;
    if (!fs::is_regular_file(config_path, ec)) {
        config_path = fs::current_path() / "config.toml";
    }
    return config_path;
}

int handleTelegramCli(int argc, char** argv)
{
    auto print_help = [] {
        std::cout
            << "Usage:\n"
            << "  tplay telegram sync\n"
            << "  tplay telegram refresh\n"
            << "  tplay telegram list-chats\n"
            << "  tplay telegram list-items <chat_id>\n";
    };

    if (argc < 3) {
        print_help();
        return 1;
    }

    std::string subcommand = argv[2] != nullptr ? argv[2] : "";
    Config config;
    try {
        config = Config::load(activeConfigPath().string());
    } catch (const std::exception& ex) {
        std::cerr << "Config error: " << ex.what() << "\n";
        return 1;
    }

    LibraryDatabase database;
    std::string error;
    if (!database.open(config.library.databasePath, error) ||
        !database.initialize(error)) {
        std::cerr << "Library database error: " << error << "\n";
        return 1;
    }

    TelegramBotClient client(config.telegram.botToken);
    TelegramRepository repository(database);
    TelegramInboxService service(config, client, repository);

    if (subcommand == "sync" || subcommand == "refresh") {
        TelegramSyncSummary summary;
        if (!service.sync(summary, error)) {
            std::cerr << "Telegram sync error: " << error << "\n";
            return 1;
        }
        std::cout << "Telegram sync: updates " << summary.updates
                  << " | chats " << summary.chats
                  << " | audio " << summary.audioItems
                  << " | skipped " << summary.skipped
                  << " | last_update_id " << summary.lastUpdateId << "\n";
        return 0;
    }

    if (subcommand == "list-chats") {
        auto chats = service.listChats(error);
        if (!error.empty()) {
            std::cerr << "Telegram list error: " << error << "\n";
            return 1;
        }
        for (const auto& chat : chats) {
            std::cout << chat.title << " | " << chat.type
                      << " | " << chat.chatId
                      << " | " << chat.folderPath.string() << "\n";
        }
        if (chats.empty()) {
            std::cout << "No Telegram chats found. Run: tplay telegram sync\n";
        }
        return 0;
    }

    if (subcommand == "list-items") {
        if (argc < 4) {
            std::cerr << "Usage: tplay telegram list-items <chat_id>\n";
            return 1;
        }
        std::string chat_id = argv[3] != nullptr ? argv[3] : "";
        auto items = service.listAudioItems(chat_id, error);
        if (!error.empty()) {
            std::cerr << "Telegram list error: " << error << "\n";
            return 1;
        }
        for (const auto& item : items) {
            std::cout << (item.downloaded ? "downloaded" : "not downloaded")
                      << " | " << item.fileName
                      << " | " << item.title
                      << " | " << item.fileSize
                      << " | " << item.localPath.string() << "\n";
        }
        if (items.empty()) {
            std::cout << "No Telegram audio items found for chat " << chat_id << "\n";
        }
        return 0;
    }

    print_help();
    return 1;
}

std::string displayName(const std::string& path)
{
    if (path == "telegram://root") {
        return "Telegram";
    }
    if (path.rfind("telegram://chat/", 0) == 0) {
        return path.substr(std::string("telegram://chat/").size());
    }
    fs::path p(path);
    std::string name = p.filename().string();
    return name.empty() ? path : name;
}

enum class TrackSortColumn {
    Title,
    Time,
    Bpm,
    Key,
    Genre,
    Bitrate,
    SampleRate,
    Size,
};

int trackColumnWidth(const std::string& column)
{
    if (column == "time") return 5;
    if (column == "bpm") return 4;
    if (column == "key") return 4;
    if (column == "kbps") return 5;
    if (column == "rate") return 5;
    if (column == "size") return 6;
    if (column == "genre") return 7;
    return 0;
}

std::string trackColumnLabel(const std::string& column)
{
    if (column == "time") return "Time";
    if (column == "bpm") return "BPM";
    if (column == "key") return "Key";
    if (column == "kbps") return "Kbps";
    if (column == "rate") return "Rate";
    if (column == "size") return "Size";
    if (column == "genre") return "Genre";
    return column;
}

TrackSortColumn trackColumnSort(const std::string& column)
{
    if (column == "time") return TrackSortColumn::Time;
    if (column == "bpm") return TrackSortColumn::Bpm;
    if (column == "key") return TrackSortColumn::Key;
    if (column == "genre") return TrackSortColumn::Genre;
    if (column == "kbps") return TrackSortColumn::Bitrate;
    if (column == "rate") return TrackSortColumn::SampleRate;
    if (column == "size") return TrackSortColumn::Size;
    return TrackSortColumn::Title;
}

bool trackColumnEnabled(const ColumnVisibility& columns, const std::string& column)
{
    if (column == "time") return columns.time;
    if (column == "bpm") return columns.bpm;
    if (column == "key") return columns.key;
    if (column == "kbps") return columns.kbps;
    if (column == "rate") return columns.rate;
    if (column == "size") return columns.size;
    if (column == "genre") return columns.genre;
    return false;
}

Element trackRow(const std::string& title,
                 const std::string& time,
                 const std::string& bpm,
                 const std::string& key,
                 const std::string& bitrate,
                 const std::string& sample_rate,
                 const std::string& file_size,
                 const std::string& genre,
                 const std::vector<std::string>& column_order,
                 const std::string& marker = "  ")
{
    Elements columns = {
        text(marker) | size(WIDTH, EQUAL, 2),
        text(title) | flex,
    };
    for (const auto& column : column_order) {
        std::string value = "-";
        if (column == "time") value = time;
        else if (column == "bpm") value = bpm;
        else if (column == "key") value = key;
        else if (column == "kbps") value = bitrate;
        else if (column == "rate") value = sample_rate;
        else if (column == "size") value = file_size;
        else if (column == "genre") value = genre;
        int width = trackColumnWidth(column);
        if (width <= 0) {
            continue;
        }
        columns.push_back(separator());
        columns.push_back(text(value) | size(WIDTH, EQUAL, width));
    }
    return hbox(columns);
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

void sortTracks(std::vector<Track>& tracks,
                TrackSortColumn column,
                bool ascending)
{
    auto compareText = [](const std::string& first, const std::string& second) {
        std::string left = lowercase(first);
        std::string right = lowercase(second);
        if (left == right) {
            return 0;
        }
        return left < right ? -1 : 1;
    };
    auto compareNumber = [](double first, double second) {
        if (first == second) {
            return 0;
        }
        return first < second ? -1 : 1;
    };

    std::stable_sort(tracks.begin(), tracks.end(),
                     [&](const Track& first, const Track& second) {
        bool first_has_value = true;
        bool second_has_value = true;
        int result = 0;
        switch (column) {
        case TrackSortColumn::Title:
            result = compareText(first.title, second.title);
            break;
        case TrackSortColumn::Time:
            first_has_value = first.duration > 0.0;
            second_has_value = second.duration > 0.0;
            result = compareNumber(first.duration, second.duration);
            break;
        case TrackSortColumn::Bpm:
            first_has_value = first.bpm > 0.0;
            second_has_value = second.bpm > 0.0;
            result = compareNumber(first.bpm, second.bpm);
            break;
        case TrackSortColumn::Key:
            first_has_value = !first.key.empty();
            second_has_value = !second.key.empty();
            result = compareText(first.key, second.key);
            break;
        case TrackSortColumn::Genre:
            first_has_value = !first.genre.empty();
            second_has_value = !second.genre.empty();
            result = compareText(first.genre, second.genre);
            break;
        case TrackSortColumn::Bitrate:
            first_has_value = first.bitrateKbps > 0.0;
            second_has_value = second.bitrateKbps > 0.0;
            result = compareNumber(first.bitrateKbps, second.bitrateKbps);
            break;
        case TrackSortColumn::SampleRate:
            first_has_value = first.sampleRateHz > 0.0;
            second_has_value = second.sampleRateHz > 0.0;
            result = compareNumber(first.sampleRateHz, second.sampleRateHz);
            break;
        case TrackSortColumn::Size:
            first_has_value = first.sizeBytes > 0;
            second_has_value = second.sizeBytes > 0;
            result = compareNumber((double)first.sizeBytes, (double)second.sizeBytes);
            break;
        }

        if (first_has_value != second_has_value) {
            return first_has_value;
        }
        if (result == 0) {
            result = compareText(first.title, second.title);
        }
        return ascending ? result < 0 : result > 0;
    });
}

enum class HoverControl {
    None,
    VolumeDown,
    VolumeUp,
    Previous,
    PlayPause,
    Next,
    Mode,
    SpeedReset,
};

enum class PendingConfirmation {
    None,
    PlaylistDownload,
    StemSeparation,
    Normalization,
    Convert,
    AutoCue,
    Quit,
    DeleteEntry,
};

bool keyMatches(const Event& event, std::initializer_list<std::string> keys)
{
    return ui::keyMatches(event, keys);
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1] != nullptr ? argv[1] : "") == "telegram") {
        return handleTelegramCli(argc, argv);
    }

    if (argc > 1) {
        AppController controller;
        std::string error;
        std::string command = argv[1] != nullptr ? argv[1] : "";
        if (command == "--export-library") {
            if (controller.exportLibrary(error, true)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Export error: " << error << "\n";
            return 1;
        }
        if (command == "--import-library-json") {
            if (controller.importChangedJson(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Import error: " << error << "\n";
            return 1;
        }
        if (command == "--import-serato-cues") {
            if (controller.importSeratoCues(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Serato import error: " << error << "\n";
            return 1;
        }
        if (command == "--validate-library-export") {
            if (controller.validateLibraryExport(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Validation error: " << error << "\n";
            return 1;
        }
        if (command == "--validate-serato-cues") {
            if (controller.validateSeratoCues(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Serato validation error: " << error << "\n";
            return 1;
        }
        if (command == "--validate-traktor-tags") {
            if (controller.validateTraktorEmbeddedCues(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Traktor embedded validation error: " << error << "\n";
            return 1;
        }
        if (command == "--dump-traktor-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tplay --dump-traktor-cues <audio-file>\n";
                return 1;
            }
            TraktorMetadataWriter traktor;
            fs::path file = argv[2];
            auto status = traktor.inspect(file, error);
            std::cout << "file=" << file << "\n"
                      << "supported=" << (status.supportedContainer ? "yes" : "no") << "\n"
                      << "has_traktor4=" << (status.hasTraktor4Tag ? "yes" : "no") << "\n"
                      << "tag_size=" << status.tagSize << "\n"
                      << "detail=" << status.detail << "\n";
            if (!error.empty()) {
                std::cerr << "inspect_error=" << error << "\n";
                return 1;
            }
            std::string read_error;
            auto cues = traktor.readCues(file, read_error);
            if (!read_error.empty()) {
                std::cerr << "read_error=" << read_error << "\n";
                return 1;
            }
            std::cout << "cue_count=" << cues.size() << "\n";
            for (const auto& cue : cues) {
                std::cout << cue.index << "|"
                          << cue.name << "|"
                          << std::format("{:.3f}", cue.seconds) << "\n";
            }
            return 0;
        }
        if (command == "--dump-serato-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tplay --dump-serato-cues <audio-file>\n";
                return 1;
            }
            SeratoCueWriter serato;
            fs::path file = argv[2];
            std::string read_error;
            auto cues = serato.readCues(file, read_error);
            std::cout << "file=" << file << "\n";
            if (!read_error.empty()) {
                std::cerr << "read_error=" << read_error << "\n";
                return 1;
            }
            std::cout << "cue_count=" << cues.size() << "\n";
            for (const auto& cue : cues) {
                std::cout << cue.index << "|"
                          << cue.name << "|"
                          << std::format("{:.3f}", cue.seconds) << "|"
                          << std::format("{:06x}", cue.colorRgb & 0xffffffu)
                          << "\n";
            }
            return 0;
        }
        if (command == "--make-serato-cue-test") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tplay --make-serato-cue-test <clean-input> <output>\n";
                return 1;
            }
            fs::path clean_input = argv[2];
            fs::path output = argv[3];
            if (!fs::is_regular_file(clean_input)) {
                std::cerr << "Clean input file does not exist\n";
                return 1;
            }
            std::error_code ec;
            fs::create_directories(output.parent_path(), ec);
            if (ec) {
                std::cerr << "Could not create output directory: " << ec.message() << "\n";
                return 1;
            }
            fs::copy_file(clean_input, output, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "Could not copy input to output: " << ec.message() << "\n";
                return 1;
            }
            SeratoCueWriter serato;
            std::vector<SeratoCue> cues = {
                {0, "START", 5.0, 0x00ff00u},
                {1, "DROP 1", 15.0, 0x0080ffu},
                {2, "DROP 2", 25.0, 0xff8000u},
            };
            std::string write_error;
            if (!serato.writeCues(output, cues, write_error, false, true)) {
                std::cerr << "Serato write error: " << write_error << "\n";
                return 1;
            }
            std::cout << "Generated Serato cue test file: " << output << "\n"
                      << "Cues: 5.000s, 15.000s, 25.000s\n";
            return 0;
        }
        if (command == "--clear-serato-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tplay --clear-serato-cues <audio-file>\n";
                return 1;
            }
            SeratoCueWriter serato;
            std::string write_error;
            if (!serato.writeCues(argv[2], {}, write_error, false, true)) {
                std::cerr << "Serato clear error: " << write_error << "\n";
                return 1;
            }
            std::cout << "Cleared Serato cues: " << argv[2] << "\n";
            return 0;
        }
        if (command == "--clear-traktor-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tplay --clear-traktor-cues <audio-file>\n";
                return 1;
            }
            LibraryTrack track;
            track.id = argv[2];
            track.path = argv[2];
            track.title = fs::path(argv[2]).stem().string();
            TraktorMetadataWriter traktor;
            std::string write_error;
            if (!traktor.writeCues(track, write_error)) {
                std::cerr << "Traktor clear error: " << write_error << "\n";
                return 1;
            }
            std::cout << "Cleared Traktor cues: " << argv[2] << "\n";
            return 0;
        }
        if (command == "--sync-dual-cues") {
            if (argc < 3 || argv[2] == nullptr) {
                std::cerr << "Usage: tplay --sync-dual-cues <audio-file>\n";
                return 1;
            }
            fs::path file = argv[2];
            SeratoCueWriter serato;
            TraktorMetadataWriter traktor;

            std::string serato_error;
            std::vector<SeratoCue> cues = serato.readCues(file, serato_error);
            std::string source = cues.empty() ? std::string() : std::string("Serato");

            if (cues.empty()) {
                std::string traktor_error;
                cues = traktor.readCues(file, traktor_error);
                if (!cues.empty()) {
                    source = "Traktor";
                }
            }

            if (cues.empty()) {
                std::cerr << "No embedded cues found to sync\n";
                return 1;
            }

            std::string write_error;
            if (!serato.writeCues(file, cues, write_error, false, true)) {
                std::cerr << "Serato sync error: " << write_error << "\n";
                return 1;
            }

            LibraryTrack track;
            track.id = file.string();
            track.path = file;
            track.title = file.stem().string();
            for (const auto& cue : cues) {
                track.cues.push_back({
                    cue.index,
                    cue.name,
                    "hotcue",
                    cue.seconds,
                    std::format("#{:06x}", cue.colorRgb & 0xffffffu),
                });
            }
            if (!traktor.writeCues(track, write_error)) {
                std::cerr << "Traktor sync error: " << write_error << "\n";
                return 1;
            }

            std::cout << "Synced " << cues.size()
                      << " cues from " << source
                      << " into Serato + Traktor metadata: " << file << "\n";
            return 0;
        }
        if (command == "--make-traktor-one-cue-test") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tplay --make-traktor-one-cue-test <clean-input> <output>\n";
                return 1;
            }
            fs::path clean_input = argv[2];
            fs::path output = argv[3];
            if (!fs::is_regular_file(clean_input)) {
                std::cerr << "Clean input file does not exist\n";
                return 1;
            }
            LibraryTrack track;
            track.id = output.string();
            track.path = output;
            track.title = output.stem().string();
            track.cues.push_back({0, "CUE 1", "hotcue", 5.0, "#00ff00"});
            std::error_code ec;
            fs::create_directories(output.parent_path(), ec);
            if (ec) {
                std::cerr << "Could not create output directory: " << ec.message() << "\n";
                return 1;
            }
            fs::copy_file(clean_input, output, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "Could not copy input to output: " << ec.message() << "\n";
                return 1;
            }
            TraktorMetadataWriter traktor;
            std::string write_error;
            if (!traktor.writeCues(track, write_error)) {
                std::cerr << "Traktor write error: " << write_error << "\n";
                return 1;
            }
            std::cout << "Generated Traktor one-cue test file: " << output << "\n"
                      << "Cue: 5.000s\n";
            return 0;
        }
        if (command == "--make-both-one-cue-test") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tplay --make-both-one-cue-test <clean-input> <output>\n";
                return 1;
            }
            fs::path clean_input = argv[2];
            fs::path output = argv[3];
            if (!fs::is_regular_file(clean_input)) {
                std::cerr << "Clean input file does not exist\n";
                return 1;
            }
            std::error_code ec;
            fs::create_directories(output.parent_path(), ec);
            if (ec) {
                std::cerr << "Could not create output directory: " << ec.message() << "\n";
                return 1;
            }
            fs::copy_file(clean_input, output, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "Could not copy input to output: " << ec.message() << "\n";
                return 1;
            }

            std::vector<SeratoCue> cues = {
                {0, "CUE 1", 5.0, 0x00ff00u},
            };
            SeratoCueWriter serato;
            std::string serato_error;
            if (!serato.writeCues(output, cues, serato_error, false, true)) {
                std::cerr << "Serato write error: " << serato_error << "\n";
                return 1;
            }

            LibraryTrack track;
            track.id = output.string();
            track.path = output;
            track.title = output.stem().string();
            track.cues.push_back({0, "CUE 1", "hotcue", 5.0, "#00ff00"});
            TraktorMetadataWriter traktor;
            std::string traktor_error;
            if (!traktor.writeCues(track, traktor_error)) {
                std::cerr << "Traktor write error: " << traktor_error << "\n";
                return 1;
            }
            std::cout << "Generated Serato+Traktor one-cue test file: " << output << "\n"
                      << "Cue: 5.000s\n";
            return 0;
        }
        if (command == "--make-traktor-cue-test") {
            if (argc < 4 || argv[2] == nullptr || argv[3] == nullptr) {
                std::cerr << "Usage: tplay --make-traktor-cue-test <clean-input.mp3> <output.mp3> [template.mp3]\n";
                return 1;
            }
            fs::path clean_input = argv[2];
            fs::path output = argv[3];
            fs::path template_file = argc >= 5 && argv[4] != nullptr
                ? fs::path(argv[4])
                : fs::path();
            TraktorMetadataWriter traktor;
            std::vector<SeratoCue> cues = {
                {0, "n.n.", 5.0, 0xffffffu},
                {1, "n.n.", 15.0, 0xffffffu},
                {2, "n.n.", 25.0, 0xffffffu},
            };
            if (!traktor.makeCueTemplateTestFile(clean_input, template_file,
                                                 output, cues, error)) {
                std::cerr << "Traktor cue test generation error: " << error << "\n";
                return 1;
            }
            std::cout << "Generated Traktor cue test file: " << output << "\n"
                      << "Template: "
                      << (template_file.empty() ? std::string("embedded") : template_file.string())
                      << "\n"
                      << "Cues: 5.000s, 15.000s, 25.000s\n";
            return 0;
        }
        if (command == "--library-status") {
            if (controller.libraryStatus(error)) {
                std::cout << error << "\n";
                return 0;
            }
            std::cerr << "Library status error: " << error << "\n";
            return 1;
        }
        if (command == "--help") {
            std::cout << "Usage: tplay [--library-status|--export-library|--import-library-json|--import-serato-cues|--validate-library-export|--validate-serato-cues|--validate-traktor-tags|--dump-traktor-cues <audio-file>|--make-traktor-cue-test <clean-input.mp3> <output.mp3> [template.mp3]]\n"
                      << "       tplay telegram [sync|refresh|list-chats|list-items <chat_id>]\n";
            return 0;
        }
        std::cerr << "Unknown option: " << command << "\n";
        return 1;
    }

    std::cout << "\033[8;30;110t" << std::flush;

    AppController controller;
    BrowserState state;
    auto screen = ScreenInteractive::FullscreenAlternateScreen();
    std::atomic_bool refresh_running = true;

    std::vector<std::string> dir_entries;
    std::vector<std::string> dir_paths;
    std::vector<std::string> track_entries;
    std::vector<Track> visible_files;
    std::vector<std::string> visible_dir_entries;
    std::vector<std::string> visible_track_entries;

    int browser_selector = 0;
    int root_selector = 0;
    int bottom_selector = 0;
    int speed_focus = 1;
    int progress_value = 0;
    int progress_min = 0;
    int progress_max = 1000;
    int progress_step = 5;
    bool progress_dragging = false;
    int speed_value = (int)std::round(controller.playbackRate() * 100.0);
    int speed_min = 50;
    int speed_max = 200;
    int speed_step = 1;
    bool preserve_pitch = controller.preservePitch();
    bool show_info = false;
    bool show_activity = true;
    bool show_download_panel = true;
    bool show_stems_panel = true;
    bool show_audio_process_panel = true;
    bool show_auto_cue_panel = true;
    bool show_metadata_popup = false;
    bool show_eq_popup = false;
    bool show_time_column = controller.config().columns.time;
    bool show_bpm_column = controller.config().columns.bpm;
    bool show_key_column = controller.config().columns.key;
    bool show_kbps_column = controller.config().columns.kbps;
    bool show_rate_column = controller.config().columns.rate;
    bool show_size_column = controller.config().columns.size;
    bool show_genre_column = controller.config().columns.genre;
    std::vector<std::string> visible_column_order;
    int visible_row_count = 1;
    int dir_view_selected = 0;
    int track_view_selected = 0;
    int last_click_pane = -1;
    int last_click_item = -1;
    auto last_click_time = std::chrono::steady_clock::time_point::min();
    HoverControl hovered_control = HoverControl::None;
    TrackSortColumn track_sort_column = TrackSortColumn::Title;
    bool track_sort_ascending = true;
    bool move_to_mode = false;
    Track move_track;
    std::string expanded_directory = controller.currentPath();
    std::string preferred_directory;
    std::string command_input;
    std::string command_status;
    std::string pending_playlist_source;
    std::string pending_playlist_destination;
    PendingConfirmation pending_confirmation = PendingConfirmation::None;
    int confirmation_selected = 1;
    int stem_option_row = 0;
    int stem_option_col = 0;
    int normalize_option_row = 0;
    int normalize_option_col = 1;
    int convert_option_row = 0;
    int convert_option_col = 1;
    int auto_cue_option_col = 0;
    int eq_selected = 0;
    int eq_low = 0;
    int eq_mid = 0;
    int eq_high = 0;
    int eq_min = -60;
    int eq_max = 6;
    int eq_step = 1;
    int pending_normalize_lufs = -14;
    std::string pending_normalize_mode = "Short-Term Max";
    bool pending_process_all_folder = false;
    std::string pending_convert_format = "mp3";
    int metadata_offset = 0;
    std::vector<std::pair<std::string, std::string>> metadata_details;
    std::string metadata_title;
    std::vector<Box> metadata_link_boxes;
    std::vector<std::string> metadata_link_urls;
    Track pending_stem_track;
    DemucsConfig pending_stem_config = controller.config().demucs;
    Track pending_process_track;
    Track pending_auto_cue_track;
    Track pending_delete_entry;
    std::string playing_track_id;
    Box directory_list_box;
    Box track_list_box;
    Box command_paste_box;
    Box command_clear_box;
    Box now_playing_box;
    Box volume_down_box;
    Box volume_up_box;
    Box previous_box;
    Box play_pause_box;
    Box next_box;
    Box mode_box;
    Box speed_reset_box;
    Box title_header_box;
    Box time_header_box;
    Box bpm_header_box;
    Box key_header_box;
    Box genre_header_box;
    Box bitrate_header_box;
    Box rate_header_box;
    Box size_header_box;
    Box confirm_yes_box;
    Box confirm_no_box;
    Box stem_2_box;
    Box stem_4_box;
    Box stem_mp3_box;
    Box stem_wav_box;
    Box stem_flac_box;
    Box normalize_16_box;
    Box normalize_14_box;
    Box normalize_9_box;
    Box normalize_one_box;
    Box normalize_all_box;

    auto columnReservedWidth = [](const std::vector<std::string>& columns) {
        int width = 2;
        for (const auto& column : columns) {
            int column_width = trackColumnWidth(column);
            if (column_width > 0) {
                width += 1 + column_width;
            }
        }
        return width;
    };

    auto configuredTrackColumns = [&] {
        std::vector<std::string> columns;
        const auto& config_columns = controller.config().columns;
        for (const auto& column : config_columns.order) {
            if (trackColumnEnabled(config_columns, column)) {
                columns.push_back(column);
            }
        }
        return columns;
    };

    auto fitTrackColumns = [&](int right_width) {
        auto columns = configuredTrackColumns();
        while (!columns.empty() &&
               right_width - columnReservedWidth(columns) < 5) {
            columns.pop_back();
        }
        return columns;
    };
    Box convert_wav_box;
    Box convert_mp3_box;
    Box convert_m4a_box;
    Box convert_flac_box;
    Box convert_one_box;
    Box convert_all_box;
    Box auto_cue_track_box;
    Box auto_cue_folder_box;
    Box auto_cue_cancel_box;
    Box metadata_close_box;
    Box metadata_list_box;
    Box eq_close_box;
    Box download_close_box;
    Box stems_close_box;
    Box audio_process_close_box;
    Box auto_cue_close_box;
    std::array<Box, 3> eq_reset_boxes{};
    std::atomic_bool manual_refresh_active = false;
    std::atomic_bool trim_refresh_active = false;
    std::atomic_bool editor_prepare_active = false;
    std::jthread editor_prepare_worker;
    ManualCueEditor manual_cue_editor(controller, command_status, manual_refresh_active);
    TrimEditor trim_editor(controller, command_status, trim_refresh_active);

    auto syncBrowserData = [&](int left_width, int right_width, int row_count) {
        auto tracks = controller.trackStore().getTracks();
        int dir_title_width = std::max(4, left_width - 2);
        int reserved_track_width = columnReservedWidth(visible_column_order);
        int track_title_width = std::max(5, right_width - reserved_track_width);

        dir_entries.clear();
        dir_paths.clear();
        track_entries.clear();
        std::string selected_track_id;
        if (state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            selected_track_id = visible_files[state.selectedTrack].id;
        }
        visible_files.clear();

        std::unordered_map<std::string, bool> seen_dirs;
        auto addDirectory = [&](const std::string& path, int depth, bool current) {
            if (path.empty() || seen_dirs[path]) {
                return;
            }
            seen_dirs[path] = true;

            std::string indent((size_t)std::max(0, depth) * 2, ' ');
            std::string connector = depth == 0 ? "" : (current ? "└ " : "├ ");
            std::string marker = current ? "▸ " : "  ";
            int available = std::max(4, dir_title_width - (int)indent.size() - (int)connector.size() - 2);
            dir_entries.push_back(indent + connector + marker + truncateEnd(displayName(path), available));
            dir_paths.push_back(path);
        };

        std::string current_path = controller.currentPath();
        std::string active_root;
        bool telegram_active = controller.isTelegramPath(expanded_directory) ||
                               controller.isTelegramPath(current_path);
        for (const auto& root : controller.config().musicDirectories) {
            std::error_code ec;
            fs::path relative = fs::relative(expanded_directory, root, ec);
            std::string relative_string = relative.string();
            if (!ec && !relative_string.starts_with("..")) {
                active_root = root;
                break;
            }
        }

        for (const auto& root : controller.config().musicDirectories) {
            bool is_active_root = root == active_root;
            addDirectory(root, 0, is_active_root && root == expanded_directory);
            if (!is_active_root) {
                continue;
            }

            int depth = 1;
            fs::path walk = root;
            std::error_code ec;
            fs::path relative = fs::relative(expanded_directory, root, ec);
            if (!ec) {
                for (const auto& part : relative) {
                    if (part == ".") {
                        continue;
                    }
                    walk /= part;
                    addDirectory(walk.string(), depth, walk.string() == expanded_directory);
                    depth++;
                }
            }

            if (expanded_directory == current_path) {
                for (const auto& track : tracks) {
                    if (track.type == EntryType::Directory) {
                        std::string indent((size_t)depth * 2, ' ');
                        int available = std::max(4, dir_title_width - (int)indent.size() - 4);
                        dir_entries.push_back(indent + "├   " + truncateEnd(track.title, available));
                        dir_paths.push_back(track.id);
                    }
                }
            }
        }

        if (controller.config().telegram.enabled) {
            std::string telegram_root = controller.telegramRootPath();
            addDirectory(telegram_root, 0, telegram_active && expanded_directory == telegram_root);
            if (telegram_active) {
                if (expanded_directory != telegram_root) {
                    addDirectory(expanded_directory, 1, true);
                }
                if (expanded_directory == current_path) {
                    int depth = expanded_directory == telegram_root ? 1 : 2;
                    for (const auto& track : tracks) {
                        if (track.type == EntryType::Directory) {
                            std::string indent((size_t)depth * 2, ' ');
                            int available = std::max(4, dir_title_width - (int)indent.size() - 4);
                            dir_entries.push_back(indent + "├   " + truncateEnd(track.title, available));
                            dir_paths.push_back(track.id);
                        }
                    }
                }
            }
        }

        for (const auto& track : tracks) {
            if (track.type == EntryType::File) {
                visible_files.push_back(track);
            }
        }
        sortTracks(visible_files, track_sort_column, track_sort_ascending);
        for (const auto& track : visible_files) {
            std::string label = track.status == TrackStatus::Downloading
                ? "[cloud] " + track.title
                : track.title;
            track_entries.push_back(truncateEnd(label, track_title_width));
        }

        if (dir_entries.empty()) {
            state.selectedDirectory = 0;
        } else {
            state.selectedDirectory = std::clamp(
                state.selectedDirectory,
                0,
                (int)dir_entries.size() - 1);

            if (!preferred_directory.empty()) {
                auto selected = std::find(dir_paths.begin(), dir_paths.end(), preferred_directory);
                if (selected != dir_paths.end()) {
                    state.selectedDirectory = (int)std::distance(dir_paths.begin(), selected);
                }
                preferred_directory.clear();
            }
        }

        if (track_entries.empty()) {
            state.selectedTrack = 0;
        } else {
            if (!selected_track_id.empty()) {
                auto selected = std::find_if(
                    visible_files.begin(), visible_files.end(),
                    [&](const Track& track) { return track.id == selected_track_id; });
                if (selected != visible_files.end()) {
                    state.selectedTrack =
                        (int)std::distance(visible_files.begin(), selected);
                }
            }
            state.selectedTrack = std::clamp(
                state.selectedTrack,
                0,
                (int)track_entries.size() - 1);
        }

        auto buildViewport = [&](const std::vector<std::string>& source,
                                 int selected,
                                 int& offset,
                                 int& view_selected,
                                 std::vector<std::string>& output) {
            int count = (int)source.size();
            int capacity = std::max(1, row_count);
            int max_offset = std::max(0, count - capacity);
            offset = std::clamp(offset, 0, max_offset);
            if (selected < offset) {
                offset = selected;
            } else if (selected >= offset + capacity) {
                offset = selected - capacity + 1;
            }
            offset = std::clamp(offset, 0, max_offset);
            view_selected = count == 0 ? 0 : selected - offset;
            int end = std::min(count, offset + capacity);
            output.assign(source.begin() + std::min(offset, count), source.begin() + end);
        };

        buildViewport(dir_entries, state.selectedDirectory, state.dirOffset,
                      dir_view_selected, visible_dir_entries);
        buildViewport(track_entries, state.selectedTrack, state.trackOffset,
                      track_view_selected, visible_track_entries);
    };

    auto openSelectedDirectory = [&] {
        if (state.selectedDirectory < 0 ||
            state.selectedDirectory >= (int)dir_paths.size()) {
            return;
        }

        const std::string selected_path = dir_paths[state.selectedDirectory];
        if (!controller.isTelegramPath(selected_path) &&
            !fs::is_directory(selected_path)) {
            return;
        }

        if (selected_path != controller.currentPath()) {
            controller.scanDirectory(selected_path);
        }

        expanded_directory = selected_path;
        preferred_directory = selected_path;
        state.selectedTrack = 0;
        state.focus = FocusPane::Directories;
        browser_selector = 0;
    };

    auto goToParentDirectory = [&] {
        const std::string current = controller.currentPath();
        if (current.empty()) {
            return;
        }
        if (controller.isTelegramPath(current)) {
            if (current == controller.telegramRootPath()) {
                expanded_directory.clear();
                preferred_directory = controller.telegramRootPath();
                return;
            }
            controller.scanDirectory(controller.telegramRootPath());
            expanded_directory = controller.telegramRootPath();
            preferred_directory = current;
            state.selectedTrack = 0;
            return;
        }

        std::string active_root;
        for (const auto& root : controller.config().musicDirectories) {
            std::error_code ec;
            std::string relative = fs::relative(current, root, ec).string();
            if (!ec && !relative.starts_with("..")) {
                active_root = root;
                break;
            }
        }

        if (active_root.empty() || fs::path(current) == fs::path(active_root)) {
            expanded_directory.clear();
            preferred_directory = active_root;
            return;
        }

        std::string parent = fs::path(current).parent_path().string();
        controller.scanDirectory(parent);
        expanded_directory = parent;
        preferred_directory = current;
        state.selectedTrack = 0;
    };

    auto playSelectedTrack = [&] {
        if (state.selectedTrack < 0 ||
            state.selectedTrack >= (int)visible_files.size()) {
            return;
        }

        if (controller.playTrack(visible_files[state.selectedTrack], visible_files)) {
            playing_track_id = visible_files[state.selectedTrack].id;
        }
    };

    auto finishKeyboardMove = [&] {
        move_to_mode = false;
        state.focus = FocusPane::Tracks;
        browser_selector = 1;
        root_selector = 0;
    };

    auto moveTrackToSelectedDirectory = [&] {
        if (!move_to_mode ||
            state.selectedDirectory < 0 ||
            state.selectedDirectory >= (int)dir_paths.size()) {
            return;
        }

        const std::string destination = dir_paths[state.selectedDirectory];
        std::string error;
        if (controller.moveTrack(move_track, destination, error)) {
            command_status = "Moved: " + move_track.title +
                " -> " + displayName(destination);
        } else {
            command_status = "Error: " + error;
        }
        finishKeyboardMove();
    };

    MenuOption dir_options = MenuOption::Vertical();
    dir_options.selected = &dir_view_selected;
    dir_options.focused_entry = &dir_view_selected;
    dir_options.entries = &visible_dir_entries;
    dir_options.on_change = [&] {
        state.selectedDirectory = state.dirOffset + dir_view_selected;
        state.focus = FocusPane::Directories;
        browser_selector = 0;
    };
    dir_options.on_enter = openSelectedDirectory;
    dir_options.entries_option.transform = [](const EntryState& entry) {
        Element row = text((entry.active ? "> " : "  ") + entry.label);
        if (entry.active) {
            row = row | bold;
        }
        return row;
    };

    MenuOption track_options = MenuOption::Vertical();
    track_options.selected = &track_view_selected;
    track_options.focused_entry = &track_view_selected;
    track_options.entries = &visible_track_entries;
    track_options.on_change = [&] {
        state.selectedTrack = state.trackOffset + track_view_selected;
        state.focus = FocusPane::Tracks;
        browser_selector = 1;
    };
    track_options.on_enter = playSelectedTrack;
    track_options.entries_option.transform = [&](const EntryState& entry) {
        std::string time = "--:--";
        std::string bpm = "0";
        std::string key = "-";
        std::string bitrate = "-";
        std::string sample_rate = "-";
        std::string size = "-";
        std::string genre = "-";

        int track_index = state.trackOffset + entry.index;
        if (track_index >= 0 && track_index < (int)visible_files.size()) {
            const auto& track = visible_files[track_index];
            time = formatTime(track.duration);
            bitrate = formatBitrate(track.bitrateKbps);
            sample_rate = formatSampleRate(track.sampleRateHz);
            size = formatSize(track.sizeBytes);
            genre = track.genre.empty() ? "-" : truncateEnd(track.genre, trackColumnWidth("genre"));
            if (track.status == TrackStatus::Analyzing) {
                bpm = "...";
                key = "...";
            } else {
                bpm = track.bpm > 0.0 ? std::to_string((int)track.bpm) : "-";
                key = track.key.empty() ? "-" : track.key;
            }
        }

        bool playing = track_index >= 0 &&
            track_index < (int)visible_files.size() &&
            visible_files[track_index].id == playing_track_id;
        std::string marker = playing ? "▶ " : (entry.active ? "> " : "  ");
        Element row = trackRow(entry.label, time, bpm, key, bitrate, sample_rate, size,
                               genre,
                               visible_column_order, marker);
        if (entry.active) {
            row = row | bold;
        }
        return row;
    };

    auto dir_menu = Menu(dir_options);
    auto track_menu = Menu(track_options);

    auto toggleTrackSort = [&](TrackSortColumn column) {
        if (track_sort_column == column) {
            track_sort_ascending = !track_sort_ascending;
        } else {
            track_sort_column = column;
            track_sort_ascending = true;
        }
        state.focus = FocusPane::Tracks;
        browser_selector = 1;
        root_selector = 0;
    };

    SliderOption<int> progress_options;
    progress_options.value = &progress_value;
    progress_options.min = &progress_min;
    progress_options.max = &progress_max;
    progress_options.increment = &progress_step;
    progress_options.on_change = [&] {
        progress_dragging = true;
        controller.seekPlayback((double)progress_value / (double)progress_max);
    };
    auto progress_slider = Slider(progress_options);

    SliderOption<int> speed_options;
    speed_options.value = &speed_value;
    speed_options.min = &speed_min;
    speed_options.max = &speed_max;
    speed_options.increment = &speed_step;
    speed_options.on_change = [&] {
        controller.setPlaybackRate((double)speed_value / 100.0);
    };
    auto speed_slider = Slider(speed_options);

    CheckboxOption pitch_lock_options = CheckboxOption::Simple();
    pitch_lock_options.label = "♩";
    pitch_lock_options.checked = &preserve_pitch;
    pitch_lock_options.on_change = [&] {
        controller.setPreservePitch(preserve_pitch);
    };
    auto pitch_lock_checkbox = Checkbox(pitch_lock_options);

    auto applyEq = [&] {
        controller.setEqualizerGains(eq_low, eq_mid, eq_high);
    };
    SliderOption<int> eq_low_options;
    eq_low_options.value = &eq_low;
    eq_low_options.min = &eq_min;
    eq_low_options.max = &eq_max;
    eq_low_options.increment = &eq_step;
    eq_low_options.on_change = applyEq;
    auto eq_low_slider = Slider(eq_low_options);

    SliderOption<int> eq_mid_options = eq_low_options;
    eq_mid_options.value = &eq_mid;
    auto eq_mid_slider = Slider(eq_mid_options);

    SliderOption<int> eq_high_options = eq_low_options;
    eq_high_options.value = &eq_high;
    auto eq_high_slider = Slider(eq_high_options);

    auto seekPlaybackBySeconds = [&](double delta_seconds) {
        auto playback = controller.playbackSnapshot();
        double duration = std::max(0.0, playback.durationSeconds);
        if (duration <= 0.0) {
            return false;
        }

        double new_seconds = std::clamp(
            playback.positionSeconds + delta_seconds,
            0.0,
            duration);
        progress_value = (int)std::round(
            (new_seconds / duration) * progress_max);
        progress_dragging = true;
        controller.seekPlayback(
            (double)progress_value / (double)progress_max);
        return true;
    };

    auto pasteClipboardToInput = [&] {
        auto value = readClipboard();
        if (!value) {
            command_status = "Clipboard unavailable";
            return false;
        }
        command_input = *value;
        while (!command_input.empty() &&
               (command_input.back() == '\n' || command_input.back() == '\r')) {
            command_input.pop_back();
        }
        root_selector = 2;
        bottom_selector = 0;
        command_status = command_input.empty()
            ? "Clipboard is empty"
            : "Pasted from clipboard";
        return true;
    };

    auto beginDownload = [&](const std::string& source,
                             const std::string& destination) {
        if (!controller.downloadToDirectory(source, destination, [&, destination] {
            screen.Post([&, destination] {
                if (controller.currentPath() == destination) {
                    controller.scanDirectory(destination, true);
                }
            });
        })) {
            command_status = controller.downloadSnapshot().detail;
            return false;
        }
        show_download_panel = true;
        command_status.clear();
        return true;
    };

    auto clearConfirmation = [&] {
        pending_confirmation = PendingConfirmation::None;
        confirmation_selected = 1;
        pending_playlist_source.clear();
        pending_playlist_destination.clear();
        pending_stem_track = Track{};
        pending_stem_config = controller.config().demucs;
        stem_option_row = 0;
        stem_option_col = 0;
        normalize_option_row = 0;
        normalize_option_col = 1;
        convert_option_row = 0;
        convert_option_col = 1;
        auto_cue_option_col = 0;
        pending_normalize_lufs = -14;
        pending_normalize_mode = "Short-Term Max";
        pending_process_all_folder = false;
        pending_convert_format = "mp3";
        pending_process_track = Track{};
        pending_auto_cue_track = Track{};
        pending_delete_entry = Track{};
    };

    auto rejectConfirmation = [&] {
        if (pending_confirmation == PendingConfirmation::PlaylistDownload) {
            std::string source = std::move(pending_playlist_source);
            std::string destination = std::move(pending_playlist_destination);
            clearConfirmation();
            if (auto single = youtubeSingleVideoUrl(source)) {
                command_status = "Downloading single video";
                beginDownload(*single, destination);
            } else {
                command_status = "Playlist download cancelled";
            }
            return;
        } else if (pending_confirmation == PendingConfirmation::StemSeparation) {
            command_status = "Demucs cancelled";
        } else if (pending_confirmation == PendingConfirmation::Normalization) {
            command_status = "Normalization cancelled";
        } else if (pending_confirmation == PendingConfirmation::Convert) {
            command_status = "Convert cancelled";
        } else if (pending_confirmation == PendingConfirmation::AutoCue) {
            command_status = "Auto Cue cancelled";
        } else if (pending_confirmation == PendingConfirmation::Quit) {
            command_status = "Quit cancelled";
        } else if (pending_confirmation == PendingConfirmation::DeleteEntry) {
            command_status = "Delete cancelled";
        }
        clearConfirmation();
    };

    auto acceptConfirmation = [&] {
        if (pending_confirmation == PendingConfirmation::PlaylistDownload) {
            std::string source = std::move(pending_playlist_source);
            std::string destination = std::move(pending_playlist_destination);
            clearConfirmation();
            beginDownload(source, destination);
            return true;
        }

        if (pending_confirmation == PendingConfirmation::StemSeparation) {
            Track track = pending_stem_track;
            DemucsConfig config = pending_stem_config;
            clearConfirmation();
            if (controller.separateTrack(track, config)) {
                show_activity = true;
                show_stems_panel = true;
                command_status = "Demucs started: " + track.title;
            } else {
                command_status = "Error: " +
                    controller.stemSeparationSnapshot().detail;
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::Normalization) {
            int lufs = pending_normalize_lufs;
            std::string mode = pending_normalize_mode;
            bool all_folder = pending_process_all_folder;
            Track track = pending_process_track;
            std::vector<Track> tracks = all_folder
                ? visible_files
                : std::vector<Track>{track};
            clearConfirmation();
            if (!all_folder && track.id.empty()) {
                command_status = "Select a track or choose all folder";
                return true;
            }
            if (controller.normalizeTracks(tracks, NormalizationOptions{lufs, mode})) {
                show_activity = true;
                show_audio_process_panel = true;
                std::string mode_suffix = mode.empty() ? "" : " " + mode;
                command_status = "Normalization started: " +
                    std::to_string(lufs) + " LUFS" + mode_suffix + " | " +
                    (all_folder ? "all folder" : track.title);
            } else {
                command_status = "Error: " +
                    controller.audioProcessSnapshot().detail;
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::Convert) {
            std::string format = pending_convert_format;
            bool all_folder = pending_process_all_folder;
            Track track = pending_process_track;
            std::vector<Track> tracks = all_folder
                ? visible_files
                : std::vector<Track>{track};
            clearConfirmation();
            if (!all_folder && track.id.empty()) {
                command_status = "Select a track or choose all folder";
                return true;
            }
            if (controller.convertTracks(tracks, ConvertOptions{format})) {
                show_activity = true;
                show_audio_process_panel = true;
                command_status = "Convert started: " + format + " | " +
                    (all_folder ? "all folder" : track.title);
            } else {
                command_status = "Error: " +
                    controller.audioProcessSnapshot().detail;
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::AutoCue) {
            int option = auto_cue_option_col;
            Track track = pending_auto_cue_track;
            clearConfirmation();
            show_activity = true;
            show_auto_cue_panel = true;
            if (option == 0) {
                if (track.id.empty()) {
                    command_status = "Select a track";
                } else if (controller.startAutoCueTrack(track)) {
                    command_status = "Auto Cue started: " + track.title;
                } else {
                    command_status = "Auto Cue: " +
                        controller.autoCueSnapshot().status;
                }
            } else if (option == 1) {
                if (controller.startAutoCueFolder()) {
                    command_status = "Auto Cue started: " +
                        displayName(controller.currentPath());
                } else {
                    command_status = "Auto Cue: " +
                        controller.autoCueSnapshot().status;
                }
            } else {
                command_status = "Auto Cue cancelled";
            }
            return true;
        }

        if (pending_confirmation == PendingConfirmation::Quit) {
            clearConfirmation();
            refresh_running = false;
            screen.Exit();
            return true;
        }

        if (pending_confirmation == PendingConfirmation::DeleteEntry) {
            Track entry = pending_delete_entry;
            clearConfirmation();
            std::string error;
            if (controller.deleteEntry(entry, error)) {
                command_status = "Deleted: " + entry.title;
            } else {
                command_status = "Error: " + error;
            }
            return true;
        }

        return false;
    };

    InputOption input_options = InputOption::Default();
    input_options.content = &command_input;
    input_options.placeholder = "input";
    input_options.multiline = false;
    input_options.on_enter = [&] {
        std::string input = trimInput(command_input);
        std::string download_source = downloadSourceFromInput(input);
        command_input.clear();

        command_status.clear();

        if (input == "q" || input == "quit") {
            refresh_running = false;
            screen.Exit();
        } else if (input == "stop") {
            controller.stopPlayback();
        } else if (input == "pause") {
            controller.togglePause();
        } else if (!download_source.empty()) {
            std::string destination = controller.currentPath();
            if (isPlaylistSource(download_source)) {
                pending_playlist_source = download_source;
                pending_playlist_destination = destination;
                pending_confirmation = PendingConfirmation::PlaylistDownload;
                command_status = "Playlist detected";
            } else {
                beginDownload(download_source, destination);
            }
        } else if (input.starts_with("mkdir ")) {
            std::string error;
            std::string name = trimInput(input.substr(6));
            command_status = controller.createFolder(name, error)
                ? "Folder created: " + name
                : "Error: " + error;
        } else if (input.starts_with("rename ")) {
            std::string error;
            std::string name = trimInput(input.substr(7));
            if (state.selectedDirectory < 0 ||
                state.selectedDirectory >= (int)dir_paths.size()) {
                command_status = "Error: Select a folder";
            } else if (controller.renameFolder(dir_paths[state.selectedDirectory], name, error)) {
                command_status = "Folder renamed: " + name;
                expanded_directory = controller.currentPath();
                preferred_directory = controller.currentPath();
            } else {
                command_status = "Error: " + error;
            }
        } else if (input == "refresh") {
            controller.scanDirectory(controller.currentPath(), true);
            command_status = "Library refreshed";
        } else if (!input.empty()) {
            command_status = "Unknown command";
        }
    };
    auto command = Input(input_options);

    auto browser = Container::Horizontal({
        dir_menu,
        track_menu,
    }, &browser_selector);

    auto bottom = Container::Horizontal({
        command,
        speed_slider,
        pitch_lock_checkbox,
    }, &bottom_selector);

    auto root_container = Container::Vertical({
        browser,
        progress_slider,
        bottom,
    }, &root_selector);

    auto root_renderer = Renderer(root_container, [&] {
        auto playback = controller.playbackSnapshot();
        auto download = controller.downloadSnapshot();
        auto stems = controller.stemSeparationSnapshot();
        auto audio_process = controller.audioProcessSnapshot();
        auto auto_cue = controller.autoCueSnapshot();
        playing_track_id = controller.playingTrackId();
        int terminal_width = std::max(60, Terminal::Size().dimx);
        int content_width = std::max(40, terminal_width - 2);
        int left_width = state.focus == FocusPane::Directories
            ? std::clamp(content_width / 4, 24, std::max(24, content_width / 2))
            : std::clamp(content_width / 5, 16, std::max(30, content_width / 3));
        int right_width = std::max(24, content_width - left_width - 1);
        show_time_column = controller.config().columns.time;
        show_bpm_column = controller.config().columns.bpm;
        show_key_column = controller.config().columns.key;
        show_kbps_column = controller.config().columns.kbps;
        show_rate_column = controller.config().columns.rate;
        show_size_column = controller.config().columns.size;
        show_genre_column = controller.config().columns.genre;
        visible_column_order = fitTrackColumns(right_width);
        int auxiliary_rows = (show_info ? 2 : 0) +
            (show_activity && show_download_panel && download.state != DownloadState::Idle ? 3 : 0) +
            (show_activity && show_stems_panel && stems.state != StemSeparationState::Idle ? 3 : 0) +
            (show_activity && show_audio_process_panel && audio_process.state != AudioProcessState::Idle ? 3 : 0) +
            (show_activity && show_auto_cue_panel && (auto_cue.running || auto_cue.done) ? 3 : 0);
        visible_row_count = std::max(1, Terminal::Size().dimy - 11 - auxiliary_rows);

        syncBrowserData(left_width, right_width, visible_row_count);

        double duration = std::max(0.0, playback.durationSeconds);
        double position = std::clamp(playback.positionSeconds, 0.0, duration);
        if (!progress_dragging || playback.state == PlaybackState::Playing) {
            progress_value = duration > 0.0
                ? (int)std::clamp((position / duration) * progress_max, 0.0, (double)progress_max)
                : 0;
            progress_dragging = false;
        }

        std::string now_playing = playback.title.empty() ? "TPlay" : playback.title;
        std::string now_playing_tags;
        Track playing_track_details = controller.playingTrack();
        if (!playing_track_id.empty()) {
            auto playing = std::find_if(visible_files.begin(), visible_files.end(),
                                        [&](const Track& track) {
                                            return track.id == playing_track_id;
                                        });
            if (playing != visible_files.end()) {
                playing_track_details = *playing;
            }
            if (!playing_track_details.id.empty()) {
                std::vector<std::string> tags;
                if (playing_track_details.bpm > 0.0) {
                    tags.push_back(std::to_string((int)playing_track_details.bpm));
                }
                if (!playing_track_details.key.empty()) {
                    tags.push_back(playing_track_details.key);
                }
                if (!tags.empty()) {
                    now_playing_tags = "  ";
                    for (size_t i = 0; i < tags.size(); ++i) {
                        if (i > 0) {
                            now_playing_tags += " | ";
                        }
                        now_playing_tags += tags[i];
                    }
                }
            }
        }
        int title_total_width = std::max(16, content_width - 24);
        int title_text_width = std::max(1, title_total_width - (int)now_playing_tags.size());
        std::string title = truncateEnd(now_playing, title_text_width) + now_playing_tags;
        bool editor_loading = editor_prepare_active.load();
        size_t spinner_frame = (size_t)(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() /
            120);
        std::string playback_status = playback.state == PlaybackState::Error
            ? "error: " + truncateEnd(playback.errorMessage, 28)
            : playbackStateToString(playback.state);
        std::string status_line = command_status;
        if (status_line.empty() && download.state != DownloadState::Idle) {
            status_line = download.message;
        }
        if (status_line.empty() && stems.state != StemSeparationState::Idle) {
            status_line = stems.message;
        }
        if (status_line.empty() && audio_process.state != AudioProcessState::Idle) {
            status_line = audio_process.message;
        }
        if (status_line.empty() && (auto_cue.running || auto_cue.done)) {
            status_line = auto_cue.status;
        }
        if (status_line.empty() && controller.directoryScanBusy()) {
            status_line = "Scanning: " + displayName(controller.currentPath());
        }

        Element directory_list = visible_dir_entries.empty()
            ? text("(empty)") | dim
            : dir_menu->Render();
        directory_list = directory_list | flex | reflect(directory_list_box);

        std::string panel_directory;
        if (move_to_mode &&
            state.selectedDirectory >= 0 &&
            state.selectedDirectory < (int)dir_paths.size()) {
            panel_directory = dir_paths[state.selectedDirectory];
        } else if (!expanded_directory.empty()) {
            panel_directory = controller.currentPath();
        }
        std::string folder_name = panel_directory.empty()
            ? ""
            : displayName(panel_directory);
        std::string panel_prefix = move_to_mode ? "Move to " : "Folder ";
        std::string panel_title = truncateEnd(
            panel_prefix + folder_name, std::max(6, left_width - 1));

        Element left_panel = vbox({
            text(panel_title) | bold,
            separator(),
            directory_list,
        }) | size(WIDTH, EQUAL, left_width);

        if (state.focus == FocusPane::Directories) {
            left_panel = left_panel | color(Color::Yellow) | flex;
        } else {
            left_panel = left_panel | flex_shrink;
        }

        Element track_list = visible_track_entries.empty()
            ? text("(empty)") | dim
            : track_menu->Render();
        track_list = track_list | flex | reflect(track_list_box);

        auto sortLabel = [&](const std::string& label, TrackSortColumn column) {
            if (column != track_sort_column) {
                return label;
            }
            return label + (track_sort_ascending ? "^" : "v");
        };
        auto headerCell = [&](const std::string& label,
                              TrackSortColumn column,
                              Box& box) {
            Element cell = text(sortLabel(label, column)) | reflect(box);
            if (column == track_sort_column) {
                cell = cell | bold | underlined;
            }
            return cell;
        };
        Elements header_columns = {
            text("  ") | size(WIDTH, EQUAL, 2),
            headerCell("Title (" + std::to_string(track_entries.size()) + ")",
                       TrackSortColumn::Title, title_header_box) | flex,
        };
        time_header_box = Box{};
        bpm_header_box = Box{};
        key_header_box = Box{};
        genre_header_box = Box{};
        bitrate_header_box = Box{};
        rate_header_box = Box{};
        size_header_box = Box{};
        auto columnHeaderBox = [&](const std::string& column) -> Box& {
            if (column == "time") return time_header_box;
            if (column == "bpm") return bpm_header_box;
            if (column == "key") return key_header_box;
            if (column == "genre") return genre_header_box;
            if (column == "kbps") return bitrate_header_box;
            if (column == "rate") return rate_header_box;
            return size_header_box;
        };
        for (const auto& column : visible_column_order) {
            int width = trackColumnWidth(column);
            if (width <= 0) {
                continue;
            }
            header_columns.push_back(separator());
            header_columns.push_back(
                headerCell(trackColumnLabel(column),
                           trackColumnSort(column),
                           columnHeaderBox(column)) |
                size(WIDTH, EQUAL, width));
        }
        Element track_header = hbox(header_columns);

        Element right_panel = vbox({
            track_header,
            separator(),
            track_list,
        });

        if (state.focus == FocusPane::Tracks) {
            right_panel = right_panel | color(Color::Yellow) | flex;
        } else {
            right_panel = right_panel | flex_shrink;
        }

        auto controlCell = [&](const std::string& label,
                               int width,
                               Box& box,
                               HoverControl control) {
            Element cell = text(label) | center | size(WIDTH, EQUAL, width) | reflect(box);
            if (hovered_control == control) {
                cell = cell | bold | color(Color::Yellow);
            }
            return cell;
        };

        Element top_progress_bar = hbox({
            progress_slider->Render() | flex,
        });

        speed_reset_box = Box{};
        Element speed_reset = controlCell("x", 3, speed_reset_box,
                                          HoverControl::SpeedReset);
        Element speed_control = speed_slider->Render() | flex;
        Element pitch_lock = pitch_lock_checkbox->Render() | size(WIDTH, EQUAL, 4);
        if (root_selector == 2 && bottom_selector == 1 && speed_focus == 0) {
            speed_reset = speed_reset | inverted;
        }
        if (root_selector == 2 && bottom_selector == 1 && speed_focus == 1) {
            speed_control = speed_control | inverted;
        }
        if (root_selector == 2 && bottom_selector == 1 && speed_focus == 2) {
            pitch_lock = pitch_lock | inverted;
        }
        Element speed_box = hbox({
            speed_reset,
            speed_control,
            text(std::format("{:.2f}x", (double)speed_value / 100.0)) |
                size(WIDTH, EQUAL, 6) | center,
            pitch_lock,
        }) | size(HEIGHT, EQUAL, 1);

        Element control_bar = hbox({
            controlCell("-", 3, volume_down_box, HoverControl::VolumeDown),
            text("🔈 " + std::to_string(controller.volume()) + "%") |
                size(WIDTH, EQUAL, 7) | center,
            controlCell("+", 3, volume_up_box, HoverControl::VolumeUp),
            separator(),
            controlCell("⏮", 4, previous_box, HoverControl::Previous),
            controlCell(playback_status, 3, play_pause_box, HoverControl::PlayPause),
            controlCell("⏭", 3, next_box, HoverControl::Next),
            controlCell(playbackModeToString(controller.playbackMode()), 4,
                        mode_box, HoverControl::Mode),
            separator(),
            speed_box | flex,
            separator(),
            text(formatPlaybackTime(position) + " / " + formatPlaybackTime(duration)) | size(WIDTH, EQUAL, 13),
        });

        command_paste_box = Box{};
        command_clear_box = Box{};
        Element command_box = hbox({
            text("v") | center | size(WIDTH, EQUAL, 3) |
                reflect(command_paste_box) |
                color(Color::Yellow),
            separator(),
            command->Render() | flex,
            separator(),
            text("x") | center | size(WIDTH, EQUAL, 3) |
                reflect(command_clear_box) |
                (command_input.empty() ? dim : color(Color::Yellow)),
        }) | size(WIDTH, EQUAL, std::max(18, content_width / 3)) |
            size(HEIGHT, EQUAL, 1);

        Element bottom_bar = hbox({
            command_box,

            separator(),

            text(truncateEnd(status_line, std::max(10, content_width / 2))) |
                dim | flex,

            filler(),
        });

        Elements layout = {
            (hbox({
                filler(),
                text(" " + title + " ") | bold,
                editor_loading
                    ? hbox({text(" "), spinner(12, spinner_frame) |
                                          color(Color::Yellow)})
                    : text("  "),
                filler(),
            }) | reflect(now_playing_box)),
            top_progress_bar,
            separator(),
            hbox({
                left_panel,
                separator(),
                right_panel,
            }) | flex,
            separator(),
            control_bar,
            separator(),
            bottom_bar,
        };

        if (show_info) {
            layout.push_back(separator());
            layout.push_back(text("S stem | X auto | Z manual | A trim | L export | U import | K check | N norm | C conv | M meta | H help | Q quit") | dim);
        }

        download_close_box = Box{};
        stems_close_box = Box{};
        audio_process_close_box = Box{};
        auto_cue_close_box = Box{};
        auto activityControl = [&](bool running, float progress, Box& close_box) {
            if (running) {
                return hbox({
                    gauge(progress) |
                        color(Color::Yellow) |
                        size(WIDTH, EQUAL, 20),
                    separator(),
                    text(std::to_string((int)(progress * 100.0f)) + "%") | dim,
                });
            }
            return hbox({
                text("x") |
                    color(Color::Yellow) |
                    bold |
                    center |
                    size(WIDTH, EQUAL, 3) |
                    reflect(close_box),
            });
        };

        if (show_activity && show_download_panel &&
            download.state != DownloadState::Idle) {
            layout.push_back(separator());
            layout.push_back(hbox({
                text("Download: " + download.message) | color(Color::Yellow),
                filler(),
                activityControl(download.state == DownloadState::Running,
                                download.progress,
                                download_close_box),
            }));
            layout.push_back(text(truncateEnd(download.detail, content_width)) | dim);
        }

        if (show_activity && show_stems_panel &&
            stems.state != StemSeparationState::Idle) {
            layout.push_back(separator());
            layout.push_back(hbox({
                text("Demucs: " + stems.message) | color(Color::Yellow),
                filler(),
                activityControl(stems.state == StemSeparationState::Running,
                                stems.progress,
                                stems_close_box),
            }));
            std::string detail;
            if (stems.elapsedSeconds > 0.0) {
                detail = "time " + formatPlaybackTime(stems.elapsedSeconds);
            }
            if (!stems.detail.empty()) {
                if (!detail.empty()) {
                    detail += " | ";
                }
                detail += stems.detail;
            }
            if (!stems.outputDirectory.empty()) {
                detail += " | Output: " + stems.outputDirectory;
            }
            layout.push_back(text(truncateEnd(detail, content_width)) | dim);
        }

        if (show_activity && show_audio_process_panel &&
            audio_process.state != AudioProcessState::Idle) {
            layout.push_back(separator());
            std::string label = audio_process.kind == AudioProcessKind::Normalize
                ? "Normalization: "
                : "Convert: ";
            layout.push_back(hbox({
                text(label + audio_process.message) | color(Color::Yellow),
                filler(),
                activityControl(audio_process.state == AudioProcessState::Running,
                                audio_process.progress,
                                audio_process_close_box),
            }));
            std::string detail = audio_process.detail;
            if (!audio_process.outputDirectory.empty()) {
                detail += " | Output: " + audio_process.outputDirectory;
            }
            layout.push_back(text(truncateEnd(detail, content_width)) | dim);
        }

        if (show_activity && show_auto_cue_panel &&
            (auto_cue.running || auto_cue.done)) {
            layout.push_back(separator());
            float cue_progress = auto_cue.total > 0
                ? std::clamp((float)auto_cue.current / (float)auto_cue.total, 0.0f, 1.0f)
                : (auto_cue.done ? 1.0f : 0.0f);
            layout.push_back(hbox({
                text("Auto Cue: " + auto_cue.status) | color(Color::Yellow),
                filler(),
                auto_cue.running
                    ? hbox({
                        gauge(cue_progress) |
                            color(Color::Yellow) |
                            size(WIDTH, EQUAL, 20),
                        separator(),
                        text(std::to_string(auto_cue.current) + "/" +
                             std::to_string(auto_cue.total)) | dim,
                    })
                    : hbox({
                        text("x") |
                            color(Color::Yellow) |
                            bold |
                            center |
                            size(WIDTH, EQUAL, 3) |
                            reflect(auto_cue_close_box),
                    }),
            }));
            std::string detail = auto_cue.currentFile;
            if (!detail.empty()) {
                detail += " | ";
            }
            detail += "ok " + std::to_string(auto_cue.success) +
                " | errors " + std::to_string(auto_cue.errors);
            layout.push_back(text(truncateEnd(detail, content_width)) | dim);
        }

        Element main_layout = vbox(layout) | border | flex;
        if (show_metadata_popup) {
            metadata_close_box = Box{};
            metadata_list_box = Box{};
            int dialog_width = std::clamp(content_width - 4, 44, 92);
            int max_rows = std::max(5, Terminal::Size().dimy - 12);
            int name_width = std::min(20, std::max(8, dialog_width / 4));
            int value_width = std::max(10, dialog_width - name_width - 8);
            std::vector<std::pair<std::string, std::string>> metadata_rows;
            for (const auto& [name, value] : metadata_details) {
                std::vector<std::string> wrapped = wrapText(value, value_width);
                if (wrapped.empty()) {
                    wrapped.emplace_back("");
                }
                for (size_t i = 0; i < wrapped.size(); ++i) {
                    metadata_rows.emplace_back(i == 0 ? name : "", wrapped[i]);
                }
            }

            int total_rows = (int)metadata_rows.size();
            metadata_offset = std::clamp(metadata_offset, 0,
                                         std::max(0, total_rows - max_rows));

            Elements rows;
            metadata_link_boxes.clear();
            metadata_link_urls.clear();
            metadata_link_boxes.reserve(max_rows);
            metadata_link_urls.reserve(max_rows);
            if (metadata_rows.empty()) {
                rows.push_back(text("No metadata") | dim | center);
            } else {
                int end = std::min(total_rows, metadata_offset + max_rows);
                for (int i = metadata_offset; i < end; ++i) {
                    const auto& [name, value] = metadata_rows[i];
                    Element value_element = text(value) |
                        dim |
                        size(WIDTH, EQUAL, value_width);
                    if (auto url = firstUrlInText(value)) {
                        metadata_link_boxes.emplace_back();
                        metadata_link_urls.push_back(*url);
                        value_element = text(value) |
                            color(Color::Cyan) |
                            underlined |
                            size(WIDTH, EQUAL, value_width) |
                            reflect(metadata_link_boxes.back());
                    }
                    rows.push_back(hbox({
                        text(truncateEnd(name, name_width)) |
                            bold |
                            size(WIDTH, EQUAL, name_width),
                        text(" "),
                        value_element,
                    }));
                }
            }

            std::string counter = total_rows == 0
                ? "0/0"
                : std::to_string(metadata_offset + 1) + "-" +
                    std::to_string(std::min(total_rows, metadata_offset + max_rows)) +
                    "/" + std::to_string(total_rows);
            Element close_button = text("Close") | center |
                size(WIDTH, EQUAL, 12) |
                border |
                bold |
                color(Color::Yellow) |
                reflect(metadata_close_box);
            Element dialog = vbox({
                text("meta data") | bold | center,
                text(truncateEnd(metadata_title, dialog_width - 4)) | dim | center,
                separator(),
                vbox(rows) | reflect(metadata_list_box),
                separator(),
                hbox({
                    text(counter) | dim,
                    filler(),
                    close_button,
                    filler(),
                    text("Esc/Enter") | dim,
                }),
            }) |
                size(WIDTH, EQUAL, dialog_width) |
                border |
                clear_under |
                center;

            return dbox({main_layout, dialog}) | flex;
        }

        if (show_eq_popup) {
            for (auto& box : eq_reset_boxes) {
                box = Box{};
            }
            eq_close_box = Box{};
            auto gainLabel = [](int value) {
                if (value <= -60) {
                    return std::string("-∞");
                }
                return value > 0 ? "+" + std::to_string(value) : std::to_string(value);
            };
            auto eqRow = [&](const std::string& label,
                             Component slider,
                             int value,
                             int index) {
                int slider_focus = index * 2;
                int reset_focus = index * 2 + 1;
                Element slider_element = slider->Render() | flex;
                Element reset_element = text("x") | center | size(WIDTH, EQUAL, 3) |
                    reflect(eq_reset_boxes[(size_t)index]);
                if (eq_selected == slider_focus) {
                    slider_element = slider_element | inverted;
                }
                if (eq_selected == reset_focus) {
                    reset_element = reset_element | inverted;
                }
                Element row = hbox({
                    text(label) | size(WIDTH, EQUAL, 4),
                    slider_element,
                    text(gainLabel(value)) | center | size(WIDTH, EQUAL, 4),
                    reset_element,
                });
                return row;
            };
            Element close_button = text("Close") | center |
                size(WIDTH, EQUAL, 12) |
                border |
                bold |
                color(Color::Yellow) |
                reflect(eq_close_box);
            if (eq_selected == 6) {
                close_button = close_button | inverted;
            }
            Element dialog = vbox({
                text("equalizer") | bold | center,
                separator(),
                eqRow("HI", eq_high_slider, eq_high, 0),
                eqRow("MID", eq_mid_slider, eq_mid, 1),
                eqRow("LOW", eq_low_slider, eq_low, 2),
                separator(),
                close_button | center,
            }) |
                size(WIDTH, EQUAL, std::clamp(content_width - 8, 44, 80)) |
                border |
                clear_under |
                center;
            return dbox({main_layout, dialog}) | flex;
        }

        if (manual_cue_editor.isOpen()) {
            return manual_cue_editor.renderOverlay(main_layout, content_width);
        }
        if (trim_editor.isOpen()) {
            return trim_editor.renderOverlay(main_layout, content_width);
        }

        if (pending_confirmation == PendingConfirmation::None) {
            return main_layout;
        }

        confirm_yes_box = Box{};
        confirm_no_box = Box{};
        std::string question;
        std::string detail;
        if (pending_confirmation == PendingConfirmation::PlaylistDownload) {
            question = "Download playlist?";
            detail = truncateEnd(pending_playlist_source, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::StemSeparation) {
            question = "Separate track with Demucs?";
            detail = truncateEnd(pending_stem_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::Normalization) {
            question = "Normalization";
            detail = pending_process_all_folder
                ? "Folder: " + displayName(controller.currentPath())
                : truncateEnd(pending_process_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::Convert) {
            question = "Convert";
            detail = pending_process_all_folder
                ? "Folder: " + displayName(controller.currentPath())
                : truncateEnd(pending_process_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::AutoCue) {
            question = "Auto Cue";
            detail = pending_auto_cue_track.id.empty()
                ? "Folder: " + displayName(controller.currentPath())
                : truncateEnd(pending_auto_cue_track.title, std::max(20, content_width - 18));
        } else if (pending_confirmation == PendingConfirmation::DeleteEntry) {
            question = pending_delete_entry.type == EntryType::Directory
                ? "Delete folder?"
                : "Delete track?";
            detail = truncateEnd(pending_delete_entry.title, std::max(20, content_width - 18));
        } else {
            question = "Quit TPlay?";
            detail = "Playback and running jobs will stop.";
        }

        auto button = [&](const std::string& label, Box& box, int index) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 10) |
                border |
                reflect(box);
            if (confirmation_selected == index) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        auto optionButton = [&](const std::string& label,
                                Box& box,
                                int row,
                                int col,
                                int active_row,
                                int active_col,
                                bool selected) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 12) |
                border |
                reflect(box);
            if (selected) {
                element = element | inverted;
            }
            if (active_row == row && active_col == col) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        auto stemButton = [&](const std::string& label,
                              Box& box,
                              int row,
                              int col,
                              bool selected) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 10) |
                border |
                reflect(box);
            if (selected) {
                element = element | inverted;
            }
            if (stem_option_row == row && stem_option_col == col) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        auto autoCueButton = [&](const std::string& label,
                                 Box& box,
                                 int col,
                                 bool enabled = true) {
            Element element = text(label) | center |
                size(WIDTH, EQUAL, 16) |
                border |
                reflect(box);
            if (!enabled) {
                element = element | dim;
            }
            if (auto_cue_option_col == col) {
                element = element | bold | color(Color::Yellow);
            }
            return element;
        };

        Elements dialog_items = {
            text(question) | bold | center,
            separator(),
            text(detail) | dim | center,
            separator(),
        };

        if (pending_confirmation == PendingConfirmation::StemSeparation) {
            stem_2_box = Box{};
            stem_4_box = Box{};
            stem_mp3_box = Box{};
            stem_wav_box = Box{};
            stem_flac_box = Box{};
            dialog_items.push_back(hbox({
                filler(),
                stemButton("2stems", stem_2_box, 0, 0,
                           pending_stem_config.stems == 2),
                text("  "),
                stemButton("4stems", stem_4_box, 0, 1,
                           pending_stem_config.stems == 4),
                filler(),
            }));
            dialog_items.push_back(hbox({
                filler(),
                stemButton("mp3", stem_mp3_box, 1, 0,
                           pending_stem_config.outputFormat == "mp3"),
                text("  "),
                stemButton("wav", stem_wav_box, 1, 1,
                           pending_stem_config.outputFormat == "wav"),
                text("  "),
                stemButton("flac", stem_flac_box, 1, 2,
                           pending_stem_config.outputFormat == "flac"),
                filler(),
            }));
            dialog_items.push_back(separator());
            dialog_items.push_back(hbox({
                filler(),
                stemButton("Yes", confirm_yes_box, 2, 0, false),
                text("  "),
                stemButton("No", confirm_no_box, 2, 1, false),
                filler(),
            }));
        } else if (pending_confirmation == PendingConfirmation::Normalization) {
            normalize_16_box = Box{};
            normalize_14_box = Box{};
            normalize_9_box = Box{};
            normalize_one_box = Box{};
            normalize_all_box = Box{};
            dialog_items.push_back(text("-14 LUFS (Short-Term Max)") | center);
            dialog_items.push_back(text("Best for Club / DJ tracks") | dim | center);
            dialog_items.push_back(text("-16 LUFS (Integrated)") | center);
            dialog_items.push_back(text("Streaming standard (Spotify/Apple)") | dim | center);
            // dialog_items.push_back(text("-9 LUFS") | center);
            dialog_items.push_back(separator());
            dialog_items.push_back(hbox({
                filler(),
                optionButton("-16", normalize_16_box, 0, 0,
                             normalize_option_row, normalize_option_col,
                             pending_normalize_lufs == -16),
                text(" "),
                optionButton("-14", normalize_14_box, 0, 1,
                             normalize_option_row, normalize_option_col,
                             pending_normalize_lufs == -14),
                text(" "),
                // optionButton("-9", normalize_9_box, 0, 2,
                //              normalize_option_row, normalize_option_col,
                //              pending_normalize_lufs == -9),
                filler(),
            }));
            dialog_items.push_back(hbox({
                filler(),
                optionButton("one", normalize_one_box, 1, 0,
                             normalize_option_row, normalize_option_col,
                             !pending_process_all_folder),
                text(" "),
                optionButton("all folder", normalize_all_box, 1, 1,
                             normalize_option_row, normalize_option_col,
                             pending_process_all_folder),
                filler(),
            }));
            dialog_items.push_back(separator());
            dialog_items.push_back(hbox({
                filler(),
                optionButton("Yes", confirm_yes_box, 2, 0,
                             normalize_option_row, normalize_option_col, false),
                text(" "),
                optionButton("No", confirm_no_box, 2, 1,
                             normalize_option_row, normalize_option_col, false),
                filler(),
            }));
        } else if (pending_confirmation == PendingConfirmation::Convert) {
            convert_wav_box = Box{};
            convert_mp3_box = Box{};
            convert_m4a_box = Box{};
            convert_flac_box = Box{};
            convert_one_box = Box{};
            convert_all_box = Box{};
            dialog_items.push_back(hbox({
                filler(),
                optionButton("wav", convert_wav_box, 0, 0,
                             convert_option_row, convert_option_col,
                             pending_convert_format == "wav"),
                text(" "),
                optionButton("mp3", convert_mp3_box, 0, 1,
                             convert_option_row, convert_option_col,
                             pending_convert_format == "mp3"),
                text(" "),
                optionButton("m4a", convert_m4a_box, 0, 2,
                             convert_option_row, convert_option_col,
                             pending_convert_format == "m4a"),
                text(" "),
                optionButton("flac", convert_flac_box, 0, 3,
                             convert_option_row, convert_option_col,
                             pending_convert_format == "flac"),
                filler(),
            }));
            dialog_items.push_back(hbox({
                filler(),
                optionButton("one", convert_one_box, 1, 0,
                             convert_option_row, convert_option_col,
                             !pending_process_all_folder),
                text(" "),
                optionButton("all folder", convert_all_box, 1, 1,
                             convert_option_row, convert_option_col,
                             pending_process_all_folder),
                filler(),
            }));
            dialog_items.push_back(separator());
            dialog_items.push_back(hbox({
                filler(),
                optionButton("Yes", confirm_yes_box, 2, 0,
                             convert_option_row, convert_option_col, false),
                text(" "),
                optionButton("No", confirm_no_box, 2, 1,
                             convert_option_row, convert_option_col, false),
                filler(),
            }));
        } else if (pending_confirmation == PendingConfirmation::AutoCue) {
            auto_cue_track_box = Box{};
            auto_cue_folder_box = Box{};
            auto_cue_cancel_box = Box{};
            bool has_track = !pending_auto_cue_track.id.empty();
            dialog_items.push_back(hbox({
                filler(),
                autoCueButton("one", auto_cue_track_box, 0, has_track),
                text(" "),
                autoCueButton("all folder", auto_cue_folder_box, 1),
                text(" "),
                autoCueButton("cancel", auto_cue_cancel_box, 2),
                filler(),
            }));
        } else {
            dialog_items.push_back(hbox({
                filler(),
                button("Yes", confirm_yes_box, 0),
                text("  "),
                button("No", confirm_no_box, 1),
                filler(),
            }));
        }

        Element dialog = vbox(dialog_items) |
            size(WIDTH, LESS_THAN, std::min(64, content_width - 4)) |
            border |
            clear_under |
            center;

        return dbox({main_layout, dialog}) | flex;
    });

    auto paneAtMouse = [&](const Mouse& mouse) {
        if (directory_list_box.Contain(mouse.x, mouse.y)) {
            return 0;
        }
        if (track_list_box.Contain(mouse.x, mouse.y)) {
            return 1;
        }
        return -1;
    };

    auto selectVisibleRowAtMouse = [&](const Mouse& mouse) -> std::pair<int, int> {
        int pane = paneAtMouse(mouse);
        if (pane == -1) {
            return {-1, -1};
        }
        int local_row = mouse.y -
            (pane == 0 ? directory_list_box.y_min : track_list_box.y_min);
        int count = pane == 0
            ? (int)visible_dir_entries.size()
            : (int)visible_track_entries.size();
        if (local_row >= count) {
            return {-1, -1};
        }

        root_selector = 0;
        browser_selector = pane;
        if (pane == 0) {
            dir_view_selected = local_row;
            state.selectedDirectory = state.dirOffset + local_row;
            state.focus = FocusPane::Directories;
            return {pane, state.selectedDirectory};
        }

        track_view_selected = local_row;
        state.selectedTrack = state.trackOffset + local_row;
        state.focus = FocusPane::Tracks;
        return {pane, state.selectedTrack};
    };

    auto moveBrowserSelection = [&](int direction, int pane) {
        int count = pane == 0 ? (int)dir_entries.size() : (int)track_entries.size();
        if (count == 0) {
            return false;
        }

        root_selector = 0;
        browser_selector = pane;
        state.focus = pane == 0 ? FocusPane::Directories : FocusPane::Tracks;
        int& selected = pane == 0 ? state.selectedDirectory : state.selectedTrack;
        int next = std::clamp(selected + direction, 0, count - 1);
        if (next == selected) {
            return false;
        }
        selected = next;
        return true;
    };

    auto controlAtMouse = [&](const Mouse& mouse) {
        if (volume_down_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::VolumeDown;
        }
        if (volume_up_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::VolumeUp;
        }
        if (previous_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::Previous;
        }
        if (play_pause_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::PlayPause;
        }
        if (next_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::Next;
        }
        if (mode_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::Mode;
        }
        if (speed_reset_box.Contain(mouse.x, mouse.y)) {
            return HoverControl::SpeedReset;
        }
        return HoverControl::None;
    };

    auto sortColumnAtMouse = [&](const Mouse& mouse) -> std::optional<TrackSortColumn> {
        if (title_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Title;
        }
        if (time_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Time;
        }
        if (bpm_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Bpm;
        }
        if (key_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Key;
        }
        if (genre_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Genre;
        }
        if (bitrate_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Bitrate;
        }
        if (rate_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::SampleRate;
        }
        if (size_header_box.Contain(mouse.x, mouse.y)) {
            return TrackSortColumn::Size;
        }
        return std::nullopt;
    };

    auto togglePlayback = [&] {
        auto playback = controller.playbackSnapshot();
        if (playback.state == PlaybackState::Stopped ||
            playback.state == PlaybackState::Error) {
            playSelectedTrack();
        } else {
            controller.togglePause();
        }
    };

    auto activateControl = [&](HoverControl control) {
        switch (control) {
        case HoverControl::VolumeDown:
            controller.volumeDown();
            return true;
        case HoverControl::VolumeUp:
            controller.volumeUp();
            return true;
        case HoverControl::Previous:
            controller.playPreviousTrack();
            return true;
        case HoverControl::PlayPause:
            togglePlayback();
            return true;
        case HoverControl::Next:
            controller.playNextTrack();
            return true;
        case HoverControl::Mode:
            controller.cyclePlaybackMode();
            return true;
        case HoverControl::SpeedReset:
            speed_value = 100;
            controller.setPlaybackRate(1.0);
            return true;
        case HoverControl::None:
            return false;
        }
        return false;
    };

    auto mainKey = [&](const Event& event,
                       const std::string& action,
                       std::initializer_list<std::string> defaults) {
        return ui::bindingMatches(event, controller.config().keybinds,
                                  action, defaults);
    };

    enum class EditorPrepareTarget {
        ManualCues,
        Trim,
    };

    auto prepareEditor = [&](const Track& track, EditorPrepareTarget target) {
        if (editor_prepare_active.load()) {
            command_status = "Waveform is loading";
            return;
        }
        if (track.type != EntryType::File || track.id.empty()) {
            command_status = target == EditorPrepareTarget::ManualCues
                ? "Select a track for manual cues"
                : "Select a track to trim";
            return;
        }
        if (editor_prepare_worker.joinable()) {
            editor_prepare_worker.request_stop();
            editor_prepare_worker.join();
        }
        controller.stopPlayback();
        controller.stopPreviewPlayback();
        editor_prepare_active = true;
        command_status = target == EditorPrepareTarget::ManualCues
            ? "Loading waveform: " + track.title
            : "Loading trim waveform: " + track.title;
        editor_prepare_worker = std::jthread(
            [&, track, target](std::stop_token token) {
                std::string error;
                AutoCueFeatures waveform = controller.waveformForTrack(track, error);
                if (token.stop_requested()) {
                    return;
                }
                screen.Post([&, track, target, waveform = std::move(waveform),
                             error = std::move(error)]() mutable {
                    editor_prepare_active = false;
                    if (!error.empty()) {
                        command_status = error;
                        return;
                    }
                    if (target == EditorPrepareTarget::ManualCues) {
                        manual_cue_editor.open(track, &waveform, &error);
                    } else {
                        trim_editor.open(track, &waveform, &error);
                    }
                });
            });
    };

    auto component = CatchEvent(root_renderer, [&](Event e) {
        if (e == Event::Custom) {
            std::string active_track_id = controller.playingTrackId();
            if (!active_track_id.empty() && active_track_id != playing_track_id) {
                auto playing = std::find_if(visible_files.begin(), visible_files.end(),
                                            [&](const Track& track) {
                                                return track.id == active_track_id;
                                            });
                if (playing != visible_files.end()) {
                    state.selectedTrack = (int)std::distance(visible_files.begin(), playing);
                    if (!move_to_mode) {
                        state.focus = FocusPane::Tracks;
                        browser_selector = 1;
                        root_selector = 0;
                    }
                }
                playing_track_id = active_track_id;
            }
            return true;
        }

        if (manual_cue_editor.isOpen()) {
            return manual_cue_editor.handleEvent(e);
        }
        if (trim_editor.isOpen()) {
            return trim_editor.handleEvent(e);
        }

        if (show_metadata_popup) {
            int visible_rows = std::max(5, Terminal::Size().dimy - 12);
            int terminal_width = std::max(60, Terminal::Size().dimx);
            int dialog_width = std::clamp(terminal_width - 6, 44, 92);
            int name_width = std::min(20, std::max(8, dialog_width / 4));
            int value_width = std::max(10, dialog_width - name_width - 8);
            int visual_rows = 0;
            for (const auto& [name, value] : metadata_details) {
                visual_rows += std::max(1, (int)wrapText(value, value_width).size());
            }
            int max_offset = std::max(0, visual_rows - visible_rows);
            auto scrollMetadata = [&](int delta) {
                metadata_offset = std::clamp(metadata_offset + delta, 0, max_offset);
            };

            if (e.is_mouse()) {
                if (e.mouse().button == Mouse::Left &&
                    e.mouse().motion == Mouse::Pressed) {
                    if (metadata_close_box.Contain(e.mouse().x, e.mouse().y)) {
                        show_metadata_popup = false;
                        return true;
                    }
                    for (size_t i = 0; i < metadata_link_boxes.size(); ++i) {
                        if (metadata_link_boxes[i].Contain(e.mouse().x, e.mouse().y)) {
                            std::string error;
                            if (controller.openExternalUrl(metadata_link_urls[i], error)) {
                                command_status = "Opened: " + metadata_link_urls[i];
                            } else {
                                command_status = "Error: " + error;
                            }
                            return true;
                        }
                    }
                }
                if (e.mouse().button == Mouse::WheelUp) {
                    scrollMetadata(-1);
                    return true;
                }
                if (e.mouse().button == Mouse::WheelDown) {
                    scrollMetadata(1);
                    return true;
                }
                return true;
            }

            if (mainKey(e, "metadata_close",
                        {"escape", "enter", "m", "M", "ь", "Ь"})) {
                show_metadata_popup = false;
                return true;
            }
            if (mainKey(e, "up", {"up"})) {
                scrollMetadata(-1);
                return true;
            }
            if (mainKey(e, "down", {"down"})) {
                scrollMetadata(1);
                return true;
            }
            if (mainKey(e, "page_up", {"page_up"})) {
                scrollMetadata(-visible_rows);
                return true;
            }
            if (mainKey(e, "page_down", {"page_down"})) {
                scrollMetadata(visible_rows);
                return true;
            }
            return true;
        }

        if (show_eq_popup) {
            auto resetEq = [&](int index) {
                if (index == 0) eq_high = 0;
                if (index == 1) eq_mid = 0;
                if (index == 2) eq_low = 0;
                applyEq();
            };
            auto adjustEq = [&](int delta) {
                int row = eq_selected / 2;
                if (eq_selected % 2 != 0 || row > 2) {
                    return;
                }
                if (row == 0) eq_high = std::clamp(eq_high + delta, eq_min, eq_max);
                if (row == 1) eq_mid = std::clamp(eq_mid + delta, eq_min, eq_max);
                if (row == 2) eq_low = std::clamp(eq_low + delta, eq_min, eq_max);
                applyEq();
            };
            if (e.is_mouse() &&
                e.mouse().button == Mouse::Left &&
                e.mouse().motion == Mouse::Pressed) {
                if (eq_close_box.Contain(e.mouse().x, e.mouse().y)) {
                    show_eq_popup = false;
                    return true;
                }
                for (int i = 0; i < 3; ++i) {
                    if (eq_reset_boxes[(size_t)i].Contain(e.mouse().x, e.mouse().y)) {
                        eq_selected = i * 2 + 1;
                        resetEq(i);
                        return true;
                    }
                }
                return true;
            }
            if (mainKey(e, "equalizer_close",
                        {"escape", "e", "E", "у", "У"})) {
                show_eq_popup = false;
                return true;
            }
            if (mainKey(e, "up", {"up"})) {
                eq_selected = std::max(0, eq_selected - 1);
                return true;
            }
            if (mainKey(e, "down", {"down"})) {
                eq_selected = std::min(6, eq_selected + 1);
                return true;
            }
            if (mainKey(e, "left", {"left"})) {
                adjustEq(-1);
                return true;
            }
            if (mainKey(e, "right", {"right"})) {
                adjustEq(1);
                return true;
            }
            if (mainKey(e, "equalizer_reset", {"x", "X", "ч", "Ч"})) {
                if (eq_selected < 6) {
                    resetEq(eq_selected / 2);
                }
                return true;
            }
            if (mainKey(e, "play", {"enter"})) {
                if (eq_selected == 6) {
                    show_eq_popup = false;
                } else if (eq_selected % 2 == 1) {
                    resetEq(eq_selected / 2);
                }
                return true;
            }
            return true;
        }

        if (pending_confirmation != PendingConfirmation::None) {
            auto maxProcessCol = [&](int row) {
                if (pending_confirmation == PendingConfirmation::Convert && row == 0) {
                    return 3;
                }
                if (pending_confirmation == PendingConfirmation::Normalization && row == 0) {
                    return 2;
                }
                return 1;
            };
            auto clampProcessCol = [&](int& row, int& col) {
                col = std::clamp(col, 0, maxProcessCol(row));
            };
            auto chooseNormalizationOption = [&](int row, int col) {
                normalize_option_row = row;
                normalize_option_col = col;
                if (row == 0) {
                    if (col == 0) {
                        pending_normalize_lufs = -16;
                        pending_normalize_mode = "Integrated";
                    } else if (col == 1) {
                        pending_normalize_lufs = -14;
                        pending_normalize_mode = "Short-Term Max";
                    } else {
                        pending_normalize_lufs = -9;
                        pending_normalize_mode = "";
                    }
                } else if (row == 1) {
                    pending_process_all_folder = col == 1;
                }
            };
            auto chooseConvertOption = [&](int row, int col) {
                convert_option_row = row;
                convert_option_col = col;
                if (row == 0) {
                    static const std::array<std::string, 4> formats = {
                        "wav", "mp3", "m4a", "flac",
                    };
                    pending_convert_format = formats[std::clamp(col, 0, 3)];
                } else if (row == 1) {
                    pending_process_all_folder = col == 1;
                }
            };
            auto chooseAutoCueOption = [&](int col) {
                bool has_track = !pending_auto_cue_track.id.empty();
                if (col == 0 && !has_track) {
                    col = 1;
                }
                auto_cue_option_col = std::clamp(col, 0, 2);
            };

            if (pending_confirmation == PendingConfirmation::AutoCue) {
                if (e.is_mouse() &&
                    e.mouse().button == Mouse::Left &&
                    e.mouse().motion == Mouse::Pressed) {
                    if (auto_cue_track_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAutoCueOption(0);
                        if (!pending_auto_cue_track.id.empty()) {
                            acceptConfirmation();
                        }
                        return true;
                    }
                    if (auto_cue_folder_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAutoCueOption(1);
                        acceptConfirmation();
                        return true;
                    }
                    if (auto_cue_cancel_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseAutoCueOption(2);
                        rejectConfirmation();
                        return true;
                    }
                    return true;
                }

                if (mainKey(e, "left", {"left", "shift_tab"})) {
                    chooseAutoCueOption(auto_cue_option_col - 1);
                    return true;
                }
                if (mainKey(e, "right", {"right", "tab"})) {
                    chooseAutoCueOption(auto_cue_option_col + 1);
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (auto_cue_option_col == 2) {
                        rejectConfirmation();
                    } else {
                        acceptConfirmation();
                    }
                    return true;
                }
                if (mainKey(e, "confirm_no", {"escape", "n", "N", "т", "Т"})) {
                    rejectConfirmation();
                    return true;
                }
                if (mainKey(e, "confirm_yes", {"y", "Y", "н", "Н"})) {
                    acceptConfirmation();
                    return true;
                }
                return true;
            }

            if (e.is_mouse() &&
                e.mouse().button == Mouse::Left &&
                e.mouse().motion == Mouse::Pressed) {
                if (pending_confirmation == PendingConfirmation::StemSeparation) {
                    if (stem_2_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.stems = 2;
                        stem_option_row = 0;
                        stem_option_col = 0;
                        return true;
                    }
                    if (stem_4_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.stems = 4;
                        stem_option_row = 0;
                        stem_option_col = 1;
                        return true;
                    }
                    if (stem_mp3_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.outputFormat = "mp3";
                        stem_option_row = 1;
                        stem_option_col = 0;
                        return true;
                    }
                    if (stem_wav_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.outputFormat = "wav";
                        stem_option_row = 1;
                        stem_option_col = 1;
                        return true;
                    }
                    if (stem_flac_box.Contain(e.mouse().x, e.mouse().y)) {
                        pending_stem_config.outputFormat = "flac";
                        stem_option_row = 1;
                        stem_option_col = 2;
                        return true;
                    }
                }

                if (pending_confirmation == PendingConfirmation::Normalization) {
                    if (normalize_16_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(0, 0);
                        return true;
                    }
                    if (normalize_14_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(0, 1);
                        return true;
                    }
                    if (normalize_9_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(0, 2);
                        return true;
                    }
                    if (normalize_one_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(1, 0);
                        return true;
                    }
                    if (normalize_all_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseNormalizationOption(1, 1);
                        return true;
                    }
                }

                if (pending_confirmation == PendingConfirmation::Convert) {
                    if (convert_wav_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(0, 0);
                        return true;
                    }
                    if (convert_mp3_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(0, 1);
                        return true;
                    }
                    if (convert_m4a_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(0, 2);
                        return true;
                    }
                    if (convert_flac_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(0, 3);
                        return true;
                    }
                    if (convert_one_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(1, 0);
                        return true;
                    }
                    if (convert_all_box.Contain(e.mouse().x, e.mouse().y)) {
                        chooseConvertOption(1, 1);
                        return true;
                    }
                }

                if (confirm_yes_box.Contain(e.mouse().x, e.mouse().y)) {
                    confirmation_selected = 0;
                    stem_option_row = 2;
                    stem_option_col = 0;
                    normalize_option_row = 2;
                    normalize_option_col = 0;
                    convert_option_row = 2;
                    convert_option_col = 0;
                    acceptConfirmation();
                    return true;
                }
                if (confirm_no_box.Contain(e.mouse().x, e.mouse().y)) {
                    confirmation_selected = 1;
                    stem_option_row = 2;
                    stem_option_col = 1;
                    normalize_option_row = 2;
                    normalize_option_col = 1;
                    convert_option_row = 2;
                    convert_option_col = 1;
                    rejectConfirmation();
                    return true;
                }
                return true;
            }

            if (pending_confirmation == PendingConfirmation::StemSeparation) {
                auto maxStemCol = [&] {
                    return stem_option_row == 0 ? 1 : (stem_option_row == 1 ? 2 : 1);
                };
                if (mainKey(e, "left", {"left", "shift_tab"})) {
                    stem_option_col = std::max(0, stem_option_col - 1);
                    return true;
                }
                if (mainKey(e, "right", {"right", "tab"})) {
                    stem_option_col = std::min(maxStemCol(), stem_option_col + 1);
                    return true;
                }
                if (mainKey(e, "up", {"up"})) {
                    stem_option_row = std::max(0, stem_option_row - 1);
                    stem_option_col = std::min(maxStemCol(), stem_option_col);
                    return true;
                }
                if (mainKey(e, "down", {"down"})) {
                    stem_option_row = std::min(2, stem_option_row + 1);
                    stem_option_col = std::min(maxStemCol(), stem_option_col);
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (stem_option_row == 0) {
                        if (stem_option_col == 0) {
                            pending_stem_config.stems = 2;
                        } else {
                            pending_stem_config.stems = 4;
                        }
                    } else if (stem_option_row == 1) {
                        if (stem_option_col == 0) {
                            pending_stem_config.outputFormat = "mp3";
                        } else if (stem_option_col == 1) {
                            pending_stem_config.outputFormat = "wav";
                        } else {
                            pending_stem_config.outputFormat = "flac";
                        }
                    } else if (stem_option_col == 0) {
                        acceptConfirmation();
                    } else {
                        rejectConfirmation();
                    }
                    return true;
                }
            }

            if (pending_confirmation == PendingConfirmation::Normalization ||
                pending_confirmation == PendingConfirmation::Convert) {
                int& row = pending_confirmation == PendingConfirmation::Normalization
                    ? normalize_option_row
                    : convert_option_row;
                int& col = pending_confirmation == PendingConfirmation::Normalization
                    ? normalize_option_col
                    : convert_option_col;
                if (mainKey(e, "left", {"left", "shift_tab"})) {
                    col = std::max(0, col - 1);
                    return true;
                }
                if (mainKey(e, "right", {"right", "tab"})) {
                    col = std::min(maxProcessCol(row), col + 1);
                    return true;
                }
                if (mainKey(e, "up", {"up"})) {
                    row = std::max(0, row - 1);
                    clampProcessCol(row, col);
                    return true;
                }
                if (mainKey(e, "down", {"down"})) {
                    row = std::min(2, row + 1);
                    clampProcessCol(row, col);
                    return true;
                }
                if (mainKey(e, "play", {"enter"})) {
                    if (pending_confirmation == PendingConfirmation::Normalization) {
                        chooseNormalizationOption(row, col);
                    } else {
                        chooseConvertOption(row, col);
                    }
                    if (row == 2 && col == 0) {
                        acceptConfirmation();
                    } else if (row == 2) {
                        rejectConfirmation();
                    }
                    return true;
                }
            }

            if (mainKey(e, "left", {"left", "shift_tab"}) ||
                mainKey(e, "right", {"right", "tab"})) {
                confirmation_selected = confirmation_selected == 0 ? 1 : 0;
                return true;
            }

            if (mainKey(e, "play", {"enter"})) {
                if (confirmation_selected == 0) {
                    acceptConfirmation();
                } else {
                    rejectConfirmation();
                }
                return true;
            }

            if (mainKey(e, "confirm_yes", {"y", "Y", "н", "Н"})) {
                confirmation_selected = 0;
                acceptConfirmation();
                return true;
            }

            if (mainKey(e, "confirm_no", {"escape", "n", "N", "т", "Т"})) {
                confirmation_selected = 1;
                rejectConfirmation();
                return true;
            }
            return true;
        }

        if (e.is_mouse() && e.mouse().motion == Mouse::Moved) {
            HoverControl next_hover = controlAtMouse(e.mouse());
            bool hover_changed = hovered_control != next_hover;
            hovered_control = next_hover;
            if (next_hover != HoverControl::None) {
                return true;
            }

            if (move_to_mode && paneAtMouse(e.mouse()) != 0) {
                return hover_changed;
            }

            bool selected = selectVisibleRowAtMouse(e.mouse()).first != -1;
            return selected || hover_changed;
        }

        if (e.is_mouse() &&
            (e.mouse().button == Mouse::WheelUp ||
             e.mouse().button == Mouse::WheelDown)) {
            int pane = paneAtMouse(e.mouse());
            if (pane != -1) {
                moveBrowserSelection(
                    e.mouse().button == Mouse::WheelDown ? 1 : -1,
                    pane);
                return true;
            }
        }

        if (e.is_mouse() &&
            e.mouse().button == Mouse::Left &&
            e.mouse().motion == Mouse::Pressed) {
            if (download_close_box.Contain(e.mouse().x, e.mouse().y)) {
                show_download_panel = false;
                return true;
            }
            if (stems_close_box.Contain(e.mouse().x, e.mouse().y)) {
                show_stems_panel = false;
                return true;
            }
            if (audio_process_close_box.Contain(e.mouse().x, e.mouse().y)) {
                show_audio_process_panel = false;
                return true;
            }
            if (auto_cue_close_box.Contain(e.mouse().x, e.mouse().y)) {
                show_auto_cue_panel = false;
                return true;
            }

            HoverControl control = controlAtMouse(e.mouse());
            if (activateControl(control)) {
                hovered_control = control;
                return true;
            }

            if (command_paste_box.Contain(e.mouse().x, e.mouse().y)) {
                pasteClipboardToInput();
                return true;
            }

            if (command_clear_box.Contain(e.mouse().x, e.mouse().y)) {
                command_input.clear();
                root_selector = 2;
                bottom_selector = 0;
                return true;
            }

            if (now_playing_box.Contain(e.mouse().x, e.mouse().y)) {
                auto playback = controller.playbackSnapshot();
                std::string value = playback.title.empty() ? std::string("TPlay") : playback.title;
                command_status = copyToClipboard(value)
                    ? "Copied: " + truncateEnd(value, 40)
                    : "Clipboard unavailable";
                return true;
            }

            if (!move_to_mode) {
                if (auto column = sortColumnAtMouse(e.mouse())) {
                    toggleTrackSort(*column);
                    return true;
                }
            }

            if (move_to_mode && paneAtMouse(e.mouse()) != 0) {
                return true;
            }

            auto selected = selectVisibleRowAtMouse(e.mouse());
            if (selected.first == -1) {
                return false;
            }
            int click_pane = selected.first;
            int click_item = selected.second;
            auto now = std::chrono::steady_clock::now();
            bool is_double_click =
                click_pane == last_click_pane &&
                click_item == last_click_item &&
                now - last_click_time <= std::chrono::milliseconds(450);

            last_click_pane = click_pane;
            last_click_item = click_item;
            last_click_time = now;

            if (is_double_click) {
                if (click_pane == 0) {
                    state.focus = FocusPane::Directories;
                    browser_selector = 0;
                    openSelectedDirectory();
                } else {
                    state.focus = FocusPane::Tracks;
                    browser_selector = 1;
                    playSelectedTrack();
                }
            }
            return true;
        }

        if (move_to_mode && mainKey(e, "cancel", {"escape"})) {
            command_status = "Move cancelled";
            finishKeyboardMove();
            return true;
        }

        bool command_active = root_selector == 2 && bottom_selector == 0;
        if (command_active) {
            if (mainKey(e, "cancel", {"escape"})) {
                root_selector = 0;
                return true;
            }
            return false;
        }

        if (mainKey(e, "focus_progress", {"/"})) {
            root_selector = 1;
            return true;
        }
        if (mainKey(e, "focus_speed", {"?"})) {
            root_selector = 2;
            bottom_selector = 1;
            speed_focus = 1;
            return true;
        }
        if (mainKey(e, "focus_input", {"i", "I", "ш", "Ш"})) {
            root_selector = 2;
            bottom_selector = 0;
            return true;
        }
        if (mainKey(e, "paste_input", {"v", "V", "м", "М"})) {
            pasteClipboardToInput();
            return true;
        }
        if (mainKey(e, "speed_reset", {":"})) {
            speed_value = 100;
            controller.setPlaybackRate(1.0);
            root_selector = 2;
            bottom_selector = 1;
            speed_focus = 0;
            return true;
        }
        if (mainKey(e, "pitch_lock", {"\""})) {
            preserve_pitch = !preserve_pitch;
            controller.setPreservePitch(preserve_pitch);
            root_selector = 2;
            bottom_selector = 1;
            speed_focus = 2;
            return true;
        }
        if (mainKey(e, "help", {"h", "H", "р", "Р"})) {
            show_info = !show_info;
            return true;
        }
        if (mainKey(e, "equalizer", {"e", "E", "у", "У"})) {
            show_eq_popup = true;
            eq_selected = 0;
            return true;
        }
        if (!move_to_mode && mainKey(e, "sort_title", {"1"})) {
            toggleTrackSort(TrackSortColumn::Title);
            return true;
        }

        if (!move_to_mode && mainKey(e, "telegram", {"t", "T", "е", "Е"})) {
            if (!controller.config().telegram.enabled) {
                command_status = "Telegram disabled in config";
                return true;
            }
            std::string telegram_root = controller.telegramRootPath();
            controller.scanDirectory(telegram_root, true);
            expanded_directory = telegram_root;
            preferred_directory = telegram_root;
            state.selectedTrack = 0;
            state.focus = FocusPane::Directories;
            browser_selector = 0;
            root_selector = 0;
            command_status = "Telegram";
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_time", {"2"})) {
            toggleTrackSort(TrackSortColumn::Time);
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_bpm", {"3"})) {
            toggleTrackSort(TrackSortColumn::Bpm);
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_key", {"4"})) {
            toggleTrackSort(TrackSortColumn::Key);
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_kbps", {"5"})) {
            toggleTrackSort(TrackSortColumn::Bitrate);
            return true;
        }
        
        if (!move_to_mode && mainKey(e, "sort_size", {"6"})) {
            toggleTrackSort(TrackSortColumn::Size);
            return true;
        }

        if (!move_to_mode && mainKey(e, "sort_rate", {"7"})) {
            toggleTrackSort(TrackSortColumn::SampleRate);
            return true;
        }

        if (root_selector == 1 && mainKey(e, "left", {"left"})) {
            return seekPlaybackBySeconds(-5.0);
        }
        if (root_selector == 1 && mainKey(e, "right", {"right"})) {
            return seekPlaybackBySeconds(5.0);
        }
        if (mainKey(e, "seek_back_15", {",", "б"})) {
            return seekPlaybackBySeconds(-15.0);
        }
        if (mainKey(e, "seek_forward_15", {".", "ю"})) {
            return seekPlaybackBySeconds(15.0);
        }
        if (mainKey(e, "seek_back_30", {"<", "Б"})) {
            return seekPlaybackBySeconds(-30.0);
        }
        if (mainKey(e, "seek_forward_30", {">", "Ю"})) {
            return seekPlaybackBySeconds(30.0);
        }

        if (root_selector == 2 && bottom_selector == 1) {
            if (mainKey(e, "left", {"left"})) {
                if (speed_focus == 1) {
                    speed_value = std::max(speed_min, speed_value - speed_step);
                    controller.setPlaybackRate((double)speed_value / 100.0);
                }
                return true;
            }
            if (mainKey(e, "right", {"right"})) {
                if (speed_focus == 1) {
                    speed_value = std::min(speed_max, speed_value + speed_step);
                    controller.setPlaybackRate((double)speed_value / 100.0);
                }
                return true;
            }
            if (mainKey(e, "up", {"up"})) {
                if (speed_focus > 0) {
                    speed_focus--;
                } else {
                    root_selector = 0;
                }
                return true;
            }
            if (mainKey(e, "down", {"down"})) {
                if (speed_focus < 2) {
                    speed_focus++;
                } else {
                    root_selector = 0;
                }
                return true;
            }
            if (mainKey(e, "play", {"enter"})) {
                if (speed_focus == 0) {
                    speed_value = 100;
                    controller.setPlaybackRate(1.0);
                } else if (speed_focus == 2) {
                    preserve_pitch = !preserve_pitch;
                    controller.setPreservePitch(preserve_pitch);
                }
                return true;
            }
        }

        if (root_selector == 1 && mainKey(e, "down", {"down"})) {
            root_selector = 0;
            return true;
        }

        if (mainKey(e, "left", {"left"}) && root_selector == 0) {
            if (browser_selector == 1) {
                state.focus = FocusPane::Directories;
                browser_selector = 0;
            } else {
                goToParentDirectory();
            }
            root_selector = 0;
            return true;
        }

        if (mainKey(e, "right", {"right"}) && root_selector == 0) {
            if (browser_selector == 0) {
                bool selected_directory_is_open =
                    !expanded_directory.empty() &&
                    state.selectedDirectory >= 0 &&
                    state.selectedDirectory < (int)dir_paths.size() &&
                    fs::path(dir_paths[state.selectedDirectory]) ==
                        fs::path(controller.currentPath());
                if (selected_directory_is_open && !move_to_mode) {
                    state.focus = FocusPane::Tracks;
                    browser_selector = 1;
                } else {
                    openSelectedDirectory();
                }
            } else if (browser_selector == 1 && !move_to_mode) {
                playSelectedTrack();
            }
            return true;
        }

        if (mainKey(e, "switch_pane", {"tab"}) && root_selector == 0) {
            if (move_to_mode) {
                return true;
            }
            browser_selector = browser_selector == 0 ? 1 : 0;
            state.focus = browser_selector == 0 ? FocusPane::Directories : FocusPane::Tracks;
            return true;
        }

        if (mainKey(e, "play", {"enter"})) {
            if (root_selector == 0 && browser_selector == 0) {
                if (move_to_mode) {
                    moveTrackToSelectedDirectory();
                    return true;
                }
                openSelectedDirectory();
                return true;
            }
            if (root_selector == 0 && browser_selector == 1) {
                playSelectedTrack();
                return true;
            }
        }

        if (mainKey(e, "move", {"d", "D", "в", "В"}) &&
            root_selector == 0 &&
            browser_selector == 1 &&
            state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            move_track = visible_files[state.selectedTrack];
            move_to_mode = true;
            command_status = "Move: " + move_track.title +
                " | Enter place | Esc cancel";
            state.focus = FocusPane::Directories;
            browser_selector = 0;
            root_selector = 0;
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "drag", {"g", "G", "п", "П"}) &&
            root_selector == 0 &&
            browser_selector == 1 &&
            state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            std::string error;
            const Track& track = visible_files[state.selectedTrack];
            if (controller.startExternalDrag(track, error)) {
                command_status = "Drag ready: " + track.title;
            } else {
                command_status = "Error: " + error;
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "open_folder", {"f", "F", "а", "А"}) &&
            root_selector == 0) {
            std::string folder = controller.currentPath();
            if (browser_selector == 0 &&
                state.selectedDirectory >= 0 &&
                state.selectedDirectory < (int)dir_paths.size() &&
                fs::is_directory(dir_paths[state.selectedDirectory])) {
                folder = dir_paths[state.selectedDirectory];
            }

            std::string error;
            if (controller.openFolderExternally(folder, error)) {
                command_status = "Opened folder: " + displayName(folder);
            } else {
                command_status = "Error: " + error;
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "demucs", {"s", "S", "ы", "Ы"}) &&
            root_selector == 0 &&
            browser_selector == 1 &&
            state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            const Track& track = visible_files[state.selectedTrack];
            pending_stem_track = track;
            pending_stem_config = controller.config().demucs;
            stem_option_row = 0;
            stem_option_col = pending_stem_config.stems == 4 ? 1 : 0;
            pending_confirmation = PendingConfirmation::StemSeparation;
            command_status = "Confirm Demucs separation";
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "normalize", {"n", "N", "т", "Т"}) &&
            root_selector == 0) {
            pending_process_track = Track{};
            pending_process_all_folder = true;
            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_process_track = visible_files[state.selectedTrack];
                pending_process_all_folder = false;
            }
            pending_normalize_lufs = -14;
            pending_normalize_mode = "Short-Term Max";
            normalize_option_row = 0;
            normalize_option_col = 1;
            pending_confirmation = PendingConfirmation::Normalization;
            command_status = "Confirm normalization";
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "convert", {"c", "C", "с", "С"}) &&
            root_selector == 0) {
            pending_process_track = Track{};
            pending_process_all_folder = true;
            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_process_track = visible_files[state.selectedTrack];
                pending_process_all_folder = false;
            }
            pending_convert_format = "mp3";
            convert_option_row = 0;
            convert_option_col = 1;
            pending_confirmation = PendingConfirmation::Convert;
            command_status = "Confirm convert";
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "auto_cue", {"x", "X", "ч", "Ч"}) &&
            root_selector == 0) {
            pending_auto_cue_track = Track{};
            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_auto_cue_track = visible_files[state.selectedTrack];
            }
            auto_cue_option_col = pending_auto_cue_track.id.empty() ? 1 : 0;
            pending_confirmation = PendingConfirmation::AutoCue;
            command_status = "Auto Cue";
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "manual_cues", {"z", "Z", "я", "Я"}) &&
            root_selector == 0 &&
            browser_selector == 1) {
            if (state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                prepareEditor(visible_files[state.selectedTrack],
                              EditorPrepareTarget::ManualCues);
            } else {
                command_status = "Select a track for manual cues";
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "sync_cues", {"w", "W", "ц", "Ц"}) &&
            root_selector == 0 &&
            browser_selector == 1) {
            if (state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                std::string result;
                if (controller.syncCueMetadata(visible_files[state.selectedTrack], result)) {
                    command_status = result;
                } else {
                    command_status = "Cue sync error: " + result;
                }
            } else {
                command_status = "Select a track to sync cues";
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "trim", {"a", "A", "ф", "Ф"}) &&
            root_selector == 0 &&
            browser_selector == 1) {
            if (state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                prepareEditor(visible_files[state.selectedTrack],
                              EditorPrepareTarget::Trim);
            } else {
                command_status = "Select a track to trim";
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "metadata", {"m", "M", "ь", "Ь"}) &&
            root_selector == 0 &&
            browser_selector == 1 &&
            state.selectedTrack >= 0 &&
            state.selectedTrack < (int)visible_files.size()) {
            const Track& track = visible_files[state.selectedTrack];
            metadata_title = track.title;
            metadata_details = controller.metadataDetails(track);
            metadata_offset = 0;
            show_metadata_popup = true;
            command_status = "Metadata: " + track.title;
            return true;
        }

        if (mainKey(e, "toggle_playback", {"space"})) {
            togglePlayback();
            return true;
        }

        if (mainKey(e, "previous", {"[", "х", "Х"})) {
            controller.playPreviousTrack();
            return true;
        }

        if (mainKey(e, "next", {"]", "ъ", "Ъ"})) {
            controller.playNextTrack();
            return true;
        }
        if (mainKey(e, "refresh", {"r", "R", "к", "К"})) {
            controller.scanDirectory(controller.currentPath(), true);
            return true;
        }

        if (!move_to_mode && mainKey(e, "export_library", {"l", "L", "д", "Д"})) {
            std::string result;
            command_status = "Exporting library";
            if (controller.exportLibrary(result, true)) {
                command_status = result;
            } else {
                command_status = "Export error: " + result;
            }
            return true;
        }

        if (!move_to_mode && mainKey(e, "import_library", {"u", "U", "г", "Г"})) {
            std::string result;
            command_status = "Importing JSON sync";
            if (controller.importChangedJson(result)) {
                command_status = result;
                controller.scanDirectory(controller.currentPath(), true);
            } else {
                command_status = "Import error: " + result;
            }
            return true;
        }

        if (!move_to_mode &&
            mainKey(e, "validate_library_export", {"k", "K", "л", "Л"})) {
            std::string result;
            if (controller.validateLibraryExport(result)) {
                command_status = result;
            } else {
                command_status = "Export check error: " + result;
            }
            return true;
        }

        if (mainKey(e, "playback_mode", {"p", "P", "з", "З"})) {
            controller.cyclePlaybackMode();
            return true;
        }

        if (mainKey(e, "stop", {"o", "O", "щ", "Щ"})) {
            controller.stopPlayback();
            return true;
        }

        if (mainKey(e, "delete", {"backspace", "delete"})) {
            if (root_selector != 0) {
                return false;
            }

            if (browser_selector == 1 &&
                state.selectedTrack >= 0 &&
                state.selectedTrack < (int)visible_files.size()) {
                pending_delete_entry = visible_files[state.selectedTrack];
            } else if (browser_selector == 0 &&
                       state.selectedDirectory >= 0 &&
                       state.selectedDirectory < (int)dir_paths.size()) {
                pending_delete_entry = Track{};
                pending_delete_entry.id = dir_paths[state.selectedDirectory];
                pending_delete_entry.title = displayName(pending_delete_entry.id);
                pending_delete_entry.type = EntryType::Directory;
            } else {
                command_status = "Select a track or folder to delete";
                return true;
            }

            pending_confirmation = PendingConfirmation::DeleteEntry;
            command_status = "Confirm delete: " + pending_delete_entry.title;
            return true;
        }

        if (mainKey(e, "volume_down", {"-", "_"})) {
            controller.volumeDown();
            return true;
        }

        if (mainKey(e, "volume_up", {"=", "+"})) {
            controller.volumeUp();
            return true;
        }

        if (mainKey(e, "toggle_activity", {"y", "Y", "н", "Н"})) {
            show_activity = !show_activity;
            return true;
        }

        if (mainKey(e, "quit", {"q", "Q", "й", "Й"})) {
            pending_confirmation = PendingConfirmation::Quit;
            command_status = "Confirm quit";
            return true;
        }

        if (mainKey(e, "down", {"down"})) {
            if (move_to_mode) {
                moveBrowserSelection(1, 0);
                return true;
            }
            if (root_selector == 0) {
                if (moveBrowserSelection(1, browser_selector)) {
                    return true;
                }
                return true;
            }
        }

        if (mainKey(e, "up", {"up"})) {
            if (move_to_mode) {
                moveBrowserSelection(-1, 0);
                return true;
            }
            if (root_selector == 0) {
                if (moveBrowserSelection(-1, browser_selector)) {
                    return true;
                }
                return true;
            }
        }

        return false;
    });

    std::thread refresh_thread([&]() {
        bool playback_was_active = false;
        bool stems_was_active = false;
        bool audio_process_was_active = false;
        bool auto_cue_was_active = false;
        auto last_metadata_refresh = std::chrono::steady_clock::now() -
            std::chrono::seconds(1);
        int ui_fps = std::clamp(controller.config().fps, 1, 30);
        auto manual_frame_delay = std::chrono::milliseconds(
            std::max(1, 1000 / ui_fps));
        while (refresh_running.load()) {
            bool manual_active =
                manual_refresh_active.load() || trim_refresh_active.load();
            std::this_thread::sleep_for(manual_active
                ? manual_frame_delay
                : std::chrono::milliseconds(250));
            if (!refresh_running.load()) {
                break;
            }
            auto snapshot = controller.playbackSnapshot();
            auto download = controller.downloadSnapshot();
            auto stems = controller.stemSeparationSnapshot();
            auto audio_process = controller.audioProcessSnapshot();
            auto auto_cue = controller.autoCueSnapshot();
            bool playback_active =
                snapshot.state == PlaybackState::Playing ||
                snapshot.state == PlaybackState::Paused;
            bool stems_active = stems.state == StemSeparationState::Running;
            bool audio_process_active =
                audio_process.state == AudioProcessState::Running;
            bool auto_cue_active = auto_cue.running;
            bool editor_prepare_running = editor_prepare_active.load();
            bool metadata_active = controller.metadataBusy();
            auto now = std::chrono::steady_clock::now();
            bool metadata_refresh_due =
                metadata_active &&
                now - last_metadata_refresh >= std::chrono::seconds(1);
            bool stems_finished =
                stems_was_active &&
                !stems_active &&
                (stems.state == StemSeparationState::Done ||
                 stems.state == StemSeparationState::Error);
            if (stems_finished) {
                controller.scanDirectory(controller.currentPath(), true);
            }
            if (playback_active ||
                playback_was_active ||
                download.state == DownloadState::Running ||
                stems_active ||
                stems_was_active ||
                audio_process_active ||
                audio_process_was_active ||
                auto_cue_active ||
                auto_cue_was_active ||
                manual_active ||
                editor_prepare_running ||
                controller.directoryScanBusy() ||
                metadata_refresh_due) {
                if (metadata_refresh_due) {
                    last_metadata_refresh = now;
                }
                screen.Post([&screen] {
                    screen.PostEvent(Event::Custom);
                });
            }
            playback_was_active = playback_active;
            stems_was_active = stems_active;
            audio_process_was_active = audio_process_active;
            auto_cue_was_active = auto_cue_active;
        }
    });

    screen.Loop(component);
    if (editor_prepare_worker.joinable()) {
        editor_prepare_worker.request_stop();
        editor_prepare_worker.join();
    }
    refresh_running = false;
    refresh_thread.join();
    return 0;
}
