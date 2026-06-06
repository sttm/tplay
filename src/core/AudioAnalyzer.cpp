#include "AudioAnalyzer.hpp"

#include "ProcessRunner.hpp"

#include <algorithmfactory.h>
#include <essentia.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/mp4tag.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

double parseNumber(const std::string& value)
{
    try {
        return std::stod(value);
    } catch (...) {
        return 0.0;
    }
}

std::string compactTagName(std::string value)
{
    value = lowercase(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](char c) {
                    return c == ' ' || c == '_' || c == '-';
                }),
                value.end());
    return value;
}

bool containsText(const std::string& value, const std::string& needle)
{
    return value.find(needle) != std::string::npos;
}

std::string lowercaseExtension(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return extension;
}

void readMp4TagMetadata(const std::string& path, AudioMetadata& metadata)
{
    TagLib::MP4::File file(path.c_str(), true);
    if (!file.isValid() || file.tag() == nullptr) {
        return;
    }

    auto* tag = file.tag();
    auto readString = [&](const char* name) {
        if (!tag->contains(name)) {
            return std::string{};
        }
        auto values = tag->item(name).toStringList();
        if (values.isEmpty()) {
            return std::string{};
        }
        return values.front().to8Bit(true);
    };

    if (metadata.bpm <= 0.0 && tag->contains("tmpo")) {
        auto values = tag->item("tmpo").toIntPair();
        if (values.first > 0) {
            metadata.bpm = values.first;
        }
    }
    if (metadata.key.empty()) {
        metadata.key = readString("----:com.apple.iTunes:initialkey");
    }
    if (metadata.key.empty()) {
        metadata.key = readString("----:com.apple.iTunes:KEY");
    }
    if (metadata.key.empty()) {
        metadata.key = readString("©key");
    }
}

void ensureEssentiaInitialized()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        essentia::init();
    });
}

void parseKeyValueOutput(const std::string& output, AudioMetadata& metadata)
{
    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        std::string name = lowercase(ProcessRunner::trim(line.substr(0, separator)));
        std::string value = ProcessRunner::trim(line.substr(separator + 1));
        std::string compact_name = compactTagName(name);
        bool is_tag = compact_name.starts_with("tag:");
        if (name == "duration" || name == "format.duration") {
            metadata.duration = parseNumber(value);
        } else if (name == "bit_rate" || name == "format.bit_rate") {
            metadata.bitrateKbps = parseNumber(value) / 1000.0;
        } else if (name == "sample_rate" ||
                   name.ends_with(".sample_rate")) {
            metadata.sampleRateHz = parseNumber(value);
        } else if (name == "size" || name == "format.size") {
            metadata.sizeBytes = (std::uintmax_t)parseNumber(value);
        } else if (name == "bpm" || name == "tag:bpm" ||
                   name == "tag:tbpm" || name == "tag:tmpo" ||
                   name == "tag:tempo" ||
                   (is_tag && (containsText(compact_name, "bpm") ||
                               containsText(compact_name, "tbpm") ||
                               containsText(compact_name, "tmpo") ||
                               compact_name.ends_with(":tempo")))) {
            metadata.bpm = parseNumber(value);
        } else if (name == "key" || name == "tag:initialkey" ||
                   name == "tag:initial key" || name == "tag:initial_key" ||
                   name == "tag:key" ||
                   (is_tag && (containsText(compact_name, "initialkey") ||
                               compact_name.ends_with(":key") ||
                               compact_name.ends_with(":tkey")))) {
            metadata.key = value;
        } else if (name == "genre" || name == "tag:genre") {
            metadata.genre = value;
        }
    }
}

std::array<float, 12> chromaFromSpectrum(const std::vector<essentia::Real>& spectrum,
                                         double sampleRate,
                                         int frameSize)
{
    std::array<float, 12> chroma{};
    if (spectrum.empty() || frameSize <= 0) {
        return chroma;
    }

    for (size_t i = 1; i < spectrum.size(); ++i) {
        double frequency = (double)i * sampleRate / (double)frameSize;
        if (frequency < 40.0 || frequency > 5000.0) {
            continue;
        }
        double midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
        int pitchClass = ((int)std::llround(midi) % 12 + 12) % 12;
        chroma[(size_t)pitchClass] += (float)spectrum[i];
    }

    float sum = std::accumulate(chroma.begin(), chroma.end(), 0.0f);
    if (sum > 0.0f) {
        for (float& value : chroma) {
            value /= sum;
        }
    }
    return chroma;
}

float spectralCentroidFromSpectrum(const std::vector<essentia::Real>& spectrum,
                                   double sampleRate,
                                   int frameSize)
{
    double weighted = 0.0;
    double total = 0.0;
    for (size_t i = 1; i < spectrum.size(); ++i) {
        double frequency = (double)i * sampleRate / (double)frameSize;
        double magnitude = std::max(0.0f, spectrum[i]);
        weighted += frequency * magnitude;
        total += magnitude;
    }
    if (total <= 0.0) {
        return 0.0f;
    }
    return (float)(weighted / total / (sampleRate * 0.5));
}

}  // namespace

AudioMetadata AudioAnalyzer::readEmbeddedMetadata(const std::string& path) const
{
    AudioMetadata result;
    auto ffprobe = ProcessRunner::findExecutable("ffprobe");
    if (!ffprobe) {
        result.error = "ffprobe not found";
        return result;
    }

    std::string output;
    int exit_code = ProcessRunner::run({
        *ffprobe,
        "-v", "error",
        "-show_entries", "format=duration,bit_rate,size:stream=sample_rate:format_tags:stream_tags",
        "-of", "default=noprint_wrappers=1",
        path,
    }, &output);
    if (exit_code != 0) {
        result.error = "Unable to read audio metadata";
        return result;
    }

    parseKeyValueOutput(output, result);
    std::string extension = lowercaseExtension(path);
    if (extension == ".m4a" || extension == ".mp4" || extension == ".mov" ||
        extension == ".aac" || extension == ".alac") {
        readMp4TagMetadata(path, result);
    }
    if (result.sizeBytes == 0) {
        std::error_code ec;
        result.sizeBytes = fs::file_size(path, ec);
        if (ec) {
            result.sizeBytes = 0;
        }
    }
    result.succeeded = true;
    return result;
}

AudioMetadata AudioAnalyzer::analyzeWithEssentia(const std::string& path) const
{
    AudioMetadata result;

    try {
        ensureEssentiaInitialized();

        constexpr double sample_rate = 44100.0;
        auto& factory = essentia::standard::AlgorithmFactory::instance();

        std::vector<essentia::Real> audio;
        std::unique_ptr<essentia::standard::Algorithm> loader(
            factory.create("MonoLoader",
                           "filename", path,
                           "sampleRate", sample_rate,
                           "resampleQuality", 1));
        loader->output("audio").set(audio);
        loader->compute();

        if (audio.empty()) {
            result.error = "Essentia loaded no audio";
            return result;
        }

        result.duration = (double)audio.size() / sample_rate;

        essentia::Real bpm = 0.0;
        essentia::Real confidence = 0.0;
        std::vector<essentia::Real> ticks;
        std::vector<essentia::Real> estimates;
        std::vector<essentia::Real> bpm_intervals;
        std::unique_ptr<essentia::standard::Algorithm> rhythm(
            factory.create("RhythmExtractor2013", "method", "multifeature"));
        rhythm->input("signal").set(audio);
        rhythm->output("bpm").set(bpm);
        rhythm->output("ticks").set(ticks);
        rhythm->output("confidence").set(confidence);
        rhythm->output("estimates").set(estimates);
        rhythm->output("bpmIntervals").set(bpm_intervals);
        rhythm->compute();
        result.bpm = bpm;

        std::string key;
        std::string scale;
        essentia::Real strength = 0.0;
        std::unique_ptr<essentia::standard::Algorithm> key_extractor(
            factory.create("KeyExtractor",
                           "profileType", "edma",
                           "sampleRate", sample_rate));
        key_extractor->input("audio").set(audio);
        key_extractor->output("key").set(key);
        key_extractor->output("scale").set(scale);
        key_extractor->output("strength").set(strength);
        key_extractor->compute();

        if (!key.empty()) {
            result.key = key + (scale == "minor" ? "m" : "");
        }

        result.succeeded = result.duration > 0.0 ||
                           result.bpm > 0.0 ||
                           !result.key.empty();
        if (!result.succeeded) {
            result.error = "Essentia returned no metadata";
        }
        return result;
    } catch (const std::exception& e) {
        result.error = std::string("Essentia analysis failed: ") + e.what();
    } catch (...) {
        result.error = "Essentia analysis failed";
    }

    return result;
}

AutoCueFeatures AudioAnalyzer::extractAutoCueFeatures(const fs::path& file) const
{
    ensureEssentiaInitialized();

    constexpr double sample_rate = 44100.0;
    constexpr int frame_size = 2048;
    constexpr int hop_size = 512;

    auto& factory = essentia::standard::AlgorithmFactory::instance();
    std::vector<essentia::Real> audio;
    std::unique_ptr<essentia::standard::Algorithm> loader(
        factory.create("MonoLoader",
                       "filename", file.string(),
                       "sampleRate", sample_rate,
                       "resampleQuality", 1));
    loader->output("audio").set(audio);
    loader->compute();
    if (audio.empty()) {
        throw std::runtime_error("Essentia loaded no audio");
    }

    AutoCueFeatures features;
    features.duration = (double)audio.size() / sample_rate;
    features.hopSeconds = (double)hop_size / sample_rate;

    essentia::Real bpm = 0.0;
    essentia::Real confidence = 0.0;
    std::vector<essentia::Real> ticks;
    std::vector<essentia::Real> estimates;
    std::vector<essentia::Real> bpm_intervals;
    std::unique_ptr<essentia::standard::Algorithm> rhythm(
        factory.create("RhythmExtractor2013", "method", "multifeature"));
    rhythm->input("signal").set(audio);
    rhythm->output("bpm").set(bpm);
    rhythm->output("ticks").set(ticks);
    rhythm->output("confidence").set(confidence);
    rhythm->output("estimates").set(estimates);
    rhythm->output("bpmIntervals").set(bpm_intervals);
    rhythm->compute();
    features.beats.reserve(ticks.size());
    for (essentia::Real tick : ticks) {
        features.beats.push_back((float)tick);
    }

    std::vector<essentia::Real> frame;
    std::vector<essentia::Real> windowed;
    std::vector<essentia::Real> spectrum;
    std::unique_ptr<essentia::standard::Algorithm> windowing(
        factory.create("Windowing", "type", "hann"));
    std::unique_ptr<essentia::standard::Algorithm> spectrum_alg(
        factory.create("Spectrum"));
    windowing->input("frame").set(frame);
    windowing->output("frame").set(windowed);
    spectrum_alg->input("frame").set(windowed);
    spectrum_alg->output("spectrum").set(spectrum);

    std::vector<essentia::Real> previous_spectrum;
    float previous_energy = 0.0f;
    for (size_t start = 0; start < audio.size(); start += hop_size) {
        frame.assign((size_t)frame_size, 0.0f);
        size_t available = std::min((size_t)frame_size, audio.size() - start);
        std::copy(audio.begin() + (long)start,
                  audio.begin() + (long)(start + available),
                  frame.begin());

        double sum_squares = 0.0;
        for (essentia::Real sample : frame) {
            sum_squares += (double)sample * (double)sample;
        }
        float energy = (float)std::sqrt(sum_squares / (double)frame.size());
        features.energyCurve.push_back(energy);
        features.onsetCurve.push_back(std::max(0.0f, energy - previous_energy));
        previous_energy = energy;

        windowing->compute();
        spectrum_alg->compute();
        float flux = 0.0f;
        if (!previous_spectrum.empty() && previous_spectrum.size() == spectrum.size()) {
            for (size_t i = 0; i < spectrum.size(); ++i) {
                flux += std::max<essentia::Real>(0.0f, spectrum[i] - previous_spectrum[i]);
            }
        }
        features.spectralFluxCurve.push_back(flux);
        features.spectralCentroidCurve.push_back(
            spectralCentroidFromSpectrum(spectrum, sample_rate, frame_size));
        features.chromaCurve.push_back(
            chromaFromSpectrum(spectrum, sample_rate, frame_size));
        previous_spectrum = spectrum;

        if (start + available >= audio.size()) {
            break;
        }
    }

    return features;
}

AutoCueFeatures AudioAnalyzer::extractWaveformFeatures(const fs::path& file) const
{
    ensureEssentiaInitialized();

    constexpr double sample_rate = 44100.0;
    constexpr int frame_size = 2048;
    constexpr int hop_size = 512;

    auto& factory = essentia::standard::AlgorithmFactory::instance();
    std::vector<essentia::Real> audio;
    std::unique_ptr<essentia::standard::Algorithm> loader(
        factory.create("MonoLoader",
                       "filename", file.string(),
                       "sampleRate", sample_rate,
                       "resampleQuality", 1));
    loader->output("audio").set(audio);
    loader->compute();
    if (audio.empty()) {
        throw std::runtime_error("Essentia loaded no audio");
    }

    AutoCueFeatures features;
    features.duration = (double)audio.size() / sample_rate;
    features.hopSeconds = (double)hop_size / sample_rate;

    float previous_energy = 0.0f;
    for (size_t start = 0; start < audio.size(); start += hop_size) {
        size_t available = std::min((size_t)frame_size, audio.size() - start);
        double sum_squares = 0.0;
        for (size_t i = 0; i < available; ++i) {
            double sample = audio[start + i];
            sum_squares += sample * sample;
        }
        float energy = available > 0
            ? (float)std::sqrt(sum_squares / (double)available)
            : 0.0f;
        features.energyCurve.push_back(energy);
        features.onsetCurve.push_back(std::max(0.0f, energy - previous_energy));
        previous_energy = energy;

        if (start + available >= audio.size()) {
            break;
        }
    }

    return features;
}
