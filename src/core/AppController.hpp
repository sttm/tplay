#pragma once

#include "TrackStore.hpp"
#include "Config.hpp"
#include "AudioEngine.hpp"
#include "AudioAnalyzer.hpp"
#include "DownloadManager.hpp"
#include "MetadataWriter.hpp"
#include "StemSeparator.hpp"
#include "AudioProcessor.hpp"
#include "../Library/CueRepository.h"
#include "../Library/LibraryDatabase.h"
#include "../Library/TrackRepository.h"
#include "../Export/RekordboxExportProvider.h"
#include "../Export/SeratoExportProvider.h"
#include "../Export/TraktorExportProvider.h"
#include "../Export/LibraryExporter.h"
#include "../AutoCue/FolderProcessor.h"
#include "../Serato/SeratoCueWriter.h"
#include "../Telegram/TelegramBotClient.h"
#include "../Telegram/TelegramInboxService.h"
#include "../Telegram/TelegramRepository.h"
#include "../Traktor/TraktorMetadataWriter.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

enum class PlaybackMode {
    Shuffle,
    RepeatOne,
    RepeatAll
};

class AppController {
public:
    AppController();
    ~AppController();

    TrackStore& trackStore();
    const Config& config() const;

    void scanDirectory(const std::string& path, bool forceRefresh = false);
    std::string currentPath() const;
    void setCurrentPath(const std::string& path);
    int volume() const;
    void volumeUp();
    void volumeDown();
    double playbackRate() const;
    void setPlaybackRate(double rate);
    bool preservePitch() const;
    void setPreservePitch(bool preserve);
    void setEqualizerGains(double lowDb, double midDb, double highDb);
    bool playTrack(const Track& track, const std::vector<Track>& orderedTracks);
    bool playPreviewTrack(const Track& track);
    void togglePreviewPause();
    void stopPreviewPlayback();
    void seekPreviewPlayback(double ratio);
    void setPreviewLoopRange(double startSeconds, double endSeconds);
    void clearPreviewLoopRange();
    PlaybackSnapshot previewPlaybackSnapshot() const;
    std::string previewPlayingTrackId() const;
    double previewPlaybackRate() const;
    void setPreviewPlaybackRate(double rate);
    bool previewPreservePitch() const;
    void setPreviewPreservePitch(bool preserve);
    void togglePause();
    void stopPlayback();
    bool playPreviousTrack();
    bool playNextTrack();
    void cyclePlaybackMode();
    PlaybackMode playbackMode() const;
    void seekPlayback(double ratio);
    PlaybackSnapshot playbackSnapshot() const;
    std::string playingTrackId() const;
    Track playingTrack() const;
    bool downloadToCurrentDirectory(const std::string& source,
                                    std::function<void()> on_finished = {});
    bool downloadToDirectory(const std::string& source,
                             const std::string& directory,
                             std::function<void()> on_finished = {});
    DownloadSnapshot downloadSnapshot() const;
    bool separateTrack(const Track& track);
    bool separateTrack(const Track& track, const DemucsConfig& config);
    StemSeparationSnapshot stemSeparationSnapshot() const;
    bool normalizeTracks(const std::vector<Track>& tracks,
                         const NormalizationOptions& options);
    bool convertTracks(const std::vector<Track>& tracks,
                       const ConvertOptions& options);
    AudioProcessSnapshot audioProcessSnapshot() const;
    bool startAutoCueFolder();
    bool startAutoCueTrack(const Track& track);
    AutoCueProgress autoCueSnapshot() const;
    std::string cuePreviewForTrack(const Track& track, int width) const;
    AutoCueFeatures waveformForTrack(const Track& track, std::string& error) const;
    bool trimTrack(const Track& track,
                   double startSeconds,
                   double endSeconds,
                   std::string& error);
    bool writeManualCues(const Track& track,
                         const std::vector<SeratoCue>& cues,
                         std::string& error);
    std::vector<SeratoCue> readManualCues(const Track& track,
                                          std::string& error);
    bool syncCueMetadata(const Track& track, std::string& result);
    bool metadataBusy() const;
    bool directoryScanBusy() const;
    bool createFolder(const std::string& name, std::string& error);
    bool renameFolder(const std::string& path,
                      const std::string& new_name,
                      std::string& error);
    bool moveTrack(const Track& track,
                   const std::string& destination,
                   std::string& error);
    bool deleteEntry(const Track& entry, std::string& error);
    bool startExternalDrag(const Track& track, std::string& error) const;
    bool openFolderExternally(const std::string& path, std::string& error) const;
    bool openExternalUrl(const std::string& url, std::string& error) const;
    std::vector<std::pair<std::string, std::string>>
    metadataDetails(const Track& track) const;
    bool exportLibrary(std::string& error);
    bool exportLibrary(std::string& result, bool validateAfterExport);
    bool importSeratoCues(std::string& result);
    bool importChangedJson(std::string& error);
    bool validateLibraryExport(std::string& result);
    bool validateSeratoCues(std::string& result) const;
    bool validateTraktorEmbeddedCues(std::string& result) const;
    bool libraryStatus(std::string& result) const;
    std::string enabledLibraryExportsLabel() const;
    bool isTelegramPath(const std::string& path) const;
    std::string telegramRootPath() const;

private:
    TrackStore trackStore_;
    Config config_;
    LibraryDatabase libraryDatabase_;
    std::unique_ptr<TrackRepository> trackRepository_;
    std::unique_ptr<CueRepository> cueRepository_;
    std::unique_ptr<TelegramRepository> telegramRepository_;
    std::unique_ptr<TelegramBotClient> telegramClient_;
    std::unique_ptr<TelegramInboxService> telegramInbox_;
    AudioEngine audioEngine_;
    AudioEngine previewAudioEngine_;
    DownloadManager downloadManager_;
    StemSeparator stemSeparator_;
    AudioProcessor audioProcessor_;
    AudioAnalyzer audioAnalyzer_;
    MetadataWriter metadataWriter_;
    AutoCueMarker autoCueMarker_{audioAnalyzer_};
    SeratoCueWriter seratoCueWriter_;
    TraktorMetadataWriter traktorMetadataWriter_;
    FolderProcessor autoCueProcessor_{autoCueMarker_, seratoCueWriter_};

    int volume_ = 80;
    std::string currentPath_;
    mutable std::mutex currentPathMutex_;
    mutable std::mutex metadataMutex_;
    std::condition_variable_any metadataCv_;
    std::vector<Track> pendingMetadata_;
    std::string pendingMetadataPath_;
    unsigned long metadataGeneration_ = 0;
    std::atomic_bool metadataBusy_{false};
    std::jthread metadataWorker_;
    mutable std::mutex playbackMutex_;
    std::vector<Track> displayedTracks_;
    std::vector<Track> playbackQueue_;
    std::string playingTrackId_;
    Track playingTrack_;
    std::string previewPlayingTrackId_;
    PlaybackMode playbackMode_ = PlaybackMode::RepeatAll;
    std::mt19937 randomGenerator_{std::random_device{}()};
    std::jthread playbackWorker_;
    mutable std::mutex autoCueMutex_;
    AutoCueProgress autoCueProgress_;
    std::atomic_bool autoCueBusy_{false};
    std::atomic_bool autoCueCancel_{false};
    std::jthread autoCueWorker_;
    std::unordered_map<std::string, std::vector<Track>> directoryCache_;
    mutable std::mutex directoryCacheMutex_;
    std::jthread initialScanWorker_;
    std::atomic_uint64_t directoryScanGeneration_{0};
    std::atomic_int directoryScansInFlight_{0};

    bool isAllowedFormat(const std::string& ext) const;
    bool scanTelegramDirectory(const std::string& path,
                               std::vector<Track>& tracks,
                               std::string& error);
    bool resolveTelegramTrackForPlayback(const Track& track,
                                         Track& localTrack,
                                         std::string& error);
    static bool needsMetadataScan(const Track& track);
    static void mergeMetadataIntoTrack(Track& track,
                                       const AudioMetadata& metadata);
    void initializeLibrary();
    void upsertLibraryTrack(const Track& track);
    void importSeratoCuesIfLibraryEmpty(const Track& track,
                                        const LibraryTrack& libraryTrack);
    void replaceLibraryCues(const Track& track,
                            const std::vector<SeratoCue>& cues);
    LibraryTrack libraryTrackForCues(const Track& track,
                                     const std::vector<SeratoCue>& cues) const;
    bool exportLibraryCues(const LibraryTrack& track,
                           std::string& error,
                           bool updateCollectionExport = true);
    bool exportLibraryCollection(std::string& error);
    bool saveAutoCueResult(const std::filesystem::path& file,
                           const AutoCueResult& result,
                           const std::vector<SeratoCue>& cues,
                           std::string& error,
                           bool updateCollectionExport = true);
    void updateCachedTrack(const std::string& directory, const Track& track);
    void removeCachedEntry(const std::string& directory, const std::string& id);
    void invalidateDirectoryCache(const std::string& path);
    void queueMetadataScan(const std::string& path,
                           const std::vector<Track>& tracks);
    void metadataLoop(std::stop_token stop_token);
    void playbackLoop(std::stop_token stop_token);
    bool playRelativeTrackLocked(int direction, bool natural_end);
};
