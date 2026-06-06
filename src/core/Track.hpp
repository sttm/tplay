#pragma once

#include <cstdint>
#include <string>

enum class TrackStatus {
    Ready,
    Downloading,
    Analyzing,
    Error
};
enum class EntryType {
    File,
    Directory
};

struct Track {
    std::string id;
    std::string title;

    double duration = 0.0;
    double bpm = 0.0;
    double bitrateKbps = 0.0;
    double sampleRateHz = 0.0;
    std::uintmax_t sizeBytes = 0;
    EntryType type = EntryType::File;
    std::string key;
    std::string genre;
    TrackStatus status = TrackStatus::Ready;
};
