// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace tc3::playable {
struct Status {
  bool running = false;
  bool paused = false;
  bool fullscreen = false;
  float video_fps = 0;
  float game_fps = 0;
  float speed_percent = 0;
  std::string title;
  std::string error;
};
Status GetStatus();
void RequestPause(bool pause);
void RequestShutdown();
void RequestFullscreen(bool fullscreen);
void RequestSaveState();
void RequestLoadState();
void RequestScreenshot();
} // namespace tc3::playable
