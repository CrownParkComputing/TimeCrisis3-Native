// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include "../rendering/options.h"

union SDL_Event;

namespace tc3::menu {
enum class Action { Pause, Resume, Quit, ToggleFullscreen, StartTwin, ToggleTwinDefault,
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
    bool twin_default = false;
    int sinden_devices = 0;
    // Cabinet service options (TC4 game options). -1 = value unknown.
    bool service_supported = false;
    bool is_tc4 = false;  // NM00032 (TSF100) EEPROM format; false = TC3 (TST-03A)
    int service_state = 0;   // 0 idle, 1 working, 2 done, 3 failed
    char service_note[96] = {};
    int service_values[8] = {};
    bool test_mode = false;  // original cabinet test menu is on screen
    bool loading = false;    // game not producing frames yet (boot/ROM load)
    float load_progress = 0; // 0..1 real boot stage progress
    char load_stage[96] = {};
    int cabinet_role = -1;   // 0 left, 1 right, -1 solo
};

// Thread-safe UI/CPU entry points. The callback MUST enqueue work on its owner
// thread: it can be invoked from either SDL input or the GS rendering thread.
void SetActionCallback(std::function<void(Action)> callback);
void SetOptionCallback(std::function<void(rendering::Option, int)> callback);
// Apply edited cabinet service options through the game's own test menu.
// targets[i] is the desired value index for service option i, or -1 to skip.
void SetServiceApplyCallback(std::function<void(const int* targets, int count)> callback);
void PublishSnapshot(const Snapshot& snapshot);
bool HandleEvent(const SDL_Event& event);
bool IsOpen();
void SetOpen(bool open);

// GS thread only, with the core's ImGui context and an active ImGui frame.
// Render only appends draw-list geometry; it never submits GPU commands.
// Initialize is also called lazily by Render. Shutdown precedes GS teardown.
void Initialize();
void Shutdown();
void Render();
} // namespace tc3::menu
