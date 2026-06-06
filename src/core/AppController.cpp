#include "AppController.hpp"

#include "ProcessRunner.hpp"
#include "../Sync/JsonSync.h"
#include "../Sync/ConflictResolver.h"
#include "../Export/ExportValidator.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {

constexpr const char* kTelegramRoot = "telegram://root";
constexpr const char* kTelegramChatPrefix = "telegram://chat/";
constexpr const char* kTelegramItemPrefix = "telegram://item/";

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string telegramChatPath(const std::string& chatId)
{
    return std::string(kTelegramChatPrefix) + chatId;
}

std::string telegramItemPath(const std::string& chatId, int messageId)
{
    return std::string(kTelegramItemPrefix) + chatId + "/" + std::to_string(messageId);
}

std::optional<std::pair<std::string, int>> parseTelegramItemPath(const std::string& path)
{
    if (!startsWith(path, kTelegramItemPrefix)) {
        return std::nullopt;
    }
    std::string rest = path.substr(std::string(kTelegramItemPrefix).size());
    std::size_t slash = rest.rfind('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }
    std::string chat_id = rest.substr(0, slash);
    int message_id = 0;
    try {
        message_id = std::stoi(rest.substr(slash + 1));
    } catch (...) {
        return std::nullopt;
    }
    return std::pair<std::string, int>{chat_id, message_id};
}

}  // namespace

std::string AppController::currentPath() const {
    std::lock_guard<std::mutex> lock(currentPathMutex_);
    return currentPath_;
}

void AppController::setCurrentPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(currentPathMutex_);
    currentPath_ = path;
}
// ---------------- utils ----------------

bool AppController::isAllowedFormat(const std::string& ext) const {
    std::string normalized = ext;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    for (const auto& f : config_.formats) {
        std::string format = f;
        std::transform(format.begin(), format.end(), format.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });

        if (normalized == "." + format)
            return true;
    }
    return false;
}

bool AppController::isTelegramPath(const std::string& path) const
{
    return path == kTelegramRoot ||
           startsWith(path, kTelegramChatPrefix) ||
           startsWith(path, kTelegramItemPrefix);
}

std::string AppController::telegramRootPath() const
{
    return kTelegramRoot;
}

bool AppController::scanTelegramDirectory(const std::string& path,
                                          std::vector<Track>& tracks,
                                          std::string& error)
{
    if (!telegramInbox_) {
        error = "Telegram is not configured";
        return false;
    }

    if (path == kTelegramRoot) {
        if (config_.telegram.syncOnOpenFolder) {
            TelegramSyncSummary summary;
            std::string sync_error;
            telegramInbox_->sync(summary, sync_error);
        }

        auto chats = telegramInbox_->listChats(error);
        if (!error.empty()) {
            return false;
        }
        for (const auto& chat : chats) {
            Track t;
            t.id = telegramChatPath(chat.chatId);
            t.title = chat.title.empty() ? chat.chatId : chat.title;
            t.type = EntryType::Directory;
            t.status = TrackStatus::Ready;
            tracks.push_back(std::move(t));
        }
        return true;
    }

    if (startsWith(path, kTelegramChatPrefix)) {
        std::string chat_id = path.substr(std::string(kTelegramChatPrefix).size());
        auto items = telegramInbox_->listAudioItems(chat_id, error);
        if (!error.empty()) {
            return false;
        }
        for (const auto& item : items) {
            Track t;
            t.id = telegramItemPath(item.chatId, item.messageId);
            t.title = item.fileName.empty()
                ? (item.title.empty() ? ("Telegram " + std::to_string(item.messageId))
                                      : item.title)
                : item.fileName;
            t.duration = item.duration;
            t.sizeBytes = item.fileSize;
            t.type = EntryType::File;
            t.status = item.downloaded ? TrackStatus::Ready : TrackStatus::Downloading;
            if (item.downloaded && !item.localPath.empty()) {
                t.id = item.localPath.string();
                t.status = TrackStatus::Ready;
            }
            tracks.push_back(std::move(t));
        }
        return true;
    }

    error = "Unsupported Telegram path";
    return false;
}

bool AppController::needsMetadataScan(const Track& track) {
    if (track.type != EntryType::File) {
        return false;
    }
    return track.bpm <= 0.0 || track.key.empty();
}

void AppController::mergeMetadataIntoTrack(Track& track,
                                           const AudioMetadata& metadata) {
    if (metadata.duration > 0.0) {
        track.duration = metadata.duration;
    }
    if (metadata.bpm > 0.0) {
        track.bpm = metadata.bpm;
    }
    if (metadata.bitrateKbps > 0.0) {
        track.bitrateKbps = metadata.bitrateKbps;
    }
    if (metadata.sampleRateHz > 0.0) {
        track.sampleRateHz = metadata.sampleRateHz;
    }
    if (metadata.sizeBytes > 0) {
        track.sizeBytes = metadata.sizeBytes;
    }
    if (!metadata.key.empty()) {
        track.key = metadata.key;
    }
    if (!metadata.genre.empty()) {
        track.genre = metadata.genre;
    }
}

std::string cueColorHex(std::uint32_t color)
{
    std::ostringstream stream;
    stream << "#" << std::hex << std::setfill('0') << std::setw(6)
           << (color & 0xffffffu);
    return stream.str();
}

std::uint32_t cueColorFromString(std::string color)
{
    if (color.starts_with("#")) {
        color.erase(color.begin());
    } else if (color.starts_with("0x") || color.starts_with("0X")) {
        color.erase(0, 2);
    }
    std::uint32_t value = 0xffffffu;
    if (color.size() == 6) {
        std::from_chars(color.data(), color.data() + color.size(), value, 16);
    }
    return value;
}

std::vector<SeratoCue> seratoCuesFromLibraryTrack(const LibraryTrack& track)
{
    std::vector<SeratoCue> cues;
    cues.reserve(track.cues.size());
    for (const auto& cue : track.cues) {
        if (cue.index < 0 || cue.index > 7 || cue.positionSeconds < 0.0) {
            continue;
        }
        cues.push_back({
            cue.index,
            cue.name,
            cue.positionSeconds,
            cueColorFromString(cue.color),
        });
    }
    std::sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) {
        if (a.index != b.index) {
            return a.index < b.index;
        }
        return a.seconds < b.seconds;
    });
    return cues;
}

bool cuePositionsMatch(std::vector<SeratoCue> expected,
                       std::vector<SeratoCue> actual)
{
    auto sort_cues = [](std::vector<SeratoCue>& cues) {
        std::sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) {
            if (a.index != b.index) {
                return a.index < b.index;
            }
            return a.seconds < b.seconds;
        });
    };
    sort_cues(expected);
    sort_cues(actual);
    if (expected.size() != actual.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i].index != actual[i].index ||
            std::abs(expected[i].seconds - actual[i].seconds) > 0.02) {
            return false;
        }
    }
    return true;
}

std::vector<SeratoCue> sortedCues(std::vector<SeratoCue> cues)
{
    std::sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) {
        if (a.index != b.index) {
            return a.index < b.index;
        }
        return a.seconds < b.seconds;
    });
    return cues;
}

std::vector<SeratoCue> chooseCueSyncSource(
    const std::vector<SeratoCue>& serato,
    const std::vector<SeratoCue>& traktor,
    const std::string& prefer)
{
    if (serato.empty()) {
        return sortedCues(traktor);
    }
    if (traktor.empty()) {
        return sortedCues(serato);
    }
    if (cuePositionsMatch(serato, traktor)) {
        return sortedCues(serato);
    }
    if (prefer == "serato") {
        return sortedCues(serato);
    }
    if (prefer == "traktor") {
        return sortedCues(traktor);
    }
    return sortedCues(serato);
}

std::int64_t nowUnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string cueHash(std::vector<SeratoCue> cues)
{
    cues = sortedCues(std::move(cues));
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&](unsigned char byte) {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    auto mixString = [&](const std::string& value) {
        for (unsigned char c : value) {
            mix(c);
        }
        mix(0xff);
    };
    for (const auto& cue : cues) {
        mix((unsigned char)std::clamp(cue.index, 0, 255));
        auto millis = (std::int64_t)std::llround(std::max(0.0, cue.seconds) * 1000.0);
        for (int i = 0; i < 8; ++i) {
            mix((unsigned char)((millis >> (i * 8)) & 0xff));
        }
        mixString(cue.name);
    }
    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}

std::string syncStateKey(const std::string& trackId,
                         const std::string& source,
                         const std::string& field)
{
    return "cue_sync:" + trackId + ":" + source + ":" + field;
}

std::string syncStateGet(sqlite3* db, const std::string& key)
{
    if (db == nullptr) {
        return {};
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT value FROM sync_state WHERE key = ? LIMIT 1;",
                           -1,
                           &stmt,
                           nullptr) != SQLITE_OK) {
        return {};
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string value;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text != nullptr) {
            value = (const char*)text;
        }
    }
    sqlite3_finalize(stmt);
    return value;
}

void syncStateSet(sqlite3* db, const std::string& key, const std::string& value)
{
    if (db == nullptr) {
        return;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO sync_state(key, value) VALUES(?, ?) "
                           "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
                           -1,
                           &stmt,
                           nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::int64_t syncStateTime(sqlite3* db,
                           const std::string& trackId,
                           const std::string& source)
{
    std::string value = syncStateGet(db, syncStateKey(trackId, source, "time"));
    if (value.empty()) {
        return 0;
    }
    try {
        return std::stoll(value);
    } catch (...) {
        return 0;
    }
}

void syncStateTouch(sqlite3* db,
                    const std::string& trackId,
                    const std::string& source,
                    const std::string& hash,
                    std::int64_t timestamp)
{
    syncStateSet(db, syncStateKey(trackId, source, "hash"), hash);
    syncStateSet(db, syncStateKey(trackId, source, "time"), std::to_string(timestamp));
}

fs::path expandUserPath(fs::path path)
{
    std::string value = path.string();
    if (value == "~" || value.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home != nullptr && *home != '\0') {
            return fs::path(std::string(home) + value.substr(1));
        }
    }
    return path;
}

// ---------------- ctor ----------------

AppController::AppController() {
    fs::path config_path = fs::path(ProcessRunner::executableDirectory()) / "config.toml";
    std::error_code ec;
    if (!fs::is_regular_file(config_path, ec)) {
        config_path = fs::current_path() / "config.toml";
    }
    config_ = Config::load(config_path.string());
    initializeLibrary();
    volume_ = std::clamp(config_.volume, 0, 100);
    audioEngine_.setVolume(volume_);
    previewAudioEngine_.setVolume(volume_);
    metadataWorker_ = std::jthread([this](std::stop_token stop_token) {
        metadataLoop(stop_token);
    });
    playbackWorker_ = std::jthread([this](std::stop_token stop_token) {
        playbackLoop(stop_token);
    });

    if (!config_.musicDirectories.empty()) {
        std::string initial_path = config_.musicDirectories[0];
        setCurrentPath(initial_path);
        initialScanWorker_ = std::jthread(
            [this, initial_path](std::stop_token stop_token) {
                if (!stop_token.stop_requested() &&
                    currentPath() == initial_path) {
                    scanDirectory(initial_path);
                }
            });
    }
    stemSeparator_.setOnFinished([this] {
        scanDirectory(currentPath(), true);
    });
}

AppController::~AppController() {
    autoCueCancel_ = true;
    if (initialScanWorker_.joinable()) {
        initialScanWorker_.request_stop();
    }
    if (autoCueWorker_.joinable()) {
        autoCueWorker_.request_stop();
    }
    playbackWorker_.request_stop();
    metadataWorker_.request_stop();
    metadataCv_.notify_all();
    stopPreviewPlayback();
    stopPlayback();
    if (autoCueWorker_.joinable()) {
        autoCueWorker_.join();
    }
    if (playbackWorker_.joinable()) {
        playbackWorker_.join();
    }
    if (metadataWorker_.joinable()) {
        metadataWorker_.join();
    }
    if (initialScanWorker_.joinable()) {
        initialScanWorker_.join();
    }
}

// ---------------- store ----------------

TrackStore& AppController::trackStore() {
    return trackStore_;
}

const Config& AppController::config() const {
    return config_;
}

void AppController::initializeLibrary() {
    if (!config_.library.enabled) {
        return;
    }

    std::string error;
    if (!libraryDatabase_.open(config_.library.databasePath, error)) {
        return;
    }
    if (!libraryDatabase_.initialize(error)) {
        return;
    }
    trackRepository_ = std::make_unique<TrackRepository>(libraryDatabase_);
    cueRepository_ = std::make_unique<CueRepository>(libraryDatabase_);
    if (config_.telegram.enabled && config_.telegram.mode == "bot") {
        telegramRepository_ = std::make_unique<TelegramRepository>(libraryDatabase_);
        telegramClient_ = std::make_unique<TelegramBotClient>(config_.telegram.botToken);
        telegramInbox_ = std::make_unique<TelegramInboxService>(
            config_, *telegramClient_, *telegramRepository_);
    }
}

void AppController::upsertLibraryTrack(const Track& track) {
    if (!trackRepository_ || track.type != EntryType::File || track.id.empty()) {
        return;
    }
    std::string error;
    LibraryTrack library_track = libraryTrackFromTrack(track);
    if (trackRepository_->upsertTrack(library_track, error)) {
        importSeratoCuesIfLibraryEmpty(track, library_track);
    }
}

void AppController::importSeratoCuesIfLibraryEmpty(
    const Track& track,
    const LibraryTrack& libraryTrack)
{
    if (!cueRepository_ || track.id.empty() || libraryTrack.id.empty()) {
        return;
    }

    std::string error;
    auto existing = cueRepository_->cuesForTrack(libraryTrack.id, error);
    if (!error.empty() || !existing.empty()) {
        return;
    }

    std::string serato_error;
    auto serato_cues = seratoCueWriter_.readCues(track.id, serato_error);
    std::vector<SeratoCue> embedded_cues = serato_cues;
    if (embedded_cues.empty()) {
        std::string traktor_error;
        embedded_cues = traktorMetadataWriter_.readCues(track.id, traktor_error);
    }
    if (embedded_cues.empty()) {
        return;
    }

    LibraryTrack imported = libraryTrackForCues(track, embedded_cues);
    if (imported.id != libraryTrack.id) {
        imported.id = libraryTrack.id;
    }
    cueRepository_->replaceCues(libraryTrack.id, imported.cues, error);
}

void AppController::replaceLibraryCues(const Track& track,
                                       const std::vector<SeratoCue>& cues) {
    if (!trackRepository_ || !cueRepository_ ||
        track.type != EntryType::File || track.id.empty()) {
        return;
    }

    LibraryTrack library_track = libraryTrackForCues(track, cues);
    std::string error;
    if (!trackRepository_->upsertTrack(library_track, error)) {
        return;
    }

    cueRepository_->replaceCues(library_track.id, library_track.cues, error);
}

LibraryTrack AppController::libraryTrackForCues(
    const Track& track,
    const std::vector<SeratoCue>& cues) const
{
    LibraryTrack library_track = libraryTrackFromTrack(track);
    library_track.cues.reserve(cues.size());
    for (const auto& cue : cues) {
        library_track.cues.push_back({
            cue.index,
            cue.name,
            "hotcue",
            cue.seconds,
            cueColorHex(cue.colorRgb),
        });
    }
    return library_track;
}

bool AppController::exportLibraryCollection(std::string& error)
{
    if (!trackRepository_ || !cueRepository_) {
        error = "Library database is disabled";
        return false;
    }
    if (!config_.library.exportRekordbox && !config_.library.exportTraktor) {
        return true;
    }

    LibraryExporter exporter(*trackRepository_, *cueRepository_, seratoCueWriter_);
    LibraryExportOptions options;
    options.exportSerato = false;
    options.exportRekordbox = config_.library.exportRekordbox;
    options.exportTraktor = config_.library.exportTraktor;
    options.exportJson = false;
    options.syncFolder = expandUserPath(config_.library.syncFolder);
    options.outputFolder = options.syncFolder / "exports";
    return exporter.exportAll(options, error);
}

bool AppController::exportLibraryCues(const LibraryTrack& track,
                                      std::string& error,
                                      bool updateCollectionExport)
{
    const auto expected_cues = seratoCuesFromLibraryTrack(track);

    if (config_.autoCue.writeSerato) {
        SeratoExportProvider serato(seratoCueWriter_,
                                    config_.autoCue.backupBeforeWrite,
                                    config_.autoCue.overwriteExistingCues);
        if (!serato.exportTrack(track, error)) {
            return false;
        }
    }

    if (config_.library.exportRekordbox) {
        RekordboxExportProvider rekordbox;
        if (!rekordbox.exportTrack(track, error)) {
            return false;
        }
    }

    if (config_.library.exportTraktor) {
        TraktorExportProvider traktor;
        if (!traktor.exportTrack(track, error)) {
            return false;
        }
    }

    if (config_.autoCue.writeTraktor) {
        TraktorEmbeddedMetadataStatus status;
        if (!traktorMetadataWriter_.writeCues(track, error, &status)) {
            return false;
        }
    }

    if (config_.autoCue.writeSerato) {
        std::string verify_error;
        auto written = seratoCueWriter_.readCues(track.path, verify_error);
        if (!verify_error.empty() || !cuePositionsMatch(expected_cues, written)) {
            error = "Serato cue verification failed";
            if (!verify_error.empty()) {
                error += ": " + verify_error;
            } else {
                error += ": expected " + std::to_string(expected_cues.size()) +
                    ", read " + std::to_string(written.size());
            }
            return false;
        }
    }

    if (config_.autoCue.writeTraktor) {
        std::string verify_error;
        auto written = traktorMetadataWriter_.readCues(track.path, verify_error);
        if (!verify_error.empty() || !cuePositionsMatch(expected_cues, written)) {
            error = "Traktor cue verification failed";
            if (!verify_error.empty()) {
                error += ": " + verify_error;
            } else {
                error += ": expected " + std::to_string(expected_cues.size()) +
                    ", read " + std::to_string(written.size());
            }
            return false;
        }
    }

    if (libraryDatabase_.isOpen()) {
        std::int64_t timestamp = nowUnixSeconds();
        std::string hash = cueHash(expected_cues);
        sqlite3* db = libraryDatabase_.handle();
        if (config_.autoCue.writeSerato) {
            syncStateTouch(db, track.id, "serato", hash, timestamp);
        }
        if (config_.autoCue.writeTraktor) {
            syncStateTouch(db, track.id, "traktor", hash, timestamp);
        }
    }

    if (config_.library.exportJson) {
        JsonSync sync;
        if (!sync.exportTrack(track, expandUserPath(config_.library.syncFolder), error)) {
            return false;
        }
    }
    if (updateCollectionExport && !exportLibraryCollection(error)) {
        return false;
    }
    return true;
}

bool AppController::saveAutoCueResult(const fs::path& file,
                                      const AutoCueResult& result,
                                      const std::vector<SeratoCue>& cues,
                                      std::string& error,
                                      bool updateCollectionExport)
{
    Track track;
    track.id = file.string();
    track.title = file.stem().string();
    track.type = EntryType::File;
    track.duration = result.duration;

    try {
        mergeMetadataIntoTrack(track, audioAnalyzer_.readEmbeddedMetadata(track.id));
    } catch (...) {
    }

    LibraryTrack library_track = libraryTrackForCues(track, cues);
    if (trackRepository_ && cueRepository_) {
        if (trackRepository_->upsertTrack(library_track, error)) {
            if (!cueRepository_->replaceCues(library_track.id, library_track.cues, error)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return exportLibraryCues(library_track, error, updateCollectionExport);
}

// ---------------- scanner ----------------

void AppController::scanDirectory(const std::string& path, bool forceRefresh) {
    struct ScanBusyGuard {
        std::atomic_int& count;
        explicit ScanBusyGuard(std::atomic_int& value) : count(value) {
            count.fetch_add(1);
        }
        ~ScanBusyGuard() {
            count.fetch_sub(1);
        }
    } scan_busy(directoryScansInFlight_);

    std::uint64_t scan_generation =
        directoryScanGeneration_.fetch_add(1) + 1;
    setCurrentPath(path);
    auto scanIsCurrent = [&] {
        return directoryScanGeneration_.load() == scan_generation &&
               currentPath() == path;
    };

    if (!forceRefresh) {
        std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
        auto cached = directoryCache_.find(path);
        if (cached != directoryCache_.end()) {
            if (!scanIsCurrent()) {
                return;
            }
            trackStore_.setTracks(cached->second);
            std::vector<Track> files;
            for (const auto& track : cached->second) {
                if (track.type == EntryType::File) {
                    files.push_back(track);
                }
            }
            {
                std::lock_guard<std::mutex> lock(playbackMutex_);
                displayedTracks_ = files;
            }
            std::vector<Track> metadata_files;
            for (const auto& file : files) {
                if (!isTelegramPath(file.id)) {
                    metadata_files.push_back(file);
                }
            }
            queueMetadataScan(path, metadata_files);
            return;
        }
    }

    std::vector<Track> dirs;
    std::vector<Track> files;

    if (isTelegramPath(path)) {
        std::vector<Track> tracks;
        std::string error;
        scanTelegramDirectory(path, tracks, error);
        for (const auto& track : tracks) {
            if (track.type == EntryType::Directory) {
                dirs.push_back(track);
            } else {
                files.push_back(track);
            }
        }
    } else
    try {
        for (const auto& entry : fs::directory_iterator(path)) {

            Track t;
            t.id = entry.path().string();
            t.title = entry.path().filename().string();

            // Skip hidden files
            if (t.title.empty() || t.title[0] == '.')
                continue;

            // DIRECTORY
            if (entry.is_directory()) {
                t.type = EntryType::Directory;
                t.status = TrackStatus::Ready;
                t.duration = 0;
                t.bpm = 0;
                t.key = "";
                dirs.push_back(t);
            }
            // FILE
            else {
                auto ext = entry.path().extension().string();

                if (!isAllowedFormat(ext))
                    continue;

                t.type = EntryType::File;
                t.status = TrackStatus::Ready;
                t.duration = 0;
                std::error_code ec;
                t.sizeBytes = entry.file_size(ec);
                if (ec) {
                    t.sizeBytes = 0;
                }
                files.push_back(t);
            }
        }
    } catch (const std::exception& e) {
        // Silently handle permission errors etc
    }

    // Sort directories by name, then add files
    std::sort(dirs.begin(), dirs.end(), 
              [](const Track& a, const Track& b) { return a.title < b.title; });
    std::sort(files.begin(), files.end(),
              [](const Track& a, const Track& b) { return a.title < b.title; });
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        if (scanIsCurrent()) {
            displayedTracks_ = files;
        }
    }

    std::vector<Track> tracks;
    tracks.reserve(dirs.size() + files.size());
    tracks.insert(tracks.end(), dirs.begin(), dirs.end());
    tracks.insert(tracks.end(), files.begin(), files.end());
    {
        std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
        directoryCache_[path] = tracks;
    }

    if (!scanIsCurrent()) {
        return;
    }
    trackStore_.setTracks(tracks);
    std::vector<Track> metadata_files;
    for (const auto& file : files) {
        if (!isTelegramPath(file.id)) {
            metadata_files.push_back(file);
        }
    }
    queueMetadataScan(path, metadata_files);
}

void AppController::updateCachedTrack(const std::string& directory,
                                      const Track& track) {
    std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
    auto cached = directoryCache_.find(directory);
    if (cached == directoryCache_.end()) {
        return;
    }
    for (auto& existing : cached->second) {
        if (existing.id == track.id) {
            existing = track;
            return;
        }
    }
}

void AppController::removeCachedEntry(const std::string& directory,
                                      const std::string& id) {
    std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
    auto cached = directoryCache_.find(directory);
    if (cached == directoryCache_.end()) {
        return;
    }
    cached->second.erase(
        std::remove_if(cached->second.begin(), cached->second.end(),
                       [&](const Track& track) { return track.id == id; }),
        cached->second.end());
}

void AppController::invalidateDirectoryCache(const std::string& path) {
    std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
    directoryCache_.erase(path);
}

void AppController::queueMetadataScan(const std::string& path,
                                      const std::vector<Track>& tracks) {
    std::vector<Track> pending;
    pending.reserve(tracks.size());
    for (const auto& track : tracks) {
        if (needsMetadataScan(track)) {
            pending.push_back(track);
        }
    }

    {
        std::lock_guard<std::mutex> lock(metadataMutex_);
        pendingMetadata_ = std::move(pending);
        pendingMetadataPath_ = path;
        metadataGeneration_++;
        metadataBusy_ = !pendingMetadata_.empty();
    }
    metadataCv_.notify_one();
}

void AppController::metadataLoop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        std::vector<Track> batch;
        std::string directory;
        unsigned long generation = 0;

        {
            std::unique_lock<std::mutex> lock(metadataMutex_);
            metadataCv_.wait(lock, stop_token, [&] {
                return !pendingMetadata_.empty();
            });
            if (stop_token.stop_requested()) {
                return;
            }
            batch = std::move(pendingMetadata_);
            pendingMetadata_.clear();
            directory = pendingMetadataPath_;
            generation = metadataGeneration_;
        }

        auto is_current_scan = [&] {
            std::lock_guard<std::mutex> lock(metadataMutex_);
            return generation == metadataGeneration_ &&
                   directory == pendingMetadataPath_ &&
                   directory == currentPath();
        };

        auto publish_track = [&](const Track& updated) {
            if (is_current_scan()) {
                trackStore_.updateTrack(updated);
            }
            updateCachedTrack(directory, updated);
            upsertLibraryTrack(updated);
            std::lock_guard<std::mutex> playback_lock(playbackMutex_);
            if (playingTrackId_ == updated.id) {
                playingTrack_ = updated;
            }
            for (auto& queued : playbackQueue_) {
                if (queued.id == updated.id) {
                    queued = updated;
                }
            }
        };

        std::vector<Track> analysis_batch;
        analysis_batch.reserve(batch.size());

        for (auto& track : batch) {
            if (stop_token.stop_requested() || !is_current_scan()) {
                break;
            }

            AudioMetadata metadata = audioAnalyzer_.readEmbeddedMetadata(track.id);
            if (metadata.succeeded) {
                mergeMetadataIntoTrack(track, metadata);
            }

            if (needsMetadataScan(track)) {
                analysis_batch.push_back(track);
            } else {
                track.status = TrackStatus::Ready;
                publish_track(track);
            }
        }

        for (auto& track : analysis_batch) {
            if (stop_token.stop_requested() || !is_current_scan()) {
                break;
            }

            const bool missing_bpm = track.bpm <= 0.0;
            const bool missing_key = track.key.empty();
            if (!missing_bpm && !missing_key) {
                track.status = TrackStatus::Ready;
                publish_track(track);
                continue;
            }

            track.status = TrackStatus::Analyzing;
            publish_track(track);

            AudioMetadata analyzed =
                audioAnalyzer_.analyzeWithEssentia(track.id);
            if (analyzed.succeeded) {
                if (track.duration <= 0.0 && analyzed.duration > 0.0) {
                    track.duration = analyzed.duration;
                }
                AudioMetadata write_metadata;
                write_metadata.succeeded = true;
                if (missing_bpm && analyzed.bpm > 0.0) {
                    track.bpm = analyzed.bpm;
                    write_metadata.bpm = analyzed.bpm;
                }
                if (missing_key && !analyzed.key.empty()) {
                    track.key = analyzed.key;
                    write_metadata.key = analyzed.key;
                }
                std::string write_error;
                metadataWriter_.write(track.id, write_metadata, write_error);
                track.status = TrackStatus::Ready;
            } else {
                track.status = TrackStatus::Error;
            }
            publish_track(track);
        }

        std::lock_guard<std::mutex> lock(metadataMutex_);
        if (generation == metadataGeneration_ && pendingMetadata_.empty()) {
            metadataBusy_ = false;
        }
    }
}
// ---------------- volume ----------------

int AppController::volume() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return volume_;
}

void AppController::volumeUp() {
    int next_volume = 0;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        volume_ = std::min(100, volume_ + 5);
        next_volume = volume_;
    }
    audioEngine_.setVolume(next_volume);
    previewAudioEngine_.setVolume(next_volume);
}

void AppController::volumeDown() {
    int next_volume = 0;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        volume_ = std::max(0, volume_ - 5);
        next_volume = volume_;
    }
    audioEngine_.setVolume(next_volume);
    previewAudioEngine_.setVolume(next_volume);
}

double AppController::playbackRate() const {
    return audioEngine_.playbackRate();
}

void AppController::setPlaybackRate(double rate) {
    audioEngine_.setPlaybackRate(rate);
}

bool AppController::preservePitch() const {
    return audioEngine_.preservePitch();
}

void AppController::setPreservePitch(bool preserve) {
    audioEngine_.setPreservePitch(preserve);
}

void AppController::setEqualizerGains(double lowDb, double midDb, double highDb) {
    audioEngine_.setEqualizerGains(lowDb, midDb, highDb);
}

bool AppController::resolveTelegramTrackForPlayback(const Track& track,
                                                    Track& localTrack,
                                                    std::string& error)
{
    localTrack = track;
    if (!isTelegramPath(track.id)) {
        return true;
    }
    if (!telegramInbox_) {
        error = "Telegram is not configured";
        return false;
    }

    auto parsed = parseTelegramItemPath(track.id);
    if (!parsed) {
        error = "Unsupported Telegram track";
        return false;
    }

    auto item = telegramInbox_->findAudioItem(parsed->first, parsed->second, error);
    if (!error.empty()) {
        return false;
    }
    if (!item) {
        error = "Telegram audio item was not found";
        return false;
    }

    if (!item->downloaded || item->localPath.empty() ||
        !fs::is_regular_file(item->localPath)) {
        if (!config_.telegram.downloadOnPlay) {
            error = "Telegram download_on_play is disabled";
            return false;
        }
        Track downloading = track;
        downloading.status = TrackStatus::Downloading;
        trackStore_.updateTrack(downloading);
        if (!telegramInbox_->downloadItem(*item, error)) {
            downloading.status = TrackStatus::Error;
            trackStore_.updateTrack(downloading);
            return false;
        }
    }

    localTrack.id = item->localPath.string();
    localTrack.title = item->fileName.empty() ? track.title : item->fileName;
    localTrack.type = EntryType::File;
    localTrack.status = TrackStatus::Ready;
    localTrack.duration = item->duration > 0 ? item->duration : track.duration;
    localTrack.sizeBytes = item->fileSize > 0 ? item->fileSize : track.sizeBytes;
    try {
        mergeMetadataIntoTrack(localTrack, audioAnalyzer_.readEmbeddedMetadata(localTrack.id));
    } catch (...) {
    }
    LibraryTrack imported_track = libraryTrackFromTrack(localTrack);
    item->importedTrackId = imported_track.id;
    std::string item_error;
    telegramRepository_->upsertAudioItem(*item, item_error);
    upsertLibraryTrack(localTrack);
    {
        std::string directory = currentPath();
        std::lock_guard<std::mutex> cache_lock(directoryCacheMutex_);
        auto cached = directoryCache_.find(directory);
        if (cached != directoryCache_.end()) {
            for (auto& existing : cached->second) {
                if (existing.id == track.id) {
                    existing = localTrack;
                    break;
                }
            }
        }
    }
    auto visible = trackStore_.getTracks();
    for (auto& existing : visible) {
        if (existing.id == track.id) {
            existing = localTrack;
            break;
        }
    }
    trackStore_.setTracks(visible);
    return true;
}

bool AppController::playTrack(const Track& track,
                              const std::vector<Track>& orderedTracks) {
    if (track.type != EntryType::File) {
        return false;
    }

    Track playable = track;
    std::string resolve_error;
    if (!resolveTelegramTrackForPlayback(track, playable, resolve_error)) {
        return false;
    }

    std::vector<Track> playback_tracks = orderedTracks;
    for (auto& queued : playback_tracks) {
        if (queued.id == track.id) {
            queued = playable;
        }
    }

    std::lock_guard<std::mutex> lock(playbackMutex_);
    if (!audioEngine_.play(playable.id, playable.title, volume_)) {
        return false;
    }

    auto in_ordered_tracks = std::find_if(
        playback_tracks.begin(), playback_tracks.end(),
        [&](const Track& displayed) { return displayed.id == playable.id; });
    playbackQueue_ = in_ordered_tracks != playback_tracks.end()
        ? playback_tracks
        : std::vector<Track>{playable};
    playingTrackId_ = playable.id;
    playingTrack_ = playable;
    return true;
}

bool AppController::playPreviewTrack(const Track& track) {
    if (track.type != EntryType::File) {
        return false;
    }

    std::lock_guard<std::mutex> lock(playbackMutex_);
    if (!previewAudioEngine_.play(track.id, track.title, volume_)) {
        return false;
    }
    previewPlayingTrackId_ = track.id;
    return true;
}

void AppController::togglePreviewPause() {
    previewAudioEngine_.togglePause();
}

void AppController::stopPreviewPlayback() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    previewAudioEngine_.stop();
    previewPlayingTrackId_.clear();
}

void AppController::seekPreviewPlayback(double ratio) {
    previewAudioEngine_.seekToRatio(ratio);
}

void AppController::setPreviewLoopRange(double startSeconds, double endSeconds) {
    previewAudioEngine_.setLoopRange(startSeconds, endSeconds);
}

void AppController::clearPreviewLoopRange() {
    previewAudioEngine_.clearLoopRange();
}

PlaybackSnapshot AppController::previewPlaybackSnapshot() const {
    return previewAudioEngine_.snapshot();
}

std::string AppController::previewPlayingTrackId() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return previewPlayingTrackId_;
}

double AppController::previewPlaybackRate() const {
    return previewAudioEngine_.playbackRate();
}

void AppController::setPreviewPlaybackRate(double rate) {
    previewAudioEngine_.setPlaybackRate(rate);
}

bool AppController::previewPreservePitch() const {
    return previewAudioEngine_.preservePitch();
}

void AppController::setPreviewPreservePitch(bool preserve) {
    previewAudioEngine_.setPreservePitch(preserve);
}

void AppController::togglePause() {
    audioEngine_.togglePause();
}

void AppController::stopPlayback() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    audioEngine_.stop();
    playingTrackId_.clear();
    playingTrack_ = {};
}

bool AppController::playPreviousTrack() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playRelativeTrackLocked(-1, false);
}

bool AppController::playNextTrack() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playRelativeTrackLocked(1, false);
}

void AppController::cyclePlaybackMode() {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    switch (playbackMode_) {
    case PlaybackMode::RepeatAll:
        playbackMode_ = PlaybackMode::Shuffle;
        break;
    case PlaybackMode::Shuffle:
        playbackMode_ = PlaybackMode::RepeatOne;
        break;
    case PlaybackMode::RepeatOne:
        playbackMode_ = PlaybackMode::RepeatAll;
        break;
    }
}

PlaybackMode AppController::playbackMode() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playbackMode_;
}

void AppController::seekPlayback(double ratio) {
    audioEngine_.seekToRatio(ratio);
}

PlaybackSnapshot AppController::playbackSnapshot() const {
    return audioEngine_.snapshot();
}

std::string AppController::playingTrackId() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playingTrackId_;
}

Track AppController::playingTrack() const {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    return playingTrack_;
}

void AppController::playbackLoop(std::stop_token stop_token) {
    auto last_output_check = std::chrono::steady_clock::now();
    while (!stop_token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto snapshot = audioEngine_.snapshot();
        bool playback_active = snapshot.state == PlaybackState::Playing ||
                               snapshot.state == PlaybackState::Paused;
        auto now = std::chrono::steady_clock::now();
        if (playback_active &&
            now - last_output_check >= std::chrono::seconds(1)) {
            audioEngine_.followSystemAudioOutput();
            last_output_check = now;
        }
        if (!audioEngine_.consumeFinishedNaturally()) {
            continue;
        }

        std::lock_guard<std::mutex> lock(playbackMutex_);
        playRelativeTrackLocked(1, true);
    }
}

bool AppController::playRelativeTrackLocked(int direction, bool natural_end) {
    if (playbackQueue_.empty()) {
        return false;
    }

    auto current = std::find_if(
        playbackQueue_.begin(), playbackQueue_.end(),
        [&](const Track& track) { return track.id == playingTrackId_; });
    int index = current == playbackQueue_.end()
        ? 0
        : (int)std::distance(playbackQueue_.begin(), current);

    if (natural_end && playbackMode_ == PlaybackMode::RepeatOne) {
        // Keep the current index.
    } else if (natural_end && playbackMode_ == PlaybackMode::Shuffle &&
               playbackQueue_.size() > 1) {
        std::uniform_int_distribution<int> distribution(0, (int)playbackQueue_.size() - 2);
        int random_index = distribution(randomGenerator_);
        index = random_index >= index ? random_index + 1 : random_index;
    } else {
        int count = (int)playbackQueue_.size();
        index = (index + direction + count) % count;
    }

    Track next = playbackQueue_[(size_t)index];
    std::string resolve_error;
    if (!resolveTelegramTrackForPlayback(next, next, resolve_error)) {
        return false;
    }
    playbackQueue_[(size_t)index] = next;
    if (!audioEngine_.play(next.id, next.title, volume_)) {
        return false;
    }
    playingTrackId_ = next.id;
    playingTrack_ = next;
    return true;
}

bool AppController::downloadToCurrentDirectory(const std::string& source,
                                               std::function<void()> on_finished) {
    std::string destination = currentPath();
    if (destination.empty() && !config_.musicDirectories.empty()) {
        destination = config_.musicDirectories.front();
    }

    return downloadToDirectory(source, destination, std::move(on_finished));
}

bool AppController::downloadToDirectory(const std::string& source,
                                        const std::string& directory,
                                        std::function<void()> on_finished) {
    std::error_code ec;
    std::string destination = fs::weakly_canonical(directory, ec).string();
    if (ec) {
        destination = directory;
    }
    return downloadManager_.start(source,
                                  destination,
                                  config_.downloadFormat,
                                  config_.ytdlp.cookiesFromBrowser,
                                  std::move(on_finished));
}

DownloadSnapshot AppController::downloadSnapshot() const {
    return downloadManager_.snapshot();
}

bool AppController::separateTrack(const Track& track) {
    return stemSeparator_.start(track, config_.demucs);
}

bool AppController::separateTrack(const Track& track, const DemucsConfig& config) {
    return stemSeparator_.start(track, config);
}

StemSeparationSnapshot AppController::stemSeparationSnapshot() const {
    return stemSeparator_.snapshot();
}

bool AppController::normalizeTracks(const std::vector<Track>& tracks,
                                    const NormalizationOptions& options) {
    return audioProcessor_.normalize(tracks, currentPath(), options, [this] {
        scanDirectory(currentPath(), true);
    });
}

bool AppController::convertTracks(const std::vector<Track>& tracks,
                                  const ConvertOptions& options) {
    return audioProcessor_.convert(tracks, currentPath(), options, [this] {
        scanDirectory(currentPath(), true);
    });
}

AudioProcessSnapshot AppController::audioProcessSnapshot() const {
    return audioProcessor_.snapshot();
}

bool AppController::startAutoCueFolder() {
    if (!config_.autoCue.enabled) {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_.status = "Auto Cue disabled";
        autoCueProgress_.done = true;
        return false;
    }

    bool expected = false;
    if (!autoCueBusy_.compare_exchange_strong(expected, true)) {
        return false;
    }

    if (autoCueWorker_.joinable()) {
        autoCueWorker_.request_stop();
        autoCueWorker_.join();
    }

    autoCueCancel_ = false;
    std::string folder = currentPath();
    {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_ = {};
        autoCueProgress_.running = true;
        autoCueProgress_.status = "Starting";
    }

    autoCueWorker_ = std::jthread([this, folder](std::stop_token token) {
        auto progress = [this](const AutoCueProgress& snapshot) {
            std::lock_guard<std::mutex> lock(autoCueMutex_);
            autoCueProgress_ = snapshot;
        };
        auto result = [this](const fs::path& file,
                             const AutoCueResult& cues,
                             const std::vector<SeratoCue>& serato_cues,
                             std::string& error) {
            return saveAutoCueResult(file, cues, serato_cues, error, false);
        };
        autoCueProcessor_.processFolder(
            folder,
            config_.autoCue.writeJson,
            false,
            config_.autoCue.backupBeforeWrite,
            config_.autoCue.overwriteExistingCues,
            config_.autoCue.cleanupAfterWrite,
            config_.autoCue.cues,
            progress,
            autoCueCancel_,
            result);
        if (!token.stop_requested()) {
            std::string export_error;
            exportLibraryCollection(export_error);
        }
        autoCueBusy_ = false;
        if (!token.stop_requested()) {
            scanDirectory(folder, true);
        }
    });
    return true;
}

bool AppController::startAutoCueTrack(const Track& track) {
    if (!config_.autoCue.enabled) {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_.status = "Auto Cue disabled";
        autoCueProgress_.done = true;
        return false;
    }
    if (track.type != EntryType::File || track.id.empty()) {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_.status = "Select a track";
        autoCueProgress_.done = true;
        return false;
    }

    bool expected = false;
    if (!autoCueBusy_.compare_exchange_strong(expected, true)) {
        return false;
    }

    if (autoCueWorker_.joinable()) {
        autoCueWorker_.request_stop();
        autoCueWorker_.join();
    }

    autoCueCancel_ = false;
    std::string folder = currentPath();
    fs::path file = track.id;
    {
        std::lock_guard<std::mutex> lock(autoCueMutex_);
        autoCueProgress_ = {};
        autoCueProgress_.running = true;
        autoCueProgress_.status = "Starting";
    }

    autoCueWorker_ = std::jthread([this, folder, file](std::stop_token token) {
        auto progress = [this](const AutoCueProgress& snapshot) {
            std::lock_guard<std::mutex> lock(autoCueMutex_);
            autoCueProgress_ = snapshot;
        };
        auto result = [this](const fs::path& file,
                             const AutoCueResult& cues,
                             const std::vector<SeratoCue>& serato_cues,
                             std::string& error) {
            return saveAutoCueResult(file, cues, serato_cues, error);
        };
        autoCueProcessor_.processFiles(
            std::vector<fs::path>{file},
            config_.autoCue.writeJson,
            false,
            config_.autoCue.backupBeforeWrite,
            config_.autoCue.overwriteExistingCues,
            config_.autoCue.cleanupAfterWrite,
            config_.autoCue.cues,
            progress,
            autoCueCancel_,
            result);
        autoCueBusy_ = false;
        if (!token.stop_requested()) {
            scanDirectory(folder, true);
        }
    });
    return true;
}

AutoCueProgress AppController::autoCueSnapshot() const {
    std::lock_guard<std::mutex> lock(autoCueMutex_);
    return autoCueProgress_;
}

AutoCueFeatures AppController::waveformForTrack(const Track& track,
                                                std::string& error) const {
    AutoCueFeatures features;
    if (track.type != EntryType::File || track.id.empty()) {
        error = "Select a track";
        return features;
    }
    try {
        features = audioAnalyzer_.extractWaveformFeatures(track.id);
    } catch (const std::exception& e) {
        error = std::string("Waveform failed: ") + e.what();
    } catch (...) {
        error = "Waveform failed";
    }
    return features;
}

bool AppController::trimTrack(const Track& track,
                              double startSeconds,
                              double endSeconds,
                              std::string& error) {
    if (track.type != EntryType::File || track.id.empty()) {
        error = "Select a track";
        return false;
    }
    if (endSeconds <= startSeconds + 0.05) {
        error = "Trim range is too short";
        return false;
    }
    auto ffmpeg = ProcessRunner::findExecutable("ffmpeg");
    if (!ffmpeg) {
        error = "ffmpeg not found";
        return false;
    }

    fs::path input(track.id);
    fs::path output = input.parent_path() /
        (input.stem().string() + "_trim" + input.extension().string());
    int index = 2;
    std::error_code ec;
    while (fs::exists(output, ec)) {
        output = input.parent_path() /
            (input.stem().string() + "_trim_" + std::to_string(index) +
             input.extension().string());
        index++;
    }

    auto seconds = [](double value) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3) << std::max(0.0, value);
        return stream.str();
    };

    std::vector<std::string> args = {
        *ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
        "-ss", seconds(startSeconds),
        "-to", seconds(endSeconds),
        "-i", input.string(),
        "-map", "0", "-map_metadata", "0",
        "-c", "copy",
        output.string(),
    };

    std::string ffmpeg_output;
    int exit_code = ProcessRunner::runWithCombinedOutput(args, &ffmpeg_output);
    if (exit_code != 0 || !fs::exists(output, ec)) {
        fs::remove(output, ec);
        std::vector<std::string> fallback = {
            *ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
            "-ss", seconds(startSeconds),
            "-to", seconds(endSeconds),
            "-i", input.string(),
            "-map", "0:a:0", "-map_metadata", "0",
            output.string(),
        };
        ffmpeg_output.clear();
        exit_code = ProcessRunner::runWithCombinedOutput(fallback, &ffmpeg_output);
    }
    if (exit_code != 0 || !fs::exists(output, ec)) {
        error = ProcessRunner::trim(ffmpeg_output);
        if (error.empty()) {
            error = "ffmpeg trim failed";
        }
        return false;
    }

    scanDirectory(currentPath(), true);
    return true;
}

bool AppController::writeManualCues(const Track& track,
                                    const std::vector<SeratoCue>& cues,
                                    std::string& error) {
    if (track.type != EntryType::File || track.id.empty()) {
        error = "Select a track";
        return false;
    }

    LibraryTrack library_track = libraryTrackForCues(track, cues);
    if (trackRepository_ && cueRepository_) {
        if (!trackRepository_->upsertTrack(library_track, error)) {
            return false;
        }
        if (!cueRepository_->replaceCues(library_track.id, library_track.cues, error)) {
            return false;
        }
    }
    return exportLibraryCues(library_track, error);
}

bool AppController::syncCueMetadata(const Track& track, std::string& result)
{
    if (track.type != EntryType::File || track.id.empty()) {
        result = "Select a track";
        return false;
    }

    std::string serato_error;
    auto serato_cues = seratoCueWriter_.readCues(track.id, serato_error);
    std::string traktor_error;
    auto traktor_cues = traktorMetadataWriter_.readCues(track.id, traktor_error);

    sqlite3* db = libraryDatabase_.isOpen() ? libraryDatabase_.handle() : nullptr;
    std::int64_t now = nowUnixSeconds();

    struct SourceSnapshot {
        std::string name;
        std::vector<SeratoCue> cues;
        std::string hash;
        std::int64_t timestamp = 0;
    };

    std::vector<SourceSnapshot> sources = {
        {"serato", sortedCues(serato_cues), cueHash(serato_cues), 0},
        {"traktor", sortedCues(traktor_cues), cueHash(traktor_cues), 0},
    };

    for (auto& source : sources) {
        std::string old_hash =
            syncStateGet(db, syncStateKey(track.id, source.name, "hash"));
        std::int64_t old_time = syncStateTime(db, track.id, source.name);
        if (old_hash.empty()) {
            source.timestamp = old_time > 0 ? old_time : (source.cues.empty() ? 0 : now);
        } else if (old_hash != source.hash) {
            source.timestamp = now;
        } else {
            source.timestamp = old_time;
        }
    }

    auto rank = [&](const std::string& source) {
        if (source == config_.autoCue.syncPrefer) {
            return 2;
        }
        if (source == "serato") {
            return 1;
        }
        return 0;
    };

    auto chosen = sources.end();
    if (sources.size() == 2 &&
        sources[0].cues.empty() != sources[1].cues.empty()) {
        chosen = sources[0].cues.empty() ? sources.begin() + 1 : sources.begin();
    } else {
        chosen = std::max_element(
            sources.begin(),
            sources.end(),
            [&](const SourceSnapshot& a, const SourceSnapshot& b) {
                if (a.timestamp != b.timestamp) {
                    return a.timestamp < b.timestamp;
                }
                return rank(a.name) < rank(b.name);
            });
    }
    if (chosen == sources.end()) {
        result = "No cue source found";
        return false;
    }

    std::vector<SeratoCue> chosen_cues = sortedCues(chosen->cues);

    std::string write_error;
    seratoCueWriter_.writeCues(track.id, {}, write_error, false, true);
    LibraryTrack clear_track = libraryTrackForCues(track, {});
    traktorMetadataWriter_.writeCues(clear_track, write_error);

    if (!seratoCueWriter_.writeCues(track.id,
                                    chosen_cues,
                                    write_error,
                                    false,
                                    true)) {
        result = "Serato sync error: " + write_error;
        return false;
    }

    LibraryTrack sync_track = libraryTrackForCues(track, chosen_cues);
    if (!traktorMetadataWriter_.writeCues(sync_track, write_error)) {
        result = "Traktor sync error: " + write_error;
        return false;
    }

    if (trackRepository_ && cueRepository_) {
        if (!trackRepository_->upsertTrack(sync_track, write_error) ||
            !cueRepository_->replaceCues(sync_track.id, sync_track.cues, write_error)) {
            result = "Library cue sync error: " + write_error;
            return false;
        }
    }

    std::int64_t timestamp = nowUnixSeconds();
    std::string hash = cueHash(chosen_cues);
    syncStateTouch(db, track.id, "serato", hash, timestamp);
    syncStateTouch(db, track.id, "traktor", hash, timestamp);

    result = "Cue sync: " + chosen->name + " -> Serato + Traktor | cues " +
        std::to_string(chosen_cues.size());
    scanDirectory(currentPath(), true);
    return true;
}

bool AppController::exportLibrary(std::string& error)
{
    if (!trackRepository_ || !cueRepository_) {
        error = "Library database is disabled";
        return false;
    }
    LibraryExporter exporter(*trackRepository_, *cueRepository_, seratoCueWriter_);
    LibraryExportOptions options;
    options.exportSerato = config_.library.exportSerato;
    options.exportRekordbox = config_.library.exportRekordbox;
    options.exportTraktor = config_.library.exportTraktor;
    options.exportJson = config_.library.exportJson;
    options.backupBeforeSeratoWrite = config_.autoCue.backupBeforeWrite;
    options.overwriteExistingSeratoCues = config_.autoCue.overwriteExistingCues;
    options.syncFolder = expandUserPath(config_.library.syncFolder);
    options.outputFolder = options.syncFolder / "exports";
    return exporter.exportAll(options, error);
}

bool AppController::exportLibrary(std::string& result, bool validateAfterExport)
{
    std::string error;
    if (!exportLibrary(error)) {
        result = error;
        return false;
    }
    if (validateAfterExport &&
        (config_.library.exportRekordbox ||
         config_.library.exportTraktor ||
         config_.library.exportJson)) {
        if (!validateLibraryExport(result)) {
            return false;
        }
        return true;
    }
    result = "Library exported: " + enabledLibraryExportsLabel();
    return true;
}

bool AppController::importSeratoCues(std::string& result)
{
    if (!trackRepository_ || !cueRepository_) {
        result = "Library database is disabled";
        return false;
    }

    std::string error;
    auto tracks = trackRepository_->listTracks(error);
    if (!error.empty()) {
        result = error;
        return false;
    }

    int scanned = 0;
    int imported_tracks = 0;
    int imported_cues = 0;
    for (const auto& track : tracks) {
        scanned++;
        auto existing = cueRepository_->cuesForTrack(track.id, error);
        if (!error.empty()) {
            result = error;
            return false;
        }
        if (!existing.empty() || track.path.empty()) {
            continue;
        }

        std::string serato_error;
        auto serato_cues = seratoCueWriter_.readCues(track.path, serato_error);
        if (serato_cues.empty()) {
            continue;
        }

        Track app_track;
        app_track.id = track.path.string();
        app_track.title = track.title.empty()
            ? track.path.stem().string()
            : track.title;
        app_track.duration = track.duration;
        app_track.bpm = track.bpm;
        app_track.key = track.key;
        app_track.genre = track.genre;
        app_track.sizeBytes = track.fileSize;
        app_track.type = EntryType::File;

        LibraryTrack imported = libraryTrackForCues(app_track, serato_cues);
        if (imported.id != track.id) {
            imported.id = track.id;
        }
        if (!cueRepository_->replaceCues(track.id, imported.cues, error)) {
            result = error;
            return false;
        }
        imported_tracks++;
        imported_cues += (int)imported.cues.size();
    }

    result = "Serato cue import: scanned " + std::to_string(scanned) +
        " tracks | imported " + std::to_string(imported_tracks) +
        " tracks | cues " + std::to_string(imported_cues);
    return true;
}

std::string AppController::enabledLibraryExportsLabel() const
{
    std::vector<std::string> exports;
    if (config_.library.exportSerato) {
        exports.push_back("Serato");
    }
    if (config_.library.exportRekordbox) {
        exports.push_back("Rekordbox");
    }
    if (config_.library.exportTraktor) {
        exports.push_back("Traktor");
    }
    if (config_.library.exportJson) {
        exports.push_back("JSON");
    }
    if (exports.empty()) {
        return "none";
    }
    std::ostringstream stream;
    for (std::size_t i = 0; i < exports.size(); ++i) {
        if (i > 0) {
            stream << " | ";
        }
        stream << exports[i];
    }
    return stream.str();
}

bool AppController::importChangedJson(std::string& error)
{
    if (!trackRepository_ || !cueRepository_) {
        error = "Library database is disabled";
        return false;
    }

    fs::path folder = expandUserPath(config_.library.syncFolder);
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        error = "Sync folder not found: " + folder.string();
        return false;
    }

    JsonSync sync;
    ConflictResolver resolver;
    int imported = 0;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) {
            error = ec.message();
            return false;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }

        auto incoming = sync.importTrack(entry.path(), error);
        if (!incoming) {
            return false;
        }

        auto local = trackRepository_->findById(incoming->id, error);
        if (local) {
            local->cues = cueRepository_->cuesForTrack(local->id, error);
            if (!error.empty()) {
                return false;
            }
        }
        LibraryTrack merged = local
            ? resolver.resolve(*local, *incoming)
            : *incoming;
        if (local && merged.updatedAt == local->updatedAt &&
            merged.contentHash == local->contentHash &&
            merged.cues.size() == local->cues.size()) {
            continue;
        }

        if (!trackRepository_->upsertTrack(merged, error)) {
            return false;
        }
        if (!cueRepository_->replaceCues(merged.id, merged.cues, error)) {
            return false;
        }
        if (!exportLibraryCues(merged, error)) {
            return false;
        }
        imported++;
    }

    error = "Imported JSON tracks: " + std::to_string(imported);
    return true;
}

bool AppController::validateLibraryExport(std::string& result)
{
    ExportValidator validator;
    ExportValidationSummary summary;
    fs::path sync_folder = expandUserPath(config_.library.syncFolder);
    fs::path export_folder = sync_folder / "exports";
    ExportValidationOptions options;
    options.validateRekordbox = config_.library.exportRekordbox;
    options.validateTraktor = config_.library.exportTraktor;
    options.validateJson = config_.library.exportJson;
    std::string error;
    if (!validator.validateExportFolder(export_folder,
                                        sync_folder,
                                        options,
                                        summary,
                                        error)) {
        result = error;
        return false;
    }

    result = "Export valid: Rekordbox tracks " +
        std::to_string(summary.rekordboxTracks) + ", cues " +
        std::to_string(summary.rekordboxCues) + " | Traktor tracks " +
        std::to_string(summary.traktorTracks) + ", cues " +
        std::to_string(summary.traktorCues) + " | JSON tracks " +
        std::to_string(summary.jsonTracks) + ", cues " +
        std::to_string(summary.jsonCues);
    return true;
}

bool AppController::validateSeratoCues(std::string& result) const
{
    if (!trackRepository_ || !cueRepository_) {
        result = "Library database is disabled";
        return false;
    }

    std::string error;
    auto tracks = trackRepository_->listTracks(error);
    if (!error.empty()) {
        result = error;
        return false;
    }

    int checked_tracks = 0;
    int matched_tracks = 0;
    int mismatched_tracks = 0;
    int library_cues_total = 0;
    int serato_cues_total = 0;
    std::string mismatch_example;
    for (const auto& track : tracks) {
        auto library_cues = cueRepository_->cuesForTrack(track.id, error);
        if (!error.empty()) {
            result = error;
            return false;
        }
        if (library_cues.empty()) {
            continue;
        }

        checked_tracks++;
        library_cues_total += (int)library_cues.size();

        std::string serato_error;
        auto serato_cues = seratoCueWriter_.readCues(track.path, serato_error);
        serato_cues_total += (int)serato_cues.size();
        bool matches = serato_cues.size() == library_cues.size();
        for (const auto& library_cue : library_cues) {
            auto serato_match = std::find_if(
                serato_cues.begin(),
                serato_cues.end(),
                [&](const SeratoCue& cue) {
                    return cue.index == library_cue.index;
                });
            if (serato_match == serato_cues.end()) {
                matches = false;
                if (mismatch_example.empty()) {
                    mismatch_example = track.title + " missing cue index " +
                        std::to_string(library_cue.index);
                }
                break;
            }
            bool name_matches = serato_match->name == library_cue.name;
            bool position_matches =
                std::abs(serato_match->seconds - library_cue.positionSeconds) <= 0.02;
            if (!name_matches || !position_matches) {
                matches = false;
                if (mismatch_example.empty()) {
                    std::ostringstream stream;
                    stream << track.title << " cue " << library_cue.index
                           << " differs";
                    if (!name_matches) {
                        stream << " name";
                    }
                    if (!position_matches) {
                        stream << " position";
                    }
                    mismatch_example = stream.str();
                }
                break;
            }
        }
        if (matches) {
            matched_tracks++;
        } else {
            mismatched_tracks++;
        }
    }

    result = "Serato validation: checked " + std::to_string(checked_tracks) +
        " tracks | matched " + std::to_string(matched_tracks) +
        " | mismatched " + std::to_string(mismatched_tracks) +
        " | library cues " + std::to_string(library_cues_total) +
        " | serato cues " + std::to_string(serato_cues_total);
    if (!mismatch_example.empty()) {
        result += " | first mismatch: " + mismatch_example;
    }
    return mismatched_tracks == 0;
}

bool AppController::validateTraktorEmbeddedCues(std::string& result) const
{
    if (!trackRepository_ || !cueRepository_) {
        result = "Library database is disabled";
        return false;
    }

    std::string error;
    auto tracks = trackRepository_->listTracks(error);
    if (!error.empty()) {
        result = error;
        return false;
    }

    int checked_tracks = 0;
    int supported_tracks = 0;
    int tagged_tracks = 0;
    int missing_tags = 0;
    int matched_tracks = 0;
    int mismatched_tracks = 0;
    int unsupported_tracks = 0;
    int library_cues_total = 0;
    int embedded_cues_total = 0;
    std::string first_missing;
    for (const auto& track : tracks) {
        auto library_cues = cueRepository_->cuesForTrack(track.id, error);
        if (!error.empty()) {
            result = error;
            return false;
        }
        if (library_cues.empty()) {
            continue;
        }

        checked_tracks++;
        library_cues_total += (int)library_cues.size();
        std::string inspect_error;
        auto status = traktorMetadataWriter_.inspect(track.path, inspect_error);
        if (!inspect_error.empty()) {
            if (first_missing.empty()) {
                first_missing = track.title + ": " + inspect_error;
            }
            missing_tags++;
            continue;
        }
        if (!status.supportedContainer) {
            unsupported_tracks++;
            continue;
        }
        supported_tracks++;
        if (status.hasTraktor4Tag) {
            tagged_tracks++;
            std::string read_error;
            auto traktor_cues = traktorMetadataWriter_.readCues(track.path, read_error);
            embedded_cues_total += (int)traktor_cues.size();
            bool matches = read_error.empty() &&
                traktor_cues.size() == library_cues.size();
            for (const auto& library_cue : library_cues) {
                auto embedded = std::find_if(
                    traktor_cues.begin(),
                    traktor_cues.end(),
                    [&](const SeratoCue& cue) {
                        return cue.index == library_cue.index;
                    });
                if (embedded == traktor_cues.end()) {
                    matches = false;
                    break;
                }
                bool name_matches = embedded->name == library_cue.name ||
                    embedded->name == "n.n.";
                bool position_matches =
                    std::abs(embedded->seconds - library_cue.positionSeconds) <= 0.02;
                if (!name_matches || !position_matches) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                matched_tracks++;
            } else {
                mismatched_tracks++;
                if (first_missing.empty()) {
                    first_missing = track.title + ": TRAKTOR4 cues differ";
                }
            }
        } else {
            missing_tags++;
            if (first_missing.empty()) {
                first_missing = track.title + ": " + status.detail;
            }
        }
    }

    result = "Traktor embedded validation: checked " +
        std::to_string(checked_tracks) +
        " tracks | supported " + std::to_string(supported_tracks) +
        " | tagged " + std::to_string(tagged_tracks) +
        " | matched " + std::to_string(matched_tracks) +
        " | mismatched " + std::to_string(mismatched_tracks) +
        " | missing " + std::to_string(missing_tags) +
        " | unsupported " + std::to_string(unsupported_tracks) +
        " | library cues " + std::to_string(library_cues_total) +
        " | embedded cues " + std::to_string(embedded_cues_total);
    if (!first_missing.empty()) {
        result += " | first missing: " + first_missing;
    }
    return missing_tags == 0 && unsupported_tracks == 0 && mismatched_tracks == 0;
}

bool AppController::libraryStatus(std::string& result) const
{
    if (!trackRepository_) {
        result = "Library database is disabled";
        return false;
    }
    std::string error;
    LibraryStats stats = trackRepository_->stats(error);
    if (!error.empty()) {
        result = error;
        return false;
    }
    std::ostringstream stream;
    stream << "Library: " << libraryDatabase_.path().string()
           << " | tracks " << stats.tracks
           << " | cues " << stats.cues
           << " | loops " << stats.loops
           << " | playlists " << stats.playlists
           << " | exports " << enabledLibraryExportsLabel();
    result = stream.str();
    return true;
}

std::vector<SeratoCue> AppController::readManualCues(const Track& track,
                                                     std::string& error) {
    if (track.type != EntryType::File || track.id.empty()) {
        error = "Select a track";
        return {};
    }

    std::string serato_error;
    auto serato_cues = seratoCueWriter_.readCues(track.id, serato_error);

    std::string traktor_error;
    auto traktor_cues = traktorMetadataWriter_.readCues(track.id, traktor_error);

    bool has_traktor_tag = false;
    if (traktor_error.empty()) {
        std::string inspect_error;
        auto traktor_status = traktorMetadataWriter_.inspect(track.id, inspect_error);
        has_traktor_tag = inspect_error.empty() && traktor_status.hasTraktor4Tag;
    }

    std::vector<SeratoCue> chosen = chooseCueSyncSource(
        serato_cues,
        traktor_cues,
        config_.autoCue.syncPrefer);
    if (libraryDatabase_.isOpen() &&
        !serato_cues.empty() &&
        !traktor_cues.empty() &&
        !cuePositionsMatch(serato_cues, traktor_cues)) {
        struct SourceSnapshot {
            std::string name;
            std::vector<SeratoCue> cues;
            std::string hash;
            std::int64_t timestamp = 0;
        };
        sqlite3* db = libraryDatabase_.handle();
        std::int64_t now = nowUnixSeconds();
        std::vector<SourceSnapshot> sources = {
            {"serato", sortedCues(serato_cues), cueHash(serato_cues), 0},
            {"traktor", sortedCues(traktor_cues), cueHash(traktor_cues), 0},
        };
        for (auto& source : sources) {
            std::string old_hash =
                syncStateGet(db, syncStateKey(track.id, source.name, "hash"));
            std::int64_t old_time = syncStateTime(db, track.id, source.name);
            if (old_hash.empty()) {
                source.timestamp = old_time > 0 ? old_time : now;
            } else if (old_hash != source.hash) {
                source.timestamp = now;
            } else {
                source.timestamp = old_time;
            }
        }
        auto rank = [&](const std::string& source) {
            if (source == config_.autoCue.syncPrefer) {
                return 2;
            }
            if (source == "serato") {
                return 1;
            }
            return 0;
        };
        auto selected = std::max_element(
            sources.begin(),
            sources.end(),
            [&](const SourceSnapshot& a, const SourceSnapshot& b) {
                if (a.timestamp != b.timestamp) {
                    return a.timestamp < b.timestamp;
                }
                return rank(a.name) < rank(b.name);
            });
        if (selected != sources.end()) {
            chosen = selected->cues;
        }
    }

    if (chosen.empty()) {
        if (trackRepository_ && cueRepository_) {
            LibraryTrack sync_track = libraryTrackForCues(track, {});
            std::string write_error;
            if (trackRepository_->upsertTrack(sync_track, write_error)) {
                cueRepository_->replaceCues(sync_track.id, sync_track.cues, write_error);
            }
        }
        if (libraryDatabase_.isOpen()) {
            std::int64_t timestamp = nowUnixSeconds();
            std::string hash = cueHash({});
            sqlite3* db = libraryDatabase_.handle();
            syncStateTouch(db, track.id, "serato", hash, timestamp);
            syncStateTouch(db, track.id, "traktor", hash, timestamp);
        }
        if (has_traktor_tag || serato_error.empty() || traktor_error.empty()) {
            error.clear();
            return {};
        }
        error = serato_error.empty() ? traktor_error : serato_error;
        return {};
    }

    if (config_.autoCue.writeSerato &&
        !cuePositionsMatch(chosen, serato_cues)) {
        std::string write_error;
        seratoCueWriter_.writeCues(track.id,
                                   chosen,
                                   write_error,
                                   config_.autoCue.backupBeforeWrite,
                                   true);
    }

    if (config_.autoCue.writeTraktor &&
        !cuePositionsMatch(chosen, traktor_cues)) {
        LibraryTrack sync_track = libraryTrackForCues(track, chosen);
        std::string write_error;
        traktorMetadataWriter_.writeCues(sync_track, write_error);
    }

    if (trackRepository_ && cueRepository_) {
        LibraryTrack sync_track = libraryTrackForCues(track, chosen);
        std::string write_error;
        if (trackRepository_->upsertTrack(sync_track, write_error)) {
            cueRepository_->replaceCues(sync_track.id, sync_track.cues, write_error);
        }
    }

    if (libraryDatabase_.isOpen()) {
        std::int64_t timestamp = nowUnixSeconds();
        std::string hash = cueHash(chosen);
        sqlite3* db = libraryDatabase_.handle();
        syncStateTouch(db, track.id, "serato", hash, timestamp);
        syncStateTouch(db, track.id, "traktor", hash, timestamp);
    }

    error.clear();
    return sortedCues(chosen);
}

std::string AppController::cuePreviewForTrack(const Track& track, int width) const {
    if (track.type != EntryType::File || width < 12) {
        return {};
    }
    auto renderSeratoCues = [&](const std::vector<SeratoCue>& cues) {
        double duration = track.duration;
        if (duration <= 0.0) {
            AudioMetadata metadata = audioAnalyzer_.readEmbeddedMetadata(track.id);
            duration = metadata.duration;
        }
        std::vector<CuePoint> preview_cues;
        preview_cues.reserve(cues.size());
        for (const auto& cue : cues) {
            preview_cues.push_back({cue.name, cue.seconds});
        }
        return renderCuePreview(preview_cues, duration, width);
    };

    std::string serato_error;
    auto serato_cues = seratoCueWriter_.readCues(track.id, serato_error);

    std::string traktor_error;
    auto traktor_cues = traktorMetadataWriter_.readCues(track.id, traktor_error);

    auto chosen_cues = chooseCueSyncSource(
        serato_cues,
        traktor_cues,
        config_.autoCue.syncPrefer);
    if (!chosen_cues.empty()) {
        return renderSeratoCues(chosen_cues);
    }
    if (traktor_error.empty()) {
        std::string inspect_error;
        auto traktor_status = traktorMetadataWriter_.inspect(track.id, inspect_error);
        if (inspect_error.empty() && traktor_status.hasTraktor4Tag) {
            return {};
        }
    }

    std::filesystem::path json = track.id;
    json += ".cues.json";
    std::ifstream stream(json);
    AutoCueResult cues;
    bool has_cues = false;
    if (stream) {
        std::string content((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());
        auto numberAfter = [&](const std::string& key) {
            size_t pos = content.find("\"" + key + "\"");
            if (pos == std::string::npos) {
                return 0.0;
            }
            pos = content.find(':', pos);
            if (pos == std::string::npos) {
                return 0.0;
            }
            try {
                return std::stod(content.substr(pos + 1));
            } catch (...) {
                return 0.0;
            }
        };
        cues.start.positionSeconds = numberAfter("start");
        cues.drop1.positionSeconds = numberAfter("drop1");
        cues.breakdown.positionSeconds = numberAfter("break");
        cues.drop2.positionSeconds = numberAfter("drop2");
        has_cues = cues.start.positionSeconds > 0.0 ||
            cues.drop1.positionSeconds > 0.0 ||
            cues.breakdown.positionSeconds > 0.0 ||
            cues.drop2.positionSeconds > 0.0;
    }

    if (!has_cues) {
        return {};
    }

    double duration = track.duration;
    if (duration <= 0.0) {
        AudioMetadata metadata = audioAnalyzer_.readEmbeddedMetadata(track.id);
        duration = metadata.duration;
    }
    return renderCuePreview(cues, duration, width);
}

bool AppController::metadataBusy() const {
    return metadataBusy_.load();
}

bool AppController::directoryScanBusy() const {
    return directoryScansInFlight_.load() > 0;
}

namespace {

bool validFolderName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos;
}

std::string decodeFlatValue(std::string value) {
    value = ProcessRunner::trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }

    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            decoded += value[i];
            continue;
        }

        char next = value[++i];
        if (next == 'n') {
            decoded += '\n';
        } else if (next == 'r') {
            decoded += '\r';
        } else if (next == 't') {
            decoded += '\t';
        } else {
            decoded += next;
        }
    }
    return decoded;
}

std::string cleanMetadataName(std::string name) {
    constexpr std::string_view format_tags = "format.tags.";
    constexpr std::string_view format = "format.";
    constexpr std::string_view streams = "streams.stream.";

    if (name.starts_with(format_tags)) {
        return "TAG:" + name.substr(format_tags.size());
    }
    if (name.starts_with(format)) {
        return name.substr(format.size());
    }
    if (name.starts_with(streams)) {
        std::string rest = name.substr(streams.size());
        size_t dot = rest.find('.');
        if (dot != std::string::npos) {
            std::string stream = rest.substr(0, dot);
            std::string field = rest.substr(dot + 1);
            constexpr std::string_view tags = "tags.";
            if (field.starts_with(tags)) {
                return "STREAM " + stream + " TAG:" + field.substr(tags.size());
            }
            return "stream " + stream + " " + field;
        }
    }
    return name;
}

}  // namespace

bool AppController::createFolder(const std::string& name, std::string& error) {
    if (!validFolderName(name)) {
        error = "Invalid folder name";
        return false;
    }

    fs::path path = fs::path(currentPath()) / name;
    std::error_code ec;
    if (!fs::create_directory(path, ec)) {
        error = ec ? ec.message() : "Folder already exists";
        return false;
    }
    scanDirectory(currentPath(), true);
    return true;
}

bool AppController::renameFolder(const std::string& path,
                                 const std::string& new_name,
                                 std::string& error) {
    if (!validFolderName(new_name) || !fs::is_directory(path)) {
        error = "Invalid folder name or selection";
        return false;
    }
    for (const auto& root : config_.musicDirectories) {
        if (fs::path(path) == fs::path(root)) {
            error = "A library root cannot be renamed here";
            return false;
        }
    }

    fs::path source(path);
    fs::path target = source.parent_path() / new_name;
    std::error_code ec;
    fs::rename(source, target, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    if (fs::path(currentPath()) == source) {
        scanDirectory(target.string(), true);
    } else {
        scanDirectory(currentPath(), true);
    }
    return true;
}

bool AppController::moveTrack(const Track& track,
                              const std::string& destination,
                              std::string& error) {
    if (track.type != EntryType::File || destination.empty()) {
        error = "Select a track and destination folder";
        return false;
    }

    fs::path target_dir(destination);
    if (!target_dir.is_absolute()) {
        target_dir = fs::path(currentPath()) / target_dir;
    }
    if (!fs::is_directory(target_dir)) {
        error = "Destination folder does not exist";
        return false;
    }

    fs::path target = target_dir / fs::path(track.id).filename();
    if (target == fs::path(track.id)) {
        error = "Track is already in this folder";
        return false;
    }
    if (fs::exists(target)) {
        error = "A track with this name already exists in destination";
        return false;
    }

    std::error_code ec;
    fs::rename(track.id, target, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        for (auto& queued : playbackQueue_) {
            if (queued.id == track.id) {
                queued.id = target.string();
            }
        }
        if (playingTrackId_ == track.id) {
            playingTrackId_ = target.string();
            playingTrack_.id = target.string();
        }
    }

    invalidateDirectoryCache(target_dir.string());
    removeCachedEntry(currentPath(), track.id);
    trackStore_.removeTrack(track.id);
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        displayedTracks_.erase(
            std::remove_if(displayedTracks_.begin(), displayedTracks_.end(),
                           [&](const Track& displayed) {
                               return displayed.id == track.id;
                           }),
            displayedTracks_.end());
    }
    return true;
}

bool AppController::deleteEntry(const Track& entry, std::string& error) {
    if (entry.id.empty()) {
        error = "Select a track or folder";
        return false;
    }

    fs::path path(entry.id);
    if (!fs::exists(path)) {
        error = "Selected item does not exist";
        return false;
    }

    for (const auto& root : config_.musicDirectories) {
        if (fs::equivalent(path, fs::path(root))) {
            error = "A library root cannot be deleted here";
            return false;
        }
    }

    std::error_code ec;
    bool is_directory = fs::is_directory(path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        auto is_inside_deleted_directory = [&](const std::string& queued_path) {
            std::error_code relative_error;
            fs::path relative = fs::relative(queued_path, path, relative_error);
            return !relative_error &&
                   !relative.empty() &&
                   !relative.string().starts_with("..");
        };

        if ((!is_directory && playingTrackId_ == entry.id) ||
            (is_directory && is_inside_deleted_directory(playingTrackId_))) {
            audioEngine_.stop();
            playingTrackId_.clear();
        }
        playbackQueue_.erase(
            std::remove_if(playbackQueue_.begin(), playbackQueue_.end(),
                           [&](const Track& queued) {
                               return queued.id == entry.id ||
                                      (is_directory && is_inside_deleted_directory(queued.id));
                           }),
            playbackQueue_.end());
    }

    if (is_directory) {
        fs::remove_all(path, ec);
    } else {
        fs::remove(path, ec);
    }
    if (ec) {
        error = ec.message();
        return false;
    }

    if (is_directory) {
        scanDirectory(currentPath(), true);
    } else {
        removeCachedEntry(currentPath(), entry.id);
        trackStore_.removeTrack(entry.id);
        {
            std::lock_guard<std::mutex> lock(playbackMutex_);
            displayedTracks_.erase(
                std::remove_if(displayedTracks_.begin(), displayedTracks_.end(),
                               [&](const Track& track) {
                                   return track.id == entry.id;
                               }),
                displayedTracks_.end());
        }
    }
    return true;
}

bool AppController::startExternalDrag(const Track& track, std::string& error) const {
    if (track.type != EntryType::File || !fs::is_regular_file(track.id)) {
        error = "Select an audio track";
        return false;
    }

#if defined(__APPLE__)
    auto drag = ProcessRunner::findExecutable("drag");
    if (!drag) {
        error = "Install dragterm: github.com/Wevah/dragterm";
        return false;
    }
    if (!ProcessRunner::launchDetached({*drag, track.id})) {
        error = "Unable to start external drag";
        return false;
    }
    return true;
#else
    error = "External drag is available on macOS";
    return false;
#endif
}

bool AppController::openFolderExternally(const std::string& path, std::string& error) const {
    if (path.empty() || !fs::is_directory(path)) {
        error = "Select a folder";
        return false;
    }

#if defined(__APPLE__)
    if (!ProcessRunner::launchDetached({"open", path})) {
        error = "Unable to open Finder";
        return false;
    }
    return true;
#else
    if (!ProcessRunner::launchDetached({"xdg-open", path})) {
        error = "Unable to open folder";
        return false;
    }
    return true;
#endif
}

bool AppController::openExternalUrl(const std::string& url, std::string& error) const {
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        error = "Unsupported URL";
        return false;
    }

#if defined(__APPLE__)
    if (!ProcessRunner::launchDetached({"open", url})) {
        error = "Unable to open URL";
        return false;
    }
    return true;
#else
    if (!ProcessRunner::launchDetached({"xdg-open", url})) {
        error = "Unable to open URL";
        return false;
    }
    return true;
#endif
}

std::vector<std::pair<std::string, std::string>>
AppController::metadataDetails(const Track& track) const {
    std::vector<std::pair<std::string, std::string>> details;
    std::set<std::string> seen;

    auto add = [&](std::string name, std::string value) {
        if (name.empty() || value == "N/A") {
            return;
        }
        std::string key = name + "\n" + value;
        if (seen.insert(key).second) {
            details.emplace_back(std::move(name), std::move(value));
        }
    };

    add("title", track.title);
    add("path", track.id);
    if (track.duration > 0.0) {
        add("duration", std::to_string((int)track.duration) + " sec");
    }
    if (track.bpm > 0.0) {
        add("bpm", std::to_string((int)track.bpm));
    }
    add("key", track.key);
    add("genre", track.genre);
    if (track.bitrateKbps > 0.0) {
        add("bitrate", std::to_string((int)(track.bitrateKbps + 0.5)) + " kbps");
    }
    if (track.sampleRateHz > 0.0) {
        add("sample rate", std::to_string((int)(track.sampleRateHz + 0.5)) + " Hz");
    }
    if (track.sizeBytes > 0) {
        add("size", std::to_string(track.sizeBytes) + " bytes");
    }

    if (track.id.empty() || track.type != EntryType::File) {
        return details;
    }

    auto ffprobe = ProcessRunner::findExecutable("ffprobe");
    if (!ffprobe) {
        add("error", "ffprobe not found");
        return details;
    }

    std::string output;
    int exit_code = ProcessRunner::run({
        *ffprobe,
        "-v", "error",
        "-show_entries",
        "format=format_name,format_long_name,duration,size,bit_rate:format_tags:"
        "stream=index,codec_type,codec_name,codec_long_name,sample_rate,channels,"
        "channel_layout,bits_per_sample,duration,bit_rate:stream_tags",
        "-of", "flat",
        track.id,
    }, &output);
    if (exit_code != 0) {
        add("error", "Unable to read metadata");
        return details;
    }

    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        add(cleanMetadataName(ProcessRunner::trim(line.substr(0, separator))),
            decodeFlatValue(line.substr(separator + 1)));
    }

    return details;
}
