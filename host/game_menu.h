// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include "../rendering/options.h"

union SDL_Event;

namespace tc3::menu {
enum class Action { Pause, Resume, Quit, ToggleFullscreen, StartTwin,
    CabinetTest, CabinetUp, CabinetDown, CabinetLeft, CabinetRight,
    CabinetEnter, CabinetService, CabinetCoin };
struct Snapshot {
    double game_fps = 0;
    double video_fps = 0;
    bool valid = false;
    bool fullscreen = false;
    float input_width = 0;
    float input_height = 0;
    rendering::Options rendering;
    float speed_percent = 0;
    bool twin_mode = false;
    bool twin_supported = true;
    int sinden_devices = 0;
};

// Thread-safe UI/CPU entry points. The callback MUST enqueue work on its owner
// thread: it can be invoked from either SDL input or the GS rendering thread.
void SetActionCallback(std::function<void(Action)> callback);
void SetOptionCallback(std::function<void(rendering::Option, int)> callback);
void PublishSnapshot(const Snapshot& snapshot);
bool HandleEvent(const SDL_Event& event);
bool IsOpen();

// GS thread only, with the core's ImGui context and an active ImGui frame.
// Render only appends draw-list geometry; it never submits GPU commands.
// Initialize is also called lazily by Render. Shutdown precedes GS teardown.
void Initialize();
void Shutdown();
void Render();
} // namespace tc3::menu
