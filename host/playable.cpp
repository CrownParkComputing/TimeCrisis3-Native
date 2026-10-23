// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0-or-later
// Host callbacks adapted from the pinned PCSX2 gsrunner frontend; SDL
// lifecycle, dispatch and normal gameplay are implemented here (not GS dump
// replay).
#include "playable.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/StringUtil.h"
#include "common/WindowInfo.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/Counters.h"
#include "pcsx2/DEV9/ACJV.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#include "../input/sinden.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/PrecompiledHeader.h"
#include "pcsx2/USB/USB.h"
#include "pcsx2/VMManager.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_set>
#ifdef TC3_GAME_MENU
#include "game_menu.h"
#endif
#include "../runtime/cabinet/control.h"
namespace {
using Clock = std::chrono::steady_clock;
SDL_Window *window{};
std::unique_ptr<INISettingsInterface> settings;
std::unique_ptr<tc3::cabinet::SharedControl> cabinetControl;
std::uint32_t appliedCabinetRendering = 0; // CPU thread only.
bool previewFramesRunning = false; // CPU thread only; bounded resolution refresh.
struct CabinetPulse { u16 mask; u32 started; };
std::vector<CabinetPulse> cabinetPulses; // CPU thread only; release by guest fields.
void applyCabinetRendering();
std::mutex cpuMutex, uiMutex, statusMutex, windowMutex;
std::condition_variable cpuWake;
std::deque<std::function<void()>> cpuTasks, uiTasks;
bool cpuQueueOpen = true; // guarded by cpuMutex
std::thread::id uiThread;
thread_local bool isCPU = false;
std::atomic<bool> cpuDone{false}, stopRequested{false}, fullScreen{false};
std::atomic<int> result{1};
std::atomic<uint64_t> bootedAt{0}, presented{0};
WindowInfo windowInfo;
tc3::playable::Status status;
std::atomic<int> logicalWidth{1100}, logicalHeight{770};
bool menuInitialized = false; // GS thread only.
std::atomic<bool> renderingReady{false}, casSupported{false};
std::atomic<bool> twinSupported{false};
uint64_t milliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             Clock::now().time_since_epoch())
      .count();
}
void ui(std::function<void()> fn) {
  if (std::this_thread::get_id() == uiThread) {
    fn();
    return;
  }
  std::lock_guard lock(uiMutex);
  uiTasks.push_back(std::move(fn));
}
void drainUI() {
  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard lock(uiMutex);
    tasks.swap(uiTasks);
  }
  for (auto &fn : tasks)
    fn();
}
void drainCPU() {
  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard lock(cpuMutex);
    tasks.swap(cpuTasks);
  }
  for (auto &fn : tasks)
    fn();
}
void closeCPUQueue() {
  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard lock(cpuMutex);
    cpuQueueOpen = false;
    tasks.swap(cpuTasks);
  }
  for (auto &fn : tasks)
    fn(); // Finish accepted work before destroying the VM.
}
std::optional<WindowInfo> getWindow() {
  std::lock_guard lock(windowMutex);
  return windowInfo;
}
void updateWindow() {
  WindowInfo next;
  const SDL_PropertiesID props = SDL_GetWindowProperties(window);
  const char *driver = SDL_GetCurrentVideoDriver();
  if (driver && std::strcmp(driver, "x11") == 0) {
    next.type = WindowInfo::Type::X11;
    next.display_connection = SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    next.window_handle = reinterpret_cast<void *>(uintptr_t(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
  } else if (driver && std::strcmp(driver, "wayland") == 0) {
    next.type = WindowInfo::Type::Wayland;
    next.display_connection = SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    next.window_handle = SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  } else
    throw std::runtime_error(
        "This frontend currently requires SDL X11 or Wayland");
  int w, h;
  SDL_GetWindowSizeInPixels(window, &w, &h);
  next.surface_width = std::max(1, w);
  next.surface_height = std::max(1, h);
  SDL_GetWindowSize(window, &w, &h);
  logicalWidth = std::max(1, w);
  logicalHeight = std::max(1, h);
  next.surface_scale = SDL_GetWindowDisplayScale(window);
  if (const auto *mode =
          SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
    next.surface_refresh_rate = mode->refresh_rate;
  {
    std::lock_guard lock(windowMutex);
    windowInfo = next;
  }
}
void report(std::string_view title, std::string_view text, bool error) {
  std::fprintf(error ? stderr : stdout, "[TC3] %.*s: %.*s\n", int(title.size()),
               title.data(), int(text.size()), text.data());
  if (error) {
    std::lock_guard lock(statusMutex);
    status.error = std::string(text);
  }
}
void stop() {
  if (cabinetControl)
    cabinetControl->request_quit();
  stopRequested = true;
  Host::RunOnCPUThread([] {
    if (VMManager::HasValidVM())
      VMManager::SetState(VMState::Stopping);
  });
}
void refreshPausedScene() {
  if (VMManager::GetState() != VMState::Paused || !renderingReady) return;
  previewFramesRunning = true;
  // A full new image at the requested internal resolution needs guest draws.
  // Keep the core's scheduler, audio clocks and normal auto-pause semantics.
  VMManager::FrameAdvance(2);
  std::printf("[TC3 preview] Resolution refresh: two video fields from %u\n", g_FrameCount);
  std::fflush(stdout);
}
void applyCabinetControl() {
  for (auto it=cabinetPulses.begin();it!=cabinetPulses.end();) {
    if (u32(g_FrameCount-it->started)>=6) {
      ACJV::SetButtonState(0,it->mask,false);
      it=cabinetPulses.erase(it);
    } else ++it;
  }
  if (!cabinetControl)
    return;
  if (cabinetControl->quitting()) {
    stopRequested = true;
    if (VMManager::HasValidVM())
      VMManager::SetState(VMState::Stopping);
    return;
  }
  const auto state = VMManager::GetState();
  if (previewFramesRunning && state != VMState::Running)
    previewFramesRunning = false;
  if (!previewFramesRunning && (state == VMState::Running || state == VMState::Paused) &&
      (state == VMState::Paused) != cabinetControl->paused())
    VMManager::SetPaused(cabinetControl->paused());
  applyCabinetRendering();
}
void telemetry() {
  static uint64_t last = 0;
  const auto now = milliseconds();
  if (now - last < 1000)
    return;
  last = now;
  std::lock_guard lock(statusMutex);
  std::printf("[TC3 telemetry] guest_fields=%u present_calls=%llu "
              "video_fps=%.2f game_fps=%.2f "
              "speed=%.2f state=%d\n",
              g_FrameCount, static_cast<unsigned long long>(presented.load()),
              status.video_fps, status.game_fps, status.speed_percent,
              int(VMManager::GetState()));
  std::fflush(stdout);
}
} // namespace

void Host::CommitBaseSettingChanges() {
  ui([] {
    auto lock = Host::GetSettingsLock();
    if (settings && !settings->Save())
      report("Settings", "Could not save settings", true);
  });
}

void Host::LoadSettings(SettingsInterface &si,
                        std::unique_lock<std::mutex> &lock) {}

void Host::CheckForSettingsChanges(const Pcsx2Config &old_config) {}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers,
                                bool hotkeys, bool ui) {
  // not running any UI, so no settings requests will come in
  return false;
}

void Host::SetDefaultUISettings(SettingsInterface &si) {
  // nothing
}

bool Host::LocaleCircleConfirm() {
  // not running any UI, so no settings requests will come in
  return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback() {
  return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title,
                           const std::string_view message) {
  report(title, message, false);
}

void Host::ReportErrorAsync(const std::string_view title,
                            const std::string_view message) {
  report(title, message, true);
}

void Host::OpenURL(const std::string_view url) {
  ui([url = std::string(url)] { SDL_OpenURL(url.c_str()); });
}

bool Host::CopyTextToClipboard(const std::string_view text) {
  ui([text = std::string(text)] { SDL_SetClipboardText(text.c_str()); });
  return true;
}

void Host::BeginTextInput() {
  ui([] { SDL_StartTextInput(window); });
}

void Host::EndTextInput() {
  ui([] { SDL_StopTextInput(window); });
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo() { return getWindow(); }

void Host::OnInputDeviceConnected(const std::string_view identifier,
                                  const std::string_view device_name) {}

void Host::OnInputDeviceDisconnected(const InputBindingKey key,
                                     const std::string_view identifier) {}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor) {
  ui([relative_mode, hide_cursor] {
    SDL_SetWindowRelativeMouseMode(window, relative_mode);
    if (hide_cursor)
      SDL_HideCursor();
    else
      SDL_ShowCursor();
  });
}

void Host::SetMouseLock(bool state) {
  ui([state] { SDL_SetWindowMouseGrab(window, state); });
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window) {
  return getWindow();
}

void Host::ReleaseRenderWindow() {
  renderingReady = false;
  casSupported = false;
#ifdef TC3_GAME_MENU
  if (menuInitialized) {
    tc3::menu::Shutdown();
    menuInitialized = false;
  }
#endif
  // SDL window is retained until the CPU and GS threads have shut down.
}

void Host::BeginPresentFrame() {
  ++presented;
#ifdef TC3_GAME_MENU
  if (!menuInitialized) {
    tc3::menu::Initialize();
    // Nuklear consumes input on the GS thread. Keep presenting while the VM
    // is paused, as the upstream fullscreen UI does, or its menu freezes after
    // the first frame. MTGS throttles these presents without advancing the VM.
    MTGS::SetRunIdle(true);
    menuInitialized = true;
  }
  const auto snapshot = tc3::playable::GetStatus();
  tc3::rendering::Options options;
  options.ready = g_gs_device != nullptr;
  options.cas_supported = options.ready && g_gs_device->Features().cas_sharpening;
  options.resolution = std::lround(GSConfig.UpscaleMultiplier);
  options.fxaa = GSConfig.FXAA;
  options.cas = static_cast<int>(GSConfig.CASMode);
  options.sharpness = GSConfig.CAS_Sharpness;
  options.vsync = GSConfig.VsyncEnable;
  options.queue_depth = GSConfig.VsyncQueueSize;
  options.sinden_border = tc3::sinden::mode.load();
  options.border_width = tc3::sinden::width_percent.load();
  renderingReady = options.ready;
  casSupported = options.cas_supported;
  tc3::menu::PublishSnapshot({snapshot.game_fps, snapshot.video_fps,
                              snapshot.running, snapshot.fullscreen,
                              float(logicalWidth.load()),
                              float(logicalHeight.load()), options,
                              snapshot.speed_percent, cabinetControl != nullptr,
                              twinSupported.load(), tc3::sinden::detected.load()});
  // Record applied GS values, rather than just the menu's requested values.
  static tc3::rendering::Options previous;
  if (!previous.ready || previous.resolution != options.resolution ||
      previous.fxaa != options.fxaa || previous.cas != options.cas ||
      previous.sharpness != options.sharpness || previous.vsync != options.vsync ||
      previous.queue_depth != options.queue_depth) {
    std::printf("[TC3 rendering applied] scale=%d fxaa=%d cas=%d sharpness=%d "
                "vsync=%d queue=%d cas_supported=%d\n", options.resolution,
                options.fxaa, options.cas, options.sharpness, options.vsync,
                options.queue_depth, options.cas_supported);
    std::fflush(stdout);
    previous = options;
  }
  tc3::menu::Render();
#endif
}

void Host::RequestResizeHostDisplay(s32 width, s32 height) {
  ui([width, height] {
    SDL_SetWindowSize(window, width, height);
    updateWindow();
  });
}

void Host::OnVMStarting() {
  report("VM", "Starting PCSX2-derived execution baseline", false);
}

void Host::OnVMStarted() {
  bootedAt = milliseconds();
  {
    std::lock_guard lock(statusMutex);
    status.running = true;
  }
  report("VM", "Boot succeeded", false);
}

void Host::OnVMDestroyed() {
  {
    std::lock_guard lock(statusMutex);
    status.running = false;
  }
  report("VM", "Stopped", false);
}

void Host::OnVMPaused() {
  std::lock_guard lock(statusMutex);
  status.paused = true;
}

void Host::OnVMResumed() {
  std::lock_guard lock(statusMutex);
  status.paused = false;
}

void Host::OnGameChanged(const std::string &title,
                         const std::string &elf_override,
                         const std::string &disc_path,
                         const std::string &disc_serial, u32 disc_crc,
                         u32 current_crc) {
  {
    std::lock_guard lock(statusMutex);
    status.title = title;
  }
  report("Game", title, false);
}

void Host::OnPerformanceMetricsUpdated() {
  std::lock_guard lock(statusMutex);
  status.video_fps = PerformanceMetrics::GetFPS();
  status.game_fps = PerformanceMetrics::GetInternalFPS();
  status.speed_percent = PerformanceMetrics::GetSpeed();
}

void Host::OnSaveStateLoading(const std::string_view filename) {}

void Host::OnSaveStateLoaded(const std::string_view filename,
                             bool was_successful) {}

void Host::OnSaveStateSaved(const std::string_view filename) {}

void Host::RunOnCPUThread(std::function<void()> function, bool block) {
  if (block && isCPU) {
    function();
    return;
  }
  std::shared_ptr<std::promise<void>> promise;
  std::future<void> completion;
  if (block) {
    promise = std::make_shared<std::promise<void>>();
    completion = promise->get_future();
  }
  {
    std::lock_guard lock(cpuMutex);
    if (!cpuQueueOpen)
      return;
    if (promise)
      cpuTasks.push_back([function = std::move(function), promise] {
        try {
          function();
          promise->set_value();
        } catch (...) {
          promise->set_exception(std::current_exception());
        }
      });
    else
      cpuTasks.push_back(std::move(function));
  }
  cpuWake.notify_one();
  if (block)
    completion.get();
}

void Host::RefreshGameListAsync(bool invalidate_cache) {}

void Host::CancelGameListRefresh() {}

bool Host::IsFullscreen() { return fullScreen; }

void Host::SetFullscreen(bool enabled) {
  ui([enabled] {
    if (SDL_SetWindowFullscreen(window, enabled)) {
      fullScreen = enabled;
      updateWindow();
    } else
      report("Fullscreen", SDL_GetError(), true);
  });
}

void Host::OnCaptureStarted(const std::string &filename) {}

void Host::OnCaptureStopped() {}

void Host::RequestExitApplication(bool allow_confirm) { stop(); }

void Host::RequestExitBigPicture() { stop(); }

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state,
                             bool default_save_state) {
  stop();
}

void Host::OnAchievementsLoginSuccess(const char *username, u32 points,
                                      u32 sc_points, u32 unread_messages) {
  // noop
}

void Host::OnAchievementsLoginRequested(
    Achievements::LoginRequestReason reason) {
  // noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled) {
  // noop
}

void Host::OnAchievementsRefreshed() {
  // noop
}

void Host::OnCoverDownloaderOpenRequested() {
  // noop
}

void Host::OnCreateMemoryCardOpenRequested() {
  // noop
}

bool Host::InBatchMode() { return false; }

bool Host::InNoGUIMode() { return false; }

bool Host::ShouldPreferHostFileSelector() { return false; }

void Host::OpenHostFileSelectorAsync(std::string_view title,
                                     bool select_directory,
                                     FileSelectorCallback callback,
                                     FileSelectorFilters filters,
                                     std::string_view initial_directory) {
  callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs) {
  const int res =
      std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
  if (res != 0)
    return res;
  return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32>
InputManager::ConvertHostKeyboardStringToCode(const std::string_view str) {
  const SDL_Keycode key = SDL_GetKeyFromName(std::string(str).c_str());
  return key == SDLK_UNKNOWN ? std::nullopt : std::optional<u32>(key);
}

std::optional<std::string>
InputManager::ConvertHostKeyboardCodeToString(u32 code) {
  const char *name = SDL_GetKeyName(code);
  return name && *name ? std::optional<std::string>(name) : std::nullopt;
}

const char *InputManager::ConvertHostKeyboardCodeToIcon(u32 code) {
  return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view context,
                                            const std::string_view msg,
                                            char *tbuf, size_t tbuf_space) {
  if (msg.size() > tbuf_space)
    return -1;
  else if (msg.empty())
    return 0;

  std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char *context, const char *msg,
                                          const char *disambiguation,
                                          int count) {
  TinyString count_str = TinyString::from_format("{}", count);

  std::string ret(msg);
  for (;;) {
    std::string::size_type pos = ret.find("%n");
    if (pos == std::string::npos)
      break;

    ret.replace(pos, 2, count_str.view());
  }

  return ret;
}

void Host::PumpMessagesOnCPUThread() {
  drainCPU();
  applyCabinetControl();
  telemetry();
}
void Host::RunOnGSThread(std::function<void()> fn) {
  RunOnCPUThread([fn = std::move(fn)] { MTGS::RunOnGSThread(fn); });
}
namespace tc3::playable {
Status GetStatus() {
  std::lock_guard lock(statusMutex);
  auto copy = status;
  copy.fullscreen = fullScreen;
  return copy;
}
void RequestPause(bool pause) {
  if (cabinetControl)
    cabinetControl->set_paused(pause);
  Host::RunOnCPUThread([pause] {
    if (VMManager::HasValidVM())
      VMManager::SetPaused(cabinetControl ? cabinetControl->paused() : pause);
  });
}
void RequestShutdown() { stop(); }
void RequestFullscreen(bool fullscreen) { Host::SetFullscreen(fullscreen); }
void RequestSaveState() {
  if (cabinetControl) {
    report("Save state", "Paired snapshots are not implemented", false);
    return;
  }
  Host::RunOnCPUThread([] {
    if (VMManager::HasValidVM())
      VMManager::SaveStateToSlot(1, true, [](const std::string &error) {
        report("Save state", error, true);
      });
  });
}
void RequestLoadState() {
  if (cabinetControl) {
    report("Load state", "Paired snapshots are not implemented", false);
    return;
  }
  Host::RunOnCPUThread([] {
    if (VMManager::HasValidVM()) {
      Error error;
      if (!VMManager::LoadStateFromSlot(1, false, &error))
        report("Load state", error.GetDescription(), true);
    }
  });
}
void RequestScreenshot() {
  Host::RunOnCPUThread([] {
    if (MTGS::IsOpen())
      MTGS::RunOnGSThread([] { GSQueueSnapshot(""); });
  });
}
void RequestTwinScreens() {
  if (!twinSupported) return;
  ui([] {
    const auto launcher = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "tools/run_cabinet_pair.py";
    const auto path = launcher.string();
    const char* arguments[] = {"python3", path.c_str(), "--background", nullptr};
    SDL_Process* process = SDL_CreateProcess(arguments, false);
    if (!process) {
      report("Twin screens", SDL_GetError(), true);
      return;
    }
    int exit_code = -1;
    SDL_WaitProcess(process, true, &exit_code);
    SDL_DestroyProcess(process);
    if (exit_code != 0) {
      report("Twin screens", "Cabinet supervisor could not start; see its log", true);
      return;
    }
    stop();
  });
}
#ifdef TC3_GAME_MENU
void RequestCabinetAction(tc3::menu::Action action) {
  Host::RunOnCPUThread([action] {
    if (!VMManager::HasValidVM()) return;
    using A=tc3::menu::Action;
    if (action==A::CabinetTest) ACJV::ToggleDIPSwitchState(0);
    else if (action==A::CabinetCoin) ACJV::InsertCoin(0);
    else {
      u16 mask=0;
      switch(action) {
      case A::CabinetUp: mask=JVS_BTN_UP;break;
      case A::CabinetDown: mask=JVS_BTN_DOWN;break;
      case A::CabinetLeft: mask=JVS_BTN_LEFT;break;
      case A::CabinetRight: mask=JVS_BTN_RIGHT;break;
      case A::CabinetEnter: mask=JVS_BTN_1;break;
      case A::CabinetService: mask=JVS_BTN_SERVICE;break;
      default: return;
      }
      ACJV::SetButtonState(0,mask,true);
      cabinetPulses.push_back({mask,g_FrameCount});
    }
    std::printf("[Cabinet maintenance] action=%d field=%u test=%d\n",int(action),g_FrameCount,ACJV::GetDIPSwitchState(0));
    std::fflush(stdout);
  });
}
void RequestRenderingOption(tc3::rendering::Option option, int direction) {
  Host::RunOnCPUThread([option, direction] {
    if (!VMManager::HasValidVM() || !renderingReady)
      return;
    tc3::rendering::Options value;
    bool resolution_changed = false;
    {
      auto lock = Host::GetSettingsLock();
      tc3::rendering::Options current;
      current.ready = true;
      current.cas_supported = casSupported;
      current.resolution = std::lround(settings->GetFloatValue("EmuCore/GS", "upscale_multiplier", 1));
      current.fxaa = settings->GetBoolValue("EmuCore/GS", "fxaa", false);
      current.cas = settings->GetIntValue("EmuCore/GS", "CASMode", 0);
      current.sharpness = settings->GetIntValue("EmuCore/GS", "CASSharpness", 50);
      current.vsync = settings->GetBoolValue("EmuCore/GS", "VsyncEnable", false);
      current.queue_depth = settings->GetIntValue("EmuCore/GS", "VsyncQueueSize", 2);
      current.sinden_border = tc3::sinden::mode.load();
      current.border_width = tc3::sinden::width_percent.load();
      value = tc3::rendering::adjust(current, option, direction);
      resolution_changed = value.resolution != current.resolution;
      switch (option) {
      case tc3::rendering::Option::SindenBorder:
        settings->SetIntValue("Lightgun", "SindenBorder", value.sinden_border);
        tc3::sinden::mode = value.sinden_border; break;
      case tc3::rendering::Option::BorderWidth:
        settings->SetIntValue("Lightgun", "BorderWidth", value.border_width);
        tc3::sinden::width_percent = value.border_width; break;
      case tc3::rendering::Option::Resolution:
        settings->SetFloatValue("EmuCore/GS", "upscale_multiplier", value.resolution); break;
      case tc3::rendering::Option::FXAA:
        settings->SetBoolValue("EmuCore/GS", "fxaa", value.fxaa); break;
      case tc3::rendering::Option::CAS:
        settings->SetIntValue("EmuCore/GS", "CASMode", value.cas); break;
      case tc3::rendering::Option::Sharpness:
        settings->SetIntValue("EmuCore/GS", "CASSharpness", value.sharpness); break;
      case tc3::rendering::Option::VSync:
        settings->SetBoolValue("EmuCore/GS", "VsyncEnable", value.vsync); break;
      case tc3::rendering::Option::QueueDepth:
        settings->SetIntValue("EmuCore/GS", "VsyncQueueSize", value.queue_depth); break;
      }
    }
    // ApplySettings takes the settings lock itself and synchronizes GS updates.
    VMManager::ApplySettings();
    if (cabinetControl) {
      appliedCabinetRendering = tc3::rendering::pack(value);
      cabinetControl->set_rendering_bits(appliedCabinetRendering);
    }
    Host::CommitBaseSettingChanges();
    if (resolution_changed) refreshPausedScene();
  });
}
#endif
} // namespace tc3::playable
namespace {
void applyCabinetRendering() {
#ifdef TC3_GAME_MENU
  if (!renderingReady || !VMManager::HasValidVM()) return;
  const auto bits = cabinetControl->rendering_bits();
  if (!bits || bits == appliedCabinetRendering) return;
  appliedCabinetRendering = bits;
  const auto value = tc3::rendering::unpack(bits);
  if (!value) {
    report("Rendering", "Rejected invalid peer rendering settings", true);
    return;
  }
  const bool resolution_changed = std::lround(EmuConfig.GS.UpscaleMultiplier) != value->resolution;
  {
    auto lock = Host::GetSettingsLock();
    settings->SetFloatValue("EmuCore/GS", "upscale_multiplier", value->resolution);
    settings->SetBoolValue("EmuCore/GS", "fxaa", value->fxaa);
    settings->SetIntValue("EmuCore/GS", "CASMode", casSupported ? value->cas : 0);
    settings->SetIntValue("EmuCore/GS", "CASSharpness", value->sharpness);
    settings->SetBoolValue("EmuCore/GS", "VsyncEnable", value->vsync);
    settings->SetIntValue("EmuCore/GS", "VsyncQueueSize", value->queue_depth);
    settings->SetIntValue("Lightgun", "SindenBorder", value->sinden_border);
    settings->SetIntValue("Lightgun", "BorderWidth", value->border_width);
    tc3::sinden::mode = value->sinden_border;
    tc3::sinden::width_percent = value->border_width;
  }
  VMManager::ApplySettings();
  Host::CommitBaseSettingChanges();
  if (resolution_changed) refreshPausedScene();
#endif
}
void initializeSettings(const std::string &root) {
  EmuFolders::SetAppRoot();
  EmuConfig.CustomDataPath = std::filesystem::absolute(root).string();
  Error error;
  if (!EmuFolders::SetResourcesDirectory() ||
      !EmuFolders::SetDataDirectory(&error))
    throw std::runtime_error("Core folders: " + error.GetDescription());
  CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);
  settings = std::make_unique<INISettingsInterface>(
      Path::Combine(EmuFolders::Settings, "PCSX2.ini"));
  if (!settings->Load())
    throw std::runtime_error("Cannot load private settings: " +
                             settings->GetFileName());
  // SDL's upstream controller source polls the global SDL event queue from the
  // CPU thread. This frontend owns that queue on the UI thread; keep controller
  // and raw-device sources off until events are explicitly routed to them.
  settings->SetBoolValue("InputSources", "SDL", false);
  settings->SetBoolValue("InputSources", "Evdev", false);
  settings->SetBoolValue("JVS", "SindenBorderEnabled", false);
  report("Input",
         "SDL UI owns keyboard/mouse; core SDL and Evdev sources disabled",
         false);
  // This game owns its presentation. Keep core diagnostics in the log only.
  for (const char* key : {"EnablePatches", "EnableCheats", "EnableWideScreenPatches",
                          "EnableNoInterlacingPatches"})
    settings->SetBoolValue("EmuCore", key, false);
  for (const char* key : {"OsdShowSpeed", "OsdShowFPS", "OsdShowVPS",
       "OsdShowResolution", "OsdShowGSStats", "OsdShowCPU", "OsdShowGPU",
       "OsdShowGPUDebug", "OsdShowIndicators", "OsdShowFrameTimes",
       "OsdShowHardwareInfo", "OsdShowVersion", "OsdShowSettings",
       "OsdShowInputs", "OsdShowVideoCapture", "OsdShowInputRec",
       "OsdShowTextureReplacements"})
    settings->SetBoolValue("EmuCore/GS", key, false);
  settings->SetIntValue("EmuCore/GS", "OsdMessagesPos", 0);
  settings->SetIntValue("EmuCore/GS", "OsdPerformancePos", 0);
  if (const char* role = std::getenv("TC3_CABINET_ROLE")) {
    const std::string_view side(role);
    if (side != "left" && side != "right")
      throw std::runtime_error("TC3_CABINET_ROLE must be left or right");
    settings->SetBoolValue("JVS", "LightgunLinkAs2P", side == "right");
  }
  Host::Internal::SetBaseSettingsLayer(settings.get());
  VMManager::Internal::LoadStartupSettings();
  auto fontData = FileSystem::MapBinaryFileForRead(
      EmuFolders::GetOverridableResourcePath("fonts/Roboto-Regular.ttf")
          .c_str());
  if (fontData.empty())
    throw std::runtime_error("Required core font resources missing");
  std::vector<ImGuiManager::FontInfo> fonts;
  ImGuiManager::FontInfo font{};
  font.data = fontData;
  fonts.push_back(font);
  ImGuiManager::SetFonts(std::move(fonts));
}
void cpuMain(VMBootParameters params) {
  isCPU = true;
  bool cpuInitialized = false, vmInitialized = false;
  try {
    if (!VMManager::Internal::CPUThreadInitialize())
      throw std::runtime_error("CPU thread initialization failed");
    cpuInitialized = true;
    VMManager::ApplySettings();
    Error error;
    if (VMManager::Initialize(params, &error) != VMBootResult::StartupSuccess)
      throw std::runtime_error("VM boot failed: " + error.GetDescription());
    vmInitialized = true;
    if (!stopRequested)
      VMManager::SetState(VMState::Running);
    while (!stopRequested) {
      drainCPU();
      applyCabinetControl();
      switch (VMManager::GetState()) {
      case VMState::Running:
        VMManager::Execute();
        break;
      case VMState::Resetting:
        VMManager::Reset();
        break;
      case VMState::Paused: {
        VMManager::IdlePollUpdate();
        std::unique_lock lock(cpuMutex);
        cpuWake.wait_for(lock, std::chrono::milliseconds(8),
                         [] { return !cpuTasks.empty() || stopRequested; });
        break;
      }
      case VMState::Stopping:
      case VMState::Shutdown:
        stopRequested = true;
        break;
      default:
        throw std::runtime_error("Unexpected VM state after boot");
      }
      telemetry();
    }
    closeCPUQueue();
    if (vmInitialized) {
      VMManager::Shutdown(false);
      vmInitialized = false;
    }
    result = 0;
  } catch (const std::exception &error) {
    report("Fatal", error.what(), true);
    result = 1;
    closeCPUQueue();
    if (vmInitialized) {
      VMManager::Shutdown(false);
      vmInitialized = false;
    }
  }
  if (cpuInitialized)
    VMManager::Internal::CPUThreadShutdown();
  cpuDone = true;
  cpuWake.notify_all();
}
bool inputTraceEnabled() {
  static const bool enabled = std::getenv("TC3_INPUT_TRACE") != nullptr;
  return enabled;
}
void invokeTracedInput(InputBindingKey key, float value, const char *kind,
                       unsigned code) {
  const bool bound = InputManager::InvokeEvents(key, value);
  if (!inputTraceEnabled())
    return;
  const auto &mapping = ACJV::GetGunMapping();
  std::fprintf(stderr,
               "[TC3 input] %s code=%u value=%.0f bound=%d acjv=%d mode=%d "
               "pedal_mask=%04x trigger_mask=%04x",
               kind, code, value, bound, ACJV::enabled,
               static_cast<int>(ACJV::GetMode()), mapping.pedal,
               mapping.p1_trigger);
  for (const auto &binding : USB::GetDeviceBindings(0)) {
    if (std::strcmp(binding.name, "A") == 0 ||
        std::strcmp(binding.name, "Trigger") == 0)
      std::fprintf(stderr, " usb1_%s=%.0f", binding.name,
                   USB::GetDeviceBindValue(0, binding.bind_index));
  }
  std::fprintf(stderr, "\n");
}
void dispatchInput(const SDL_Event &e) {
  if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
    if (e.key.repeat)
      return;
    const auto key = InputManager::MakeHostKeyboardKey(e.key.key);
    const float value = e.key.down ? 1.f : 0.f;
    Host::RunOnCPUThread([key, value, code = unsigned(e.key.key)] {
      invokeTracedInput(key, value, "keyboard", code);
    });
  } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
    const auto key = InputManager::MakePointerButtonKey(0, e.button.button - 1);
    const float value = e.button.down ? 1.f : 0.f;
    Host::RunOnCPUThread([key, value, code = unsigned(e.button.button)] {
      invokeTracedInput(key, value, "pointer", code);
    });
  } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
    auto info = getWindow();
    const float x = e.motion.x * info->surface_width / logicalWidth.load(),
                y = e.motion.y * info->surface_height / logicalHeight.load();
    Host::RunOnCPUThread(
        [x, y] { InputManager::UpdatePointerAbsolutePosition(0, x, y); });
  }
}
} // namespace
int main(int argc, char **argv) {
  std::thread cpu;
  try {
    std::string dataRoot, manifest, state;
    double seconds = 0;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--help") {
        std::puts(
            "tc3-native --data-root DIR --boot-manifest FILE [--statefile "
            "FILE] [--seconds N]\nIn-process PCSX2-derived execution baseline, "
            "SDL3/Vulkan/Nuklear frontend.\nControls: mouse aim/left trigger; "
            "Space pedal; 5 coin; Return start; F10 menu; F9 cabinet test; F1 "
            "save; F3 load; F8 screenshot.");
        return 0;
      }
      if (i + 1 >= argc)
        throw std::runtime_error("Missing argument value: " + arg);
      std::string value = argv[++i];
      if (arg == "--data-root")
        dataRoot = value;
      else if (arg == "--boot-manifest")
        manifest = value;
      else if (arg == "--statefile")
        state = value;
      else if (arg == "--seconds") {
        size_t end;
        seconds = std::stod(value, &end);
        if (end != value.size() || !std::isfinite(seconds) || seconds <= 0 ||
            seconds > 86400)
          throw std::runtime_error("Seconds must be 1..86400");
      } else
        throw std::runtime_error("Unknown argument: " + arg);
    }
    if (dataRoot.empty() || manifest.empty())
      throw std::runtime_error("--data-root and --boot-manifest are required");
    CrashHandler::Install();
    Log::SetConsoleOutputLevel(LOGLEVEL_INFO);
    const char *hardwareError = nullptr;
    if (!VMManager::PerformEarlyHardwareChecks(&hardwareError))
      throw std::runtime_error(hardwareError ? hardwareError
                                             : "Unsupported CPU");
    initializeSettings(dataRoot);
    INISettingsInterface gameManifest(std::filesystem::absolute(manifest).string());
    if (!gameManifest.Load()) throw std::runtime_error("Cannot read arcade game manifest");
    twinSupported = gameManifest.GetStringValue("game", "gameid", "") == "NM00012";
    tc3::sinden::mode = std::clamp(settings->GetIntValue("Lightgun", "SindenBorder", 1),0,2);
    tc3::sinden::width_percent = std::clamp(settings->GetIntValue("Lightgun", "BorderWidth", 2),1,5);
    if (const char* role = std::getenv("TC3_CABINET_ROLE")) {
      const char* directory = std::getenv("TC3_CABINET_DIR");
      if (!directory)
        throw std::runtime_error("The cabinet role requires a supervisor session directory");
      cabinetControl = std::make_unique<tc3::cabinet::SharedControl>(
          std::filesystem::path(directory) / "control.bin", std::string_view(role) == "right" ? 2U : 1U);
    }
    if (!SDL_Init(SDL_INIT_VIDEO))
      throw std::runtime_error(SDL_GetError());
    uiThread = std::this_thread::get_id();
    const auto gameName = gameManifest.GetStringValue("game", "name", "Time Crisis");
    window = SDL_CreateWindow(gameName.c_str(),
                              1100, 770,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
      throw std::runtime_error(SDL_GetError());
    if (const char* role = std::getenv("TC3_CABINET_ROLE"))
      SDL_SetWindowTitle(window, std::string("Time Crisis 3 — ").append(role).c_str());
    updateWindow();
    VMBootParameters params;
    params.filename = std::filesystem::absolute(manifest).string();
    if (!state.empty())
      params.save_state = std::filesystem::absolute(state).string();
    std::printf(
        "[TC3] In-process PCSX2-derived baseline; manifest=%s; settings=%s\n",
        params.filename.c_str(), settings->GetFileName().c_str());
#ifdef TC3_GAME_MENU
    tc3::menu::SetActionCallback([](tc3::menu::Action action) {
      switch (action) {
      case tc3::menu::Action::Pause:
        tc3::playable::RequestPause(true);
        break;
      case tc3::menu::Action::Resume:
        tc3::playable::RequestPause(false);
        break;
      case tc3::menu::Action::Quit:
        tc3::playable::RequestShutdown();
        break;
      case tc3::menu::Action::ToggleFullscreen:
        tc3::playable::RequestFullscreen(!fullScreen.load());
        break;
      case tc3::menu::Action::StartTwin:
        tc3::playable::RequestTwinScreens();
        break;
      default:
        tc3::playable::RequestCabinetAction(action);
        break;
      }
    });
    tc3::menu::SetOptionCallback(tc3::playable::RequestRenderingOption);
#endif
    cpu = std::thread(cpuMain, std::move(params));
    std::unordered_set<SDL_Keycode> heldKeys;
    std::unordered_set<uint8_t> heldButtons;
    const auto releaseInputs = [&] {
      for (auto key : heldKeys) {
        SDL_Event e{};
        e.type = SDL_EVENT_KEY_UP;
        e.key.key = key;
        dispatchInput(e);
      }
      for (auto button : heldButtons) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.button = button;
        dispatchInput(e);
      }
      heldKeys.clear();
      heldButtons.clear();
    };
    const uint64_t launch = milliseconds();
    uint64_t nextGunScan = 0;
    bool warned = false;
    while (!cpuDone) {
      drainUI();
      if (milliseconds() >= nextGunScan) {
        tc3::sinden::detected = tc3::sinden::scan();
        nextGunScan = milliseconds() + 1000;
      }
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT ||
            e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
          releaseInputs();
          stop();
          continue;
        }
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
          releaseInputs();
          continue;
        }
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
            e.type == SDL_EVENT_WINDOW_RESIZED ||
            e.type == SDL_EVENT_WINDOW_RESTORED) {
          updateWindow();
          auto wi = *getWindow();
          Host::RunOnCPUThread([wi] {
            if (MTGS::IsOpen())
              MTGS::ResizeDisplayWindow(wi.surface_width, wi.surface_height,
                                        wi.surface_scale);
          });
        }
        bool consumed = false;
#ifdef TC3_GAME_MENU
        consumed = tc3::menu::HandleEvent(e);
#endif
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
          if (e.key.key == SDLK_F1) {
            tc3::playable::RequestSaveState();
            continue;
          }
          if (e.key.key == SDLK_F3) {
            tc3::playable::RequestLoadState();
            continue;
          }
          if (e.key.key == SDLK_F8) {
            tc3::playable::RequestScreenshot();
            continue;
          }
          if (e.key.key == SDLK_PAUSE) {
            tc3::playable::RequestPause(!tc3::playable::GetStatus().paused);
            continue;
          }
#ifndef TC3_GAME_MENU
          if (e.key.key == SDLK_ESCAPE) {
            stop();
            continue;
          }
#endif
        }
        if (consumed) {
          releaseInputs();
          continue;
        }
        if (e.type == SDL_EVENT_KEY_DOWN)
          heldKeys.insert(e.key.key);
        else if (e.type == SDL_EVENT_KEY_UP)
          heldKeys.erase(e.key.key);
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
          heldButtons.insert(e.button.button);
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP)
          heldButtons.erase(e.button.button);
        dispatchInput(e);
      }
      const auto now = milliseconds(), boot = bootedAt.load();
      if (seconds && boot && now - boot >= uint64_t(seconds * 1000))
        stop();
      if (!boot && now - launch > 120000 && !warned) {
        warned = true;
        report("Boot watchdog",
               "No successful VM boot after120seconds; requesting shutdown",
               true);
        stop();
      }
      SDL_Delay(2);
    }
    drainUI();
    cpu.join();
    Host::CommitBaseSettingChanges();
    SDL_DestroyWindow(window);
    window = nullptr;
    SDL_Quit();
    return result;
  } catch (const std::exception &error) {
    report("Frontend", error.what(), true);
    if (cpu.joinable()) {
      stop();
      while (!cpuDone) {
        drainUI();
        SDL_Delay(2);
      }
      cpu.join();
    }
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
}
