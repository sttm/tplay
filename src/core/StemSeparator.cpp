#include "StemSeparator.hpp"

#include "MacActivity.hpp"
#include "ProcessRunner.hpp"

#include "model.hpp"
#include <Eigen/Core>
#include <unsupported/Eigen/CXX11/Tensor>
#include <libnyquist/Common.h>
#include <libnyquist/Decoders.h>
#include <libnyquist/Encoders.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(TPLAY_WITH_ONNXRUNTIME)
#include <onnxruntime_cxx_api.h>
#include <coreml_provider_factory.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <pthread/qos.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kOnnxSources = 4;
constexpr int kOnnxChannels = 2;
constexpr int kOnnxSampleRate = 44100;
constexpr int kOnnxSamples = 343980;

class ScopedStemActivity {
public:
    ScopedStemActivity()
    {
#if defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        IOReturn result = IOPMAssertionCreateWithName(
            kIOPMAssertionTypePreventUserIdleSystemSleep,
            kIOPMAssertionLevelOn,
            CFSTR("TPlay stem separation"),
            &assertion_);
        if (result != kIOReturnSuccess) {
            assertion_ = kIOPMNullAssertionID;
        }
#endif
    }

    ~ScopedStemActivity()
    {
#if defined(__APPLE__)
        if (assertion_ != kIOPMNullAssertionID) {
            IOPMAssertionRelease(assertion_);
        }
#endif
    }

    ScopedStemActivity(const ScopedStemActivity&) = delete;
    ScopedStemActivity& operator=(const ScopedStemActivity&) = delete;

private:
#if defined(__APPLE__)
    IOPMAssertionID assertion_ = kIOPMNullAssertionID;
    MacActivity macActivity_{"TPlay stem separation"};
#endif
};

bool validTrack(const Track& track)
{
    return track.type == EntryType::File &&
           !track.id.empty() &&
           track.id.find('\0') == std::string::npos &&
           fs::is_regular_file(track.id);
}

std::string stemName(int stems)
{
    if (stems == 2) {
        return "2 stems";
    }
    if (stems == 6) {
        return "6 stems";
    }
    return "4 stems";
}

std::string compactOutput(std::string value)
{
    value = ProcessRunner::trim(std::move(value));
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    while (value.find("  ") != std::string::npos) {
        value.erase(value.find("  "), 1);
    }
    if (value.size() > 240) {
        value = "..." + value.substr(value.size() - 237);
    }
    return value;
}

std::string outputExtension(const DemucsConfig& config)
{
    if (config.outputFormat == "mp3") {
        return ".mp3";
    }
    if (config.outputFormat == "flac") {
        return ".flac";
    }
    return ".wav";
}

std::string displayStemName(const std::string& stem)
{
    static const std::map<std::string, std::string> names = {
        {"vocals", "acapella"},
        {"no_vocals", "instrumental"},
        {"drums", "drums"},
        {"bass", "bass"},
        {"other", "other"},
        {"guitar", "guitar"},
        {"piano", "piano"},
    };
    auto found = names.find(stem);
    return found == names.end() ? stem : found->second;
}

fs::path uniqueStemPath(const fs::path& directory,
                        const std::string& baseName,
                        const std::string& stem,
                        const std::string& extension)
{
    fs::path candidate = directory /
        (baseName + " (" + displayStemName(stem) + ")" + extension);
    int index = 2;
    std::error_code ec;
    while (fs::exists(candidate, ec)) {
        candidate = directory /
            (baseName + " (" + displayStemName(stem) + " " +
             std::to_string(index) + ")" + extension);
        index++;
    }
    return candidate;
}

fs::path separatedTrackDirectory(const Track& track,
                                 const std::string& modelName,
                                 const fs::path& output)
{
    return output /
        (modelName.empty() ? "htdemucs" : modelName) /
        fs::path(track.id).stem();
}

std::vector<fs::path> modelDirectories()
{
    fs::path executable_dir = ProcessRunner::executableDirectory();
    return {
        executable_dir / "models",
        executable_dir.parent_path() / "models",
        executable_dir.parent_path().parent_path() / "models",
        fs::current_path() / "models",
        fs::current_path().parent_path() / "models",
    };
}

std::optional<fs::path> findModelByNames(const std::vector<std::string>& names)
{
    for (const auto& directory : modelDirectories()) {
        for (const auto& name : names) {
            std::error_code ec;
            fs::path candidate = directory / name;
            if (fs::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

std::optional<fs::path> findHtdemucsModel()
{
    return findModelByNames({
        "ggml-model-htdemucs-4s-f16.bin",
        "ggml-model-htdemucs-4s.bin",
    });
}

std::optional<fs::path> findHtdemucsVocalsModel()
{
    return findModelByNames({
        "ggml-model-htdemucs_ft_vocals-4s-f16.bin",
        "ggml-model-htdemucs_ft_vocals-4s.bin",
    });
}

std::optional<fs::path> findHtdemucs6Model()
{
    return findModelByNames({
        "ggml-model-htdemucs-6s-f16.bin",
        "ggml-model-htdemucs_6s-f16.bin",
        "ggml-model-htdemucs-6s.bin",
        "ggml-model-htdemucs_6s.bin",
    });
}

std::optional<fs::path> findHtdemucsOnnxModel()
{
    return findModelByNames({
        "htdemucs_fp16weights.onnx",
        "htdemucs.onnx",
    });
}

std::optional<fs::path> findHtdemucsFtVocalsOnnxModel()
{
    return findModelByNames({
        "htdemucs_ft_vocals.onnx",
        "htdemucs-ft-vocals.onnx",
    });
}

fs::path makeTempDirectory()
{
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path temp_dir = fs::temp_directory_path() /
        ("tplay-demucs-" + std::to_string(tick));
    std::error_code ec;
    fs::create_directories(temp_dir, ec);
    if (ec) {
        throw std::runtime_error("Could not create temp directory: " + ec.message());
    }
    return temp_dir;
}

bool convertInputToDemucsWav(const std::string& ffmpeg,
                             const fs::path& input,
                             const fs::path& output,
                             std::string* error)
{
    std::string ffmpeg_output;
    int exit_code = ProcessRunner::runWithCombinedOutput(
        {ffmpeg,
         "-y",
         "-i",
         input.string(),
         "-ac",
         "2",
         "-ar",
         std::to_string(demucscpp::SUPPORTED_SAMPLE_RATE),
         "-vn",
         "-f",
         "wav",
         output.string()},
        &ffmpeg_output);
    if (exit_code == 0) {
        return true;
    }
    if (error != nullptr) {
        *error = compactOutput(ffmpeg_output);
    }
    return false;
}

Eigen::MatrixXf loadAudioFile(const fs::path& filename)
{
    auto file_data = std::make_shared<nqr::AudioData>();
    nqr::NyquistIO loader;
    loader.Load(file_data.get(), filename.string());

    if (file_data->sampleRate != demucscpp::SUPPORTED_SAMPLE_RATE) {
        throw std::runtime_error("Demucs input must be 44100 Hz");
    }
    if (file_data->channelCount != 1 && file_data->channelCount != 2) {
        throw std::runtime_error("Demucs input must be mono or stereo");
    }

    size_t samples_per_channel = file_data->samples.size() /
        (size_t)file_data->channelCount;
    Eigen::MatrixXf waveform(2, (Eigen::Index)samples_per_channel);
    if (file_data->channelCount == 1) {
        for (size_t i = 0; i < samples_per_channel; ++i) {
            waveform(0, (Eigen::Index)i) = file_data->samples[i];
            waveform(1, (Eigen::Index)i) = file_data->samples[i];
        }
        return waveform;
    }

    for (size_t i = 0; i < samples_per_channel; ++i) {
        waveform(0, (Eigen::Index)i) = file_data->samples[2 * i];
        waveform(1, (Eigen::Index)i) = file_data->samples[2 * i + 1];
    }
    return waveform;
}

bool writeWavFile(const Eigen::MatrixXf& waveform, const fs::path& filename)
{
    auto file_data = std::make_shared<nqr::AudioData>();
    file_data->sampleRate = demucscpp::SUPPORTED_SAMPLE_RATE;
    file_data->channelCount = 2;
    file_data->samples.resize((size_t)waveform.cols() * 2);

    for (Eigen::Index i = 0; i < waveform.cols(); ++i) {
        file_data->samples[(size_t)2 * (size_t)i] = waveform(0, i);
        file_data->samples[(size_t)2 * (size_t)i + 1] = waveform(1, i);
    }

    return nqr::encode_wav_to_disk(
               {file_data->channelCount, nqr::PCM_FLT, nqr::DITHER_TRIANGLE},
               file_data.get(),
               filename.string()) == nqr::NoError;
}

Eigen::MatrixXf targetWaveform(const Eigen::Tensor3dXf& targets, int target)
{
    Eigen::MatrixXf waveform(2, targets.dimension(2));
    for (Eigen::Index channel = 0; channel < 2; ++channel) {
        for (Eigen::Index sample = 0; sample < waveform.cols(); ++sample) {
            waveform(channel, sample) = targets(target, channel, sample);
        }
    }
    return waveform;
}

std::vector<float> makeOnnxWindow(int n, int overlap)
{
    std::vector<float> window((size_t)n, 1.0f);
    for (int i = 0; i < overlap; ++i) {
        float value = overlap <= 1 ? 1.0f : (float)i / (float)(overlap - 1);
        window[(size_t)i] = value;
        window[(size_t)n - 1 - (size_t)i] = value;
    }
    return window;
}

#if defined(TPLAY_WITH_ONNXRUNTIME)
Eigen::Tensor3dXf runOnnxDemucs(const fs::path& model_path,
                                const Eigen::MatrixXf& audio,
                                const std::string& backend,
                                const std::function<void(float, const std::string&)>& progress)
{
    if (demucscpp::SUPPORTED_SAMPLE_RATE != kOnnxSampleRate) {
        throw std::runtime_error("Demucs ONNX expects 44100 Hz audio");
    }

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "tplay-demucs");
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.SetIntraOpNumThreads(1);

    if (backend == "coreml") {
        uint32_t flags = COREML_FLAG_USE_CPU_AND_GPU |
                         COREML_FLAG_ONLY_ALLOW_STATIC_INPUT_SHAPES;
        OrtStatus* status =
            OrtSessionOptionsAppendExecutionProvider_CoreML(options, flags);
        if (status != nullptr) {
            std::string message = Ort::GetApi().GetErrorMessage(status);
            Ort::GetApi().ReleaseStatus(status);
            throw std::runtime_error("Could not enable CoreML provider: " + message);
        }
    }

    Ort::Session session(env, model_path.string().c_str(), options);
    Ort::MemoryInfo memory_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    const Eigen::Index total = audio.cols();
    const int overlap = kOnnxSamples / 4;
    const int stride = kOnnxSamples - overlap;
    const int chunks = std::max<int>(
        1,
        (int)((total + stride - 1) / stride));
    std::vector<float> window = makeOnnxWindow(kOnnxSamples, overlap);
    std::vector<float> input((size_t)kOnnxChannels * kOnnxSamples, 0.0f);
    std::vector<float> output((size_t)kOnnxSources * kOnnxChannels * (size_t)total,
                              0.0f);
    std::vector<float> weight((size_t)total, 0.0f);

    std::array<int64_t, 3> input_shape = {1, kOnnxChannels, kOnnxSamples};
    const char* input_names[] = {"mix"};
    const char* output_names[] = {"stems"};

    for (int chunk_index = 0; chunk_index < chunks; ++chunk_index) {
        Eigen::Index start = (Eigen::Index)chunk_index * stride;
        Eigen::Index end = std::min<Eigen::Index>(start + kOnnxSamples, total);
        Eigen::Index clen = end - start;
        std::fill(input.begin(), input.end(), 0.0f);
        for (int channel = 0; channel < kOnnxChannels; ++channel) {
            for (Eigen::Index sample = 0; sample < clen; ++sample) {
                input[(size_t)channel * kOnnxSamples + (size_t)sample] =
                    audio(channel, start + sample);
            }
        }

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            input.data(),
            input.size(),
            input_shape.data(),
            input_shape.size());
        auto results = session.Run(Ort::RunOptions{nullptr},
                                   input_names,
                                   &input_tensor,
                                   1,
                                   output_names,
                                   1);
        if (results.empty() || !results[0].IsTensor()) {
            throw std::runtime_error("ONNX Demucs did not return stems tensor");
        }
        const float* stems = results[0].GetTensorData<float>();
        for (int stem = 0; stem < kOnnxSources; ++stem) {
            for (int channel = 0; channel < kOnnxChannels; ++channel) {
                for (Eigen::Index sample = 0; sample < clen; ++sample) {
                    float w = window[(size_t)sample];
                    size_t out_index =
                        ((size_t)stem * kOnnxChannels + (size_t)channel) *
                            (size_t)total +
                        (size_t)(start + sample);
                    size_t stem_index =
                        ((size_t)stem * kOnnxChannels + (size_t)channel) *
                            kOnnxSamples +
                        (size_t)sample;
                    output[out_index] += stems[stem_index] * w;
                }
            }
        }
        for (Eigen::Index sample = 0; sample < clen; ++sample) {
            weight[(size_t)(start + sample)] += window[(size_t)sample];
        }
        progress((float)(chunk_index + 1) / (float)chunks,
                 "CoreML chunk " + std::to_string(chunk_index + 1) + "/" +
                     std::to_string(chunks));
    }

    Eigen::Tensor3dXf targets(kOnnxSources, kOnnxChannels, total);
    for (int stem = 0; stem < kOnnxSources; ++stem) {
        for (int channel = 0; channel < kOnnxChannels; ++channel) {
            for (Eigen::Index sample = 0; sample < total; ++sample) {
                float w = std::max(weight[(size_t)sample], 1e-8f);
                size_t out_index =
                    ((size_t)stem * kOnnxChannels + (size_t)channel) *
                        (size_t)total +
                    (size_t)sample;
                targets(stem, channel, sample) = output[out_index] / w;
            }
        }
    }
    return targets;
}
#endif

bool convertStemOutput(const std::string& ffmpeg,
                       const fs::path& wav_path,
                       const fs::path& target_path,
                       const DemucsConfig& config,
                       std::string* error)
{
    if (config.outputFormat == "wav") {
        return true;
    }

    std::vector<std::string> args = {
        ffmpeg,
        "-y",
        "-i",
        wav_path.string(),
    };
    if (config.outputFormat == "mp3") {
        args.push_back("-codec:a");
        args.push_back("libmp3lame");
        args.push_back("-q:a");
        args.push_back("2");
    } else if (config.outputFormat == "flac") {
        args.push_back("-compression_level");
        args.push_back("5");
    }
    args.push_back(target_path.string());

    std::string ffmpeg_output;
    int exit_code = ProcessRunner::runWithCombinedOutput(args, &ffmpeg_output);
    if (exit_code == 0) {
        return true;
    }
    if (error != nullptr) {
        *error = compactOutput(ffmpeg_output);
    }
    return false;
}

bool writeStem(const Eigen::MatrixXf& waveform,
               const std::string& stem,
               const fs::path& output_dir,
               const std::string& base_name,
               const DemucsConfig& config,
               const fs::path& temp_dir,
               const std::string& ffmpeg,
               std::string* error)
{
    fs::path target = uniqueStemPath(output_dir, base_name, stem, outputExtension(config));
    fs::path wav_target = config.outputFormat == "wav"
        ? target
        : temp_dir / (stem + ".wav");
    if (!writeWavFile(waveform, wav_target)) {
        if (error != nullptr) {
            *error = "Could not write " + wav_target.string();
        }
        return false;
    }
    return convertStemOutput(ffmpeg, wav_target, target, config, error);
}

}  // namespace

StemSeparator::~StemSeparator()
{
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool StemSeparator::start(const Track& track, const DemucsConfig& config)
{
    if (!validTrack(track)) {
        setState(StemSeparationState::Error, "Error", "Select an audio track");
        return false;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        setState(StemSeparationState::Error, "Error", "Stem separation already running");
        return false;
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        startedAt_ = std::chrono::steady_clock::now();
    }

    setState(StemSeparationState::Running,
             "Separating",
             track.title + " | " + stemName(config.stems),
             {},
             0.08f);
    worker_ = std::thread(&StemSeparator::run, this, Request{track, config});
    return true;
}

StemSeparationSnapshot StemSeparator::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    StemSeparationSnapshot snapshot = snapshot_;
    if (snapshot.state == StemSeparationState::Running) {
        snapshot.elapsedSeconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - startedAt_).count();
    }
    return snapshot;
}

void StemSeparator::run(Request request)
{
    ScopedStemActivity activity;
    fs::path temp_dir;
    auto finish = [&] {
        if (!temp_dir.empty()) {
            std::error_code cleanup_ec;
            fs::remove_all(temp_dir, cleanup_ec);
        }
        running_ = false;

        if (onFinished_) {
            onFinished_();
        }
    };

    auto fail = [&](std::string detail, std::string output = {}) {
        setState(StemSeparationState::Error, "Error", std::move(detail),
                 std::move(output), 1.0f);
        finish();
    };

    if (request.config.jobs > 0) {
        Eigen::setNbThreads(request.config.jobs);
    }

    auto ffmpeg = ProcessRunner::findExecutable("ffmpeg");
    if (!ffmpeg) {
        fail("ffmpeg not found. It is still needed for audio format conversion.");
        return;
    }

    bool use_6source_model = request.config.stems == 6;
    std::string effective_model_name = use_6source_model
        ? "htdemucs_6s"
        : (request.config.stems == 2 ? "htdemucs_ft_vocals" : "htdemucs");
    std::optional<fs::path> model_path;
    std::optional<fs::path> onnx_model_path;
    bool strict_coreml = request.config.backend == "coreml";
    bool strict_onnx_cpu = request.config.backend == "onnx_cpu";
    bool try_onnx = strict_coreml || strict_onnx_cpu ||
        request.config.backend == "auto";
    bool use_onnx = false;
    std::string onnx_provider = strict_coreml ? "coreml" : "cpu";

    if (try_onnx) {
        if (use_6source_model) {
            if (strict_coreml || strict_onnx_cpu) {
                fail("ONNX Demucs backend currently supports 2 or 4 stems only. Use backend = \"cpu\" for 6 stems.");
                return;
            }
        } else {
#if defined(TPLAY_WITH_ONNXRUNTIME)
            onnx_model_path = request.config.stems == 2
                ? findHtdemucsFtVocalsOnnxModel()
                : findHtdemucsOnnxModel();
            if (onnx_model_path) {
                effective_model_name = request.config.stems == 2
                    ? (onnx_provider == "cpu"
                           ? "htdemucs_ft_vocals_onnx_cpu"
                           : "htdemucs_ft_vocals_coreml")
                    : (onnx_provider == "cpu"
                           ? "htdemucs_onnx_cpu"
                           : "htdemucs_coreml");
                use_onnx = true;
            } else if (strict_coreml || strict_onnx_cpu) {
                fail(request.config.stems == 2
                         ? "Missing ONNX vocals model. Put htdemucs_ft_vocals.onnx in models/."
                         : "Missing ONNX Demucs model. Put htdemucs_fp16weights.onnx or htdemucs.onnx in models/.");
                return;
            }
#else
            if (strict_coreml || strict_onnx_cpu) {
                fail("This binary was built without ONNX Runtime. Install onnxruntime and rebuild.");
                return;
            }
#endif
        }
    }

    if (!use_onnx) {
        if (use_6source_model) {
            model_path = findHtdemucs6Model();
            if (!model_path) {
                fail("Missing htdemucs_6s model. Put ggml-model-htdemucs-6s-f16.bin in models/.");
                return;
            }
        } else if (request.config.stems == 4) {
            model_path = findHtdemucsModel();
            if (!model_path) {
                fail("Missing htdemucs model. Put ggml-model-htdemucs-4s-f16.bin in models/.");
                return;
            }
        } else {
            model_path = findHtdemucsVocalsModel();
            if (!model_path) {
                fail("Missing htdemucs_ft_vocals model. Put ggml-model-htdemucs_ft_vocals-4s-f16.bin in models/.");
                return;
            }
        }
    }

    fs::path output = request.config.outputDirectory.empty()
        ? fs::path(request.track.id).parent_path() / "separated"
        : fs::path(request.config.outputDirectory);
    fs::path track_output = separatedTrackDirectory(
        request.track, effective_model_name, output);
    std::error_code ec;
    fs::create_directories(track_output, ec);
    if (ec) {
        fail(ec.message(), track_output.string());
        return;
    }

    setState(StemSeparationState::Running,
             "Separating",
             request.track.title,
             track_output.string(),
             0.15f);

    try {
        temp_dir = makeTempDirectory();
        fs::path demucs_wav = temp_dir / "input.wav";

        std::string ffmpeg_error;
        if (!convertInputToDemucsWav(*ffmpeg, fs::path(request.track.id),
                                     demucs_wav, &ffmpeg_error)) {
            fail(ffmpeg_error.empty()
                     ? "Could not prepare audio for Demucs"
                     : "Could not prepare audio for Demucs: " + ffmpeg_error,
                 track_output.string());
            return;
        }

        Eigen::MatrixXf audio = loadAudioFile(demucs_wav);
        Eigen::Tensor3dXf targets;
        auto last_progress_update = std::chrono::steady_clock::now() -
            std::chrono::seconds(1);
        float last_progress = -1.0f;
        auto progress_callback = [&](float progress, const std::string& log) {
            auto now = std::chrono::steady_clock::now();
            bool enough_time = now - last_progress_update >=
                std::chrono::milliseconds(250);
            bool enough_progress = std::abs(progress - last_progress) >= 0.01f;
            if (!enough_time && !enough_progress && progress < 1.0f) {
                return;
            }
            last_progress_update = now;
            last_progress = progress;
            std::string detail = log.empty() ? request.track.title : log;
            setState(StemSeparationState::Running,
                     "Separating",
                     std::move(detail),
                     track_output.string(),
                     0.30f + std::clamp(progress, 0.0f, 1.0f) * 0.60f);
        };

        if (use_onnx) {
#if defined(TPLAY_WITH_ONNXRUNTIME)
            setState(StemSeparationState::Running,
                     "Separating",
                     onnx_provider == "coreml"
                         ? "Loading htdemucs CoreML CPU+GPU"
                         : "Loading htdemucs ONNX CPU",
                     track_output.string(),
                     0.25f);
            try {
                targets = runOnnxDemucs(*onnx_model_path,
                                        audio,
                                        onnx_provider,
                                        progress_callback);
            } catch (const std::exception& ex) {
                if (strict_coreml || strict_onnx_cpu) {
                    fail(ex.what(), track_output.string());
                    return;
                }
                setState(StemSeparationState::Running,
                         "Separating",
                         std::string("ONNX failed, falling back to CPU: ") + ex.what(),
                         track_output.string(),
                         0.25f);
                use_onnx = false;
                if (request.config.stems == 4) {
                    model_path = findHtdemucsModel();
                } else {
                    model_path = findHtdemucsVocalsModel();
                }
                if (!model_path) {
                    fail("CoreML failed and CPU model is missing.",
                         track_output.string());
                    return;
                }
            }
#endif
        }

        if (!use_onnx) {
            setState(StemSeparationState::Running,
                     "Separating",
                     use_6source_model
                         ? "Loading htdemucs_6s CPU"
                         : (request.config.stems == 2
                                ? "Loading htdemucs_ft vocals CPU"
                                : "Loading htdemucs CPU"),
                     track_output.string(),
                     0.25f);

            demucscpp::demucs_model* model = nullptr;
            {
                std::lock_guard<std::mutex> model_lock(cpuModelMutex_);
                if (!cpuModelLoaded_ || cpuModelPath_ != *model_path) {
                    setState(StemSeparationState::Running,
                             "Separating",
                             "Loading htdemucs CPU model",
                             track_output.string(),
                             0.25f);
                    demucscpp::demucs_model loaded{};
                    if (!demucscpp::load_demucs_model(model_path->string(),
                                                      &loaded)) {
                        fail("Could not load htdemucs model",
                             track_output.string());
                        return;
                    }
                    cpuModel_ = std::move(loaded);
                    cpuModelPath_ = *model_path;
                    cpuModelLoaded_ = true;
                }
                model = &cpuModel_;
            }
            if (use_6source_model && model->is_4sources) {
                fail("Expected the 6-source htdemucs_6s model",
                     track_output.string());
                return;
            }
            if (!use_6source_model && !model->is_4sources) {
                fail("Expected the 4-source htdemucs model",
                     track_output.string());
                return;
            }
            targets = demucscpp::demucs_inference_configured(
                *model,
                audio,
                progress_callback,
                request.config.overlap,
                request.config.shiftSeconds);
        }

        setState(StemSeparationState::Running,
                 "Separating",
                 "Writing stems",
                 track_output.string(),
                 0.92f);

        std::string base_name = fs::path(request.track.id).stem().string();
        std::string write_error;
        if (request.config.stems == 2) {
            if ((request.config.twoStemSource.empty()
                 ? "vocals"
                 : request.config.twoStemSource) != "vocals") {
                fail("Two-stem C++ Demucs currently supports vocals only.",
                     track_output.string());
                return;
            }

            Eigen::MatrixXf vocals = targetWaveform(targets, 3);
            Eigen::MatrixXf instrumental = audio - vocals;
            if (!writeStem(vocals, "vocals", track_output, base_name,
                           request.config, temp_dir, *ffmpeg, &write_error) ||
                !writeStem(instrumental, "no_vocals", track_output, base_name,
                           request.config, temp_dir, *ffmpeg, &write_error)) {
                fail(write_error.empty() ? "Could not write stems" : write_error,
                     track_output.string());
                return;
            }
        } else {
            std::vector<std::pair<int, std::string>> stems = {
                {0, "drums"},
                {1, "bass"},
                {2, "other"},
                {3, "vocals"},
            };
            if (request.config.stems == 6) {
                stems.push_back({4, "guitar"});
                stems.push_back({5, "piano"});
            }
            for (const auto& [target, stem] : stems) {
                if (!writeStem(targetWaveform(targets, target), stem, track_output,
                               base_name, request.config, temp_dir, *ffmpeg,
                               &write_error)) {
                    fail(write_error.empty() ? "Could not write stems" : write_error,
                         track_output.string());
                    return;
                }
            }
        }
    } catch (const std::exception& ex) {
        fail(ex.what(), track_output.string());
        return;
    }

    std::string detail = request.track.title + " | " + stemName(request.config.stems);
    setState(StemSeparationState::Done,
             "Stems ready",
             std::move(detail),
             track_output.string(),
             1.0f);
    finish();
}

void StemSeparator::setOnFinished(std::function<void()> callback)
{
    onFinished_ = std::move(callback);
}

void StemSeparator::setState(StemSeparationState state,
                             std::string message,
                             std::string detail,
                             std::string outputDirectory,
                             float progress)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = state;
    snapshot_.message = std::move(message);
    snapshot_.detail = std::move(detail);
    snapshot_.outputDirectory = std::move(outputDirectory);
    snapshot_.progress = std::clamp(progress, 0.0f, 1.0f);
    snapshot_.elapsedSeconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startedAt_).count();
}
