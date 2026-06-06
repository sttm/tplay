#include "ManualCueEditor.hpp"

#include "KeyBindings.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

#include <ftxui/dom/canvas.hpp>

using namespace ftxui;

namespace {

std::string formatPlaybackTime(double seconds)
{
    int centiseconds = std::max(0, (int)std::llround(seconds * 100.0));
    int total = centiseconds / 100;
    return std::format("{:02}:{:02}.{:02}",
                       total / 60,
                       total % 60,
                       centiseconds % 100);
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

bool keyMatches(const Event& event, std::initializer_list<std::string> keys)
{
    return ui::keyMatches(event, keys);
}

Color colorFromRgb(std::uint32_t rgb)
{
    return Color::RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

std::uint32_t cuePaletteColor(int index)
{
    static constexpr std::array<std::uint32_t, 8> colors = {
        0x35d66b, 0xff4d5a, 0x38a3ff, 0xffa23a,
        0xf3df3f, 0xa873ff, 0x20d0d8, 0xffffff,
    };
    return colors[(size_t)std::clamp(index, 0, 7)];
}

}  // namespace

ManualCueEditor::ManualCueEditor(AppController& controller,
                                 std::string& commandStatus,
                                 std::atomic_bool& refreshActive)
    : controller_(controller),
      commandStatus_(commandStatus),
      refreshActive_(refreshActive)
{
    SliderOption<int> speed_options;
    speed_options.value = &speedValue_;
    speed_options.min = &speedMin_;
    speed_options.max = &speedMax_;
    speed_options.increment = &speedStep_;
    speed_options.on_change = [this] {
        setPlaybackRateFromValue(speedValue_);
    };
    speedSlider_ = Slider(speed_options);
}

bool ManualCueEditor::isOpen() const
{
    return open_;
}

double ManualCueEditor::duration() const
{
    if (waveform_.duration > 0.0) {
        return waveform_.duration;
    }
    return std::max(0.0, track_.duration);
}

void ManualCueEditor::clampView()
{
    double track_duration = duration();
    zoom_ = std::clamp(zoom_, 1.0, 1024.0);
    double visible = track_duration > 0.0 ? track_duration / zoom_ : 0.0;
    if (track_duration <= 0.0 || visible >= track_duration) {
        viewStart_ = 0.0;
    } else {
        viewStart_ = std::clamp(viewStart_, 0.0, track_duration - visible);
    }
    playhead_ = std::clamp(playhead_, 0.0, std::max(0.0, track_duration));
}

double ManualCueEditor::currentPosition()
{
    auto playback = controller_.previewPlaybackSnapshot();
    auto now = std::chrono::steady_clock::now();
    if (controller_.previewPlayingTrackId() == track_.id &&
        (playback.state == PlaybackState::Playing ||
         playback.state == PlaybackState::Paused)) {
        double track_duration = duration();
        bool raw_moved = std::abs(playback.positionSeconds - positionSample_) > 0.04;
        if (playback.state == PlaybackState::Paused ||
            playback.state != positionState_ ||
            raw_moved) {
            positionSample_ = playback.positionSeconds;
            positionSampleTime_ = now;
            positionState_ = playback.state;
        }
        double interpolated = positionSample_;
        if (playback.state == PlaybackState::Playing) {
            interpolated += std::chrono::duration<double>(
                now - positionSampleTime_).count() * controller_.previewPlaybackRate();
        }
        return std::clamp(interpolated, 0.0, track_duration);
    }
    positionState_ = playback.state;
    return std::clamp(playhead_, 0.0, duration());
}

void ManualCueEditor::seekTo(double seconds)
{
    double track_duration = duration();
    playhead_ = std::clamp(seconds, 0.0, std::max(0.0, track_duration));
    positionSample_ = playhead_;
    positionSampleTime_ = std::chrono::steady_clock::now();
    if (track_duration > 0.0 && controller_.previewPlayingTrackId() == track_.id) {
        controller_.seekPreviewPlayback(playhead_ / track_duration);
    }
    clampView();
}

void ManualCueEditor::setPlaybackRateFromValue(int value)
{
    speedValue_ = std::clamp(value, 50, 200);
    controller_.setPreviewPlaybackRate((double)speedValue_ / 100.0);
}

void ManualCueEditor::setPreservePitch(bool preserve)
{
    preservePitch_ = preserve;
    controller_.setPreviewPreservePitch(preservePitch_);
}

void ManualCueEditor::setCue(int index)
{
    index = std::clamp(index, 0, 7);
    cues_[(size_t)index].set = true;
    cues_[(size_t)index].seconds = currentPosition();
    cues_[(size_t)index].colorRgb = cuePaletteColor(index);
    selected_ = index;
    closeSelected_ = false;
    controlRow_ = 0;
    dirty_ = true;
}

void ManualCueEditor::playFrom(double seconds)
{
    double track_duration = duration();
    if (track_duration <= 0.0) {
        return;
    }
    playhead_ = std::clamp(seconds, 0.0, track_duration);
    auto playback = controller_.previewPlaybackSnapshot();
    bool same_track = controller_.previewPlayingTrackId() == track_.id;
    if (!same_track ||
        playback.state == PlaybackState::Stopped ||
        playback.state == PlaybackState::Error) {
        controller_.playPreviewTrack(track_);
    }
    controller_.seekPreviewPlayback(playhead_ / track_duration);
    playback = controller_.previewPlaybackSnapshot();
    if (controller_.previewPlayingTrackId() == track_.id &&
        playback.state == PlaybackState::Paused) {
        controller_.togglePreviewPause();
    }
    if (followPlayhead_) {
        double visible = track_duration / zoom_;
        viewStart_ = playhead_ - visible * 0.5;
    }
    clampView();
}

void ManualCueEditor::activateCue(int index)
{
    index = std::clamp(index, 0, 7);
    selected_ = index;
    closeSelected_ = false;
    controlRow_ = 0;
    const auto& slot = cues_[(size_t)index];
    if (slot.set) {
        playFrom(slot.seconds);
    } else {
        setCue(index);
    }
}

void ManualCueEditor::deleteSelectedCue()
{
    cues_[(size_t)selected_] = {};
    dirty_ = true;
}

bool ManualCueEditor::save()
{
    if (!dirty_ || track_.id.empty()) {
        return true;
    }
    std::vector<SeratoCue> cues;
    for (int i = 0; i < 8; ++i) {
        const auto& slot = cues_[(size_t)i];
        if (!slot.set) {
            continue;
        }
        cues.push_back({
            i,
            "CUE " + std::to_string(i + 1),
            slot.seconds,
            slot.colorRgb,
        });
    }
    std::string error;
    if (!controller_.writeManualCues(track_, cues, error)) {
        commandStatus_ = "Manual cues error: " + error;
        return false;
    }
    dirty_ = false;
    commandStatus_ = "Manual cues saved: " + track_.title;
    controller_.scanDirectory(controller_.currentPath(), true);
    return true;
}

bool ManualCueEditor::open(const Track& track,
                           const AutoCueFeatures* preparedWaveform,
                           const std::string* preparedError)
{
    if (track.type != EntryType::File || track.id.empty()) {
        commandStatus_ = "Select a track for manual cues";
        return false;
    }

    controller_.stopPlayback();
    controller_.stopPreviewPlayback();
    track_ = track;
    cues_ = {};
    {
        std::string cue_error;
        for (const auto& cue : controller_.readManualCues(track_, cue_error)) {
            int index = std::clamp(cue.index, 0, 7);
            cues_[(size_t)index].set = true;
            cues_[(size_t)index].seconds = cue.seconds;
            cues_[(size_t)index].colorRgb = cue.colorRgb;
        }
        if (!cue_error.empty()) {
            commandStatus_ = cue_error;
        }
    }
    selected_ = 0;
    closeSelected_ = false;
    controlRow_ = 0;
    speedSelected_ = 1;
    dirty_ = false;
    zoom_ = 3.0;
    waveRows_ = 12;
    viewStart_ = 0.0;
    playhead_ = 0.0;
    positionSample_ = 0.0;
    speedValue_ = (int)std::round(controller_.previewPlaybackRate() * 100.0);
    preservePitch_ = controller_.previewPreservePitch();
    positionState_ = PlaybackState::Stopped;
    positionSampleTime_ = std::chrono::steady_clock::now();
    waveform_ = {};
    waveformLoading_ = true;
    followPlayhead_ = false;
    scrubbing_ = false;
    scrubPausedPlayback_ = false;
    commandStatus_ = "Loading waveform: " + track_.title;

    std::string error;
    if (preparedWaveform != nullptr) {
        waveform_ = *preparedWaveform;
        if (preparedError != nullptr) {
            error = *preparedError;
        }
    } else {
        waveform_ = controller_.waveformForTrack(track_, error);
    }
    waveformLoading_ = false;
    if (!error.empty()) {
        commandStatus_ = error;
    } else {
        commandStatus_ = "Manual cues: " + track_.title;
    }
    open_ = true;
    refreshActive_ = true;
    clampView();
    return true;
}

void ManualCueEditor::close()
{
    save();
    open_ = false;
    refreshActive_ = false;
    track_ = {};
    controller_.stopPreviewPlayback();
}

Element ManualCueEditor::renderOverlay(Element mainLayout, int contentWidth)
{
    closeBox_ = Box{};
    waveBox_ = Box{};
    speedResetBox_ = Box{};
    speedSliderBox_ = Box{};
    pitchLockBox_ = Box{};
    for (auto& box : cueBoxes_) {
        box = Box{};
    }

    int dialog_width = std::clamp(contentWidth - 4, 64, 118);
    int wave_rows = std::clamp(waveRows_, 10, 24);
    int wave_columns = std::max(24, dialog_width - 4);
    int canvas_width = wave_columns * 2;
    int canvas_height = wave_rows * 4;
    double track_duration = duration();
    double visible_seconds = track_duration > 0.0
        ? std::max(0.05, track_duration / zoom_)
        : 1.0;
    double view_start = std::clamp(viewStart_,
                                   0.0,
                                   std::max(0.0, track_duration - visible_seconds));
    double view_end = std::min(track_duration, view_start + visible_seconds);
    double playhead = currentPosition();
    if (followPlayhead_ && track_duration > 0.0) {
        playhead_ = playhead;
        viewStart_ = playhead - visible_seconds * 0.5;
        clampView();
        view_start = std::clamp(viewStart_,
                                0.0,
                                std::max(0.0, track_duration - visible_seconds));
        view_end = std::min(track_duration, view_start + visible_seconds);
    }
    Color waveform_color = colorFromRgb(controller_.config().autoCue.manualWaveformColorRgb);
    Color playhead_color = colorFromRgb(controller_.config().autoCue.manualPlayheadColorRgb);

    auto secondToCanvasX = [&](double seconds) {
        if (view_end <= view_start) {
            return 0;
        }
        double ratio = (seconds - view_start) / (view_end - view_start);
        return (int)std::llround(std::clamp(ratio, 0.0, 1.0) *
                                 (double)(canvas_width - 1));
    };

    Elements cue_cells;
    cue_cells.reserve(16);
    for (int i = 0; i < 8; ++i) {
        const auto& slot = cues_[(size_t)i];
        std::string label = std::to_string(i + 1);
        label += slot.set ? " " + formatPlaybackTime(slot.seconds) : " --:--";
        Element cell = text(label) |
            center |
            size(WIDTH, EQUAL, 12) |
            border |
            reflect(cueBoxes_[(size_t)i]);
        if (slot.set) {
            cell = cell | color(colorFromRgb(slot.colorRgb));
        } else {
            cell = cell | dim;
        }
        if (controlRow_ == 0 && selected_ == i) {
            cell = cell | bold | inverted;
        }
        cue_cells.push_back(cell);
        if (i != 7) {
            cue_cells.push_back(text(" "));
        }
    }

    Element waveform = canvas(canvas_width, canvas_height, [&](Canvas& c) {
        int mid = canvas_height / 2;
        c.DrawPointLine(0, mid, canvas_width - 1, mid, Color::GrayDark);

        const auto& energy = waveform_.energyCurve;
        if (!energy.empty() && waveform_.hopSeconds > 0.0) {
            int first_frame = std::clamp(
                (int)std::floor(view_start / waveform_.hopSeconds),
                0,
                std::max(0, (int)energy.size() - 1));
            int last_frame = std::clamp(
                (int)std::ceil(view_end / waveform_.hopSeconds),
                first_frame,
                std::max(0, (int)energy.size() - 1));
            float max_energy = 0.0f;
            for (float value : energy) {
                max_energy = std::max(max_energy, value);
            }
            if (max_energy > 0.0f) {
                for (int x = 0; x < canvas_width; ++x) {
                    double ratio = canvas_width <= 1
                        ? 0.0
                        : (double)x / (double)(canvas_width - 1);
                    double seconds = view_start + ratio * (view_end - view_start);
                    int center = std::clamp(
                        (int)std::llround(seconds / waveform_.hopSeconds),
                        0,
                        std::max(0, (int)energy.size() - 1));
                    int radius = std::max(1, (int)std::ceil(
                        (view_end - view_start) /
                        std::max(1, canvas_width) /
                        waveform_.hopSeconds));
                    float value = 0.0f;
                    for (int frame = center - radius; frame <= center + radius; ++frame) {
                        value = std::max(value, energy[(size_t)std::clamp(
                            frame, 0, (int)energy.size() - 1)]);
                    }
                    int amp = (int)std::llround(
                        std::min(1.0f, (value / max_energy) * 1.35f) *
                        (double)(canvas_height / 2 - 2));
                    c.DrawPointLine(x,
                                    std::max(0, mid - amp),
                                    x,
                                    std::min(canvas_height - 1, mid + amp),
                                    waveform_color);
                }
            }
        }

        for (int i = 0; i < 8; ++i) {
            const auto& slot = cues_[(size_t)i];
            if (!slot.set || slot.seconds < view_start || slot.seconds > view_end) {
                continue;
            }
            int x = secondToCanvasX(slot.seconds);
            c.DrawPointLine(x, 0, x, canvas_height - 1, colorFromRgb(slot.colorRgb));
            c.DrawText(std::clamp(x + 1, 0, canvas_width - 8),
                       0,
                       std::to_string(i + 1),
                       colorFromRgb(slot.colorRgb));
        }

        if (playhead >= view_start && playhead <= view_end) {
            int x = secondToCanvasX(playhead);
            c.DrawPointLine(x, 0, x, canvas_height - 1, playhead_color);
        }
    }) |
        size(WIDTH, EQUAL, wave_columns) |
        size(HEIGHT, EQUAL, wave_rows) |
        reflect(waveBox_);

    Element close_button = text("Close") |
        center |
        size(WIDTH, EQUAL, 12) |
        border |
        bold |
        reflect(closeBox_);
    if (closeSelected_) {
        close_button = close_button | inverted | color(Color::Yellow);
    } else {
        close_button = close_button | color(Color::Yellow);
    }

    std::string title_text = truncateEnd(track_.title, dialog_width - 4);
    std::string range_text = formatPlaybackTime(view_start) + " - " +
        formatPlaybackTime(view_end) + " / " + formatPlaybackTime(track_duration) +
        "  x" + std::to_string((int)zoom_);
    std::string helper = waveformLoading_
        ? "Loading waveform..."
        : "Arrows scroll/select | Enter set | 1-8 set/play | Shift+1-8 delete | T follow | +/- zoom | [] height";
    std::string follow_text = followPlayhead_ ? "follow on" : "follow off";
    int speed_width = std::max(18, dialog_width - 18);
    Element speed_reset = text("x") | center | size(WIDTH, EQUAL, 3) |
        reflect(speedResetBox_);
    Element speed_slider = speedSlider_->Render() | size(WIDTH, EQUAL, speed_width) |
        reflect(speedSliderBox_);
    Element pitch_lock = text(preservePitch_ ? "▣ ♩" : "□ ♩") | center |
        size(WIDTH, EQUAL, 5) | reflect(pitchLockBox_);
    if (controlRow_ == 1 && speedSelected_ == 0) speed_reset = speed_reset | inverted;
    if (controlRow_ == 1 && speedSelected_ == 1) speed_slider = speed_slider | inverted;
    if (controlRow_ == 1 && speedSelected_ == 2) pitch_lock = pitch_lock | inverted;
    Element speed_box = hbox({
        speed_reset,
        speed_slider,
        text(std::format("{:.2f}x", (double)speedValue_ / 100.0)) |
            center | size(WIDTH, EQUAL, 6),
        pitch_lock,
    }) | size(HEIGHT, EQUAL, 1);

    Element dialog = vbox({
        text("edit markers") | bold | center,
        text(title_text) | dim | center,
        separator(),
        hbox(cue_cells) | center,
        separator(),
        waveform | border,
        speed_box | center,
        separator(),
        hbox({
            text(formatPlaybackTime(playhead)) | bold,
            separator(),
            text(range_text) | dim,
            separator(),
            text(follow_text) | dim,
            filler(),
            close_button,
        }),
        // text(helper) | dim | center,
    }) |
        size(WIDTH, EQUAL, dialog_width) |
        border |
        clear_under |
        center;

    return dbox({mainLayout, dialog}) | flex;
}

bool ManualCueEditor::handleEvent(Event event)
{
    auto markerKey = [&](const Event& event,
                         const std::string& action,
                         std::initializer_list<std::string> defaults) {
        return ui::bindingMatches(event, controller_.config().keybindsMarker,
                                  action, defaults);
    };

    double track_duration = duration();
    auto visibleSeconds = [&] {
        return track_duration > 0.0 ? std::max(0.05, track_duration / zoom_) : 1.0;
    };
    auto centerViewOnPlayhead = [&] {
        double visible = visibleSeconds();
        viewStart_ = playhead_ - visible * 0.5;
        clampView();
    };
    auto zoomManual = [&](double factor) {
        playhead_ = currentPosition();
        zoom_ = std::clamp(zoom_ * factor, 1.0, 1024.0);
        centerViewOnPlayhead();
    };
    auto scrollManual = [&](double ratio) {
        double visible = visibleSeconds();
        viewStart_ += visible * ratio;
        clampView();
    };
    auto seekRelative = [&](double seconds) {
        seekTo(currentPosition() + seconds);
        centerViewOnPlayhead();
    };
    auto secondsAtMouse = [&](const Mouse& mouse) {
        double visible = visibleSeconds();
        int width = std::max(1, waveBox_.x_max - waveBox_.x_min);
        double local = std::clamp((double)(mouse.x - waveBox_.x_min),
                                  0.0,
                                  (double)width);
        double ratio = local / (double)width;
        return std::clamp(viewStart_ + ratio * visible,
                          0.0,
                          std::max(0.0, track_duration));
    };
    auto startScrub = [&](const Mouse& mouse) {
        auto playback = controller_.previewPlaybackSnapshot();
        scrubPausedPlayback_ = controller_.previewPlayingTrackId() == track_.id &&
            playback.state == PlaybackState::Playing;
        if (scrubPausedPlayback_) {
            controller_.togglePreviewPause();
        }
        scrubbing_ = true;
        scrubStartX_ = mouse.x;
        scrubStartPlayhead_ = secondsAtMouse(mouse);
        seekTo(scrubStartPlayhead_);
        centerViewOnPlayhead();
    };
    auto scrubToMouse = [&](const Mouse& mouse) {
        double visible = visibleSeconds();
        int width = std::max(1, waveBox_.x_max - waveBox_.x_min);
        double delta = (double)(mouse.x - scrubStartX_) / (double)width;
        seekTo(scrubStartPlayhead_ - delta * visible);
        centerViewOnPlayhead();
    };
    if (event.is_mouse()) {
        const Mouse& mouse = event.mouse();
        if (speedSliderBox_.Contain(mouse.x, mouse.y) &&
            speedSlider_ &&
            speedSlider_->OnEvent(event)) {
            controlRow_ = 1;
            speedSelected_ = 1;
            closeSelected_ = false;
            return true;
        }
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
            if (speedResetBox_.Contain(mouse.x, mouse.y)) {
                setPlaybackRateFromValue(100);
                controlRow_ = 1;
                speedSelected_ = 0;
                closeSelected_ = false;
                return true;
            }
            if (pitchLockBox_.Contain(mouse.x, mouse.y)) {
                setPreservePitch(!preservePitch_);
                controlRow_ = 1;
                speedSelected_ = 2;
                closeSelected_ = false;
                return true;
            }
            if (closeBox_.Contain(mouse.x, mouse.y)) {
                close();
                return true;
            }
            for (int i = 0; i < 8; ++i) {
                if (cueBoxes_[(size_t)i].Contain(mouse.x, mouse.y)) {
                    closeSelected_ = false;
                    controlRow_ = 0;
                    if (cues_[(size_t)i].set) {
                        playFrom(cues_[(size_t)i].seconds);
                    } else {
                        selected_ = i;
                    }
                    return true;
                }
            }
            if (waveBox_.Contain(mouse.x, mouse.y)) {
                startScrub(mouse);
                return true;
            }
        }
        if (scrubbing_ && mouse.motion == Mouse::Moved) {
            scrubToMouse(mouse);
            return true;
        }
        if (scrubbing_ && mouse.motion == Mouse::Released) {
            scrubToMouse(mouse);
            scrubbing_ = false;
            return true;
        }
        if (waveBox_.Contain(mouse.x, mouse.y) && mouse.button == Mouse::WheelUp) {
            zoomManual(1.4);
            return true;
        }
        if (waveBox_.Contain(mouse.x, mouse.y) && mouse.button == Mouse::WheelDown) {
            zoomManual(1.0 / 1.4);
            return true;
        }
        return true;
    }

    if (markerKey(event, "close", {"escape", "z", "Z", "я", "Я"})) {
        close();
        return true;
    }
    if (markerKey(event, "stop", {"o", "O", "щ", "Щ"})) {
        controller_.stopPreviewPlayback();
        playhead_ = currentPosition();
        centerViewOnPlayhead();
        return true;
    }
    if (markerKey(event, "left", {"left"})) {
        if (controlRow_ == 1) {
            if (speedSelected_ == 1) {
                setPlaybackRateFromValue(speedValue_ - speedStep_);
            }
            return true;
        }
        closeSelected_ = false;
        controlRow_ = 0;
        selected_ = std::max(0, selected_ - 1);
        return true;
    }
    if (markerKey(event, "right", {"right"})) {
        if (controlRow_ == 1) {
            if (speedSelected_ == 1) {
                setPlaybackRateFromValue(speedValue_ + speedStep_);
            }
            return true;
        }
        closeSelected_ = false;
        controlRow_ = 0;
        selected_ = std::min(7, selected_ + 1);
        return true;
    }
    if (markerKey(event, "up", {"up"})) {
        if (controlRow_ == 2) {
            controlRow_ = 1;
            speedSelected_ = 2;
            closeSelected_ = false;
            return true;
        }
        if (controlRow_ == 1) {
            if (speedSelected_ > 0) {
                speedSelected_--;
            } else {
                controlRow_ = 0;
            }
            closeSelected_ = false;
            return true;
        }
        if (closeSelected_) {
            closeSelected_ = false;
        } else {
            scrollManual(-0.12);
        }
        return true;
    }
    if (markerKey(event, "down", {"down"})) {
        if (controlRow_ == 0) {
            controlRow_ = 1;
            speedSelected_ = 0;
            closeSelected_ = false;
        } else if (controlRow_ == 1) {
            if (speedSelected_ < 2) {
                speedSelected_++;
            } else {
                controlRow_ = 2;
                closeSelected_ = true;
            }
        }
        return true;
    }
    if (markerKey(event, "focus_speed", {"?"})) {
        controlRow_ = 1;
        speedSelected_ = 1;
        closeSelected_ = false;
        return true;
    }
    if (markerKey(event, "speed_reset", {":"})) {
        setPlaybackRateFromValue(100);
        controlRow_ = 1;
        speedSelected_ = 0;
        closeSelected_ = false;
        return true;
    }
    if (markerKey(event, "pitch_lock", {"\""})) {
        setPreservePitch(!preservePitch_);
        controlRow_ = 1;
        speedSelected_ = 2;
        closeSelected_ = false;
        return true;
    }
    if (markerKey(event, "zoom_in", {"+", "="})) {
        zoomManual(1.4);
        return true;
    }
    if (markerKey(event, "zoom_out", {"-", "_"})) {
        zoomManual(1.0 / 1.4);
        return true;
    }
    if (markerKey(event, "seek_back_15", {"<"})) {
        seekRelative(-15.0);
        return true;
    }
    if (markerKey(event, "seek_forward_15", {">"})) {
        seekRelative(15.0);
        return true;
    }
    if (markerKey(event, "seek_back_01", {",", "б"})) {
        seekRelative(-0.1);
        return true;
    }
    if (markerKey(event, "seek_forward_01", {".", "ю"})) {
        seekRelative(0.1);
        return true;
    }
    if (markerKey(event, "height_down", {"[", "х", "Х"})) {
        waveRows_ = std::max(10, waveRows_ - 2);
        return true;
    }
    if (markerKey(event, "height_up", {"]", "ъ", "Ъ"})) {
        waveRows_ = std::min(24, waveRows_ + 2);
        return true;
    }
    if (markerKey(event, "follow", {"t", "T", "е", "Е"})) {
        followPlayhead_ = !followPlayhead_;
        playhead_ = currentPosition();
        centerViewOnPlayhead();
        return true;
    }
    if (markerKey(event, "set", {"enter"})) {
        if (closeSelected_) {
            close();
            return true;
        }
        if (controlRow_ == 1) {
            if (speedSelected_ == 0) {
                setPlaybackRateFromValue(100);
            } else if (speedSelected_ == 2) {
                setPreservePitch(!preservePitch_);
            }
            return true;
        }
        setCue(selected_);
        return true;
    }
    if (markerKey(event, "delete", {"backspace", "delete"})) {
        if (closeSelected_) {
            return true;
        }
        deleteSelectedCue();
        return true;
    }
    if (markerKey(event, "toggle_playback", {"space"})) {
        auto playback = controller_.previewPlaybackSnapshot();
        if (controller_.previewPlayingTrackId() == track_.id &&
            (playback.state == PlaybackState::Playing ||
             playback.state == PlaybackState::Paused)) {
            controller_.togglePreviewPause();
        } else if (controller_.playPreviewTrack(track_)) {
            double track_duration = duration();
            if (track_duration > 0.0) {
                controller_.seekPreviewPlayback(currentPosition() / track_duration);
            }
        }
        return true;
    }

    const std::array<std::array<std::string, 2>, 8> shifted_delete_keys = {{
        { "!", "!" },
        { "@", "@" },
        { "#", "№" },
        { "$" },
        { "%", "%" },
        { "^", "," },
        { "&", "." },
        { "*", ";" },
    }};
    for (int i = 0; i < 8; ++i) {
        if (markerKey(event,
                      "delete_cue_" + std::to_string(i + 1),
                      {shifted_delete_keys[(size_t)i][0],
                       shifted_delete_keys[(size_t)i][1]})) {
            selected_ = i;
            closeSelected_ = false;
            controlRow_ = 0;
            cues_[(size_t)i] = {};
            dirty_ = true;
            return true;
        }
    }
    for (int i = 0; i < 8; ++i) {
        std::string key(1, (char)('1' + i));
        if (markerKey(event, "cue_" + std::to_string(i + 1), {key})) {
            activateCue(i);
            return true;
        }
    }
    return true;
}
