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
#include "pcsx2/DEV9/ACSRAM.h"
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
#include "pcsx2/IopMem.h"
#include "pcsx2/Memory.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/PrecompiledHeader.h"
#include "pcsx2/SPU2/spu2.h"
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
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_set>
#ifdef TC3_GAME_MENU
#include "game_menu.h"
#include "service_options.h"
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
std::atomic<int> appliedDisplaySize{0}, appliedDLSS{0}; // CPU thread writes; GS thread reads.
bool detectDlssCapable() {
  // NVIDIA vendor ID on any PCI device means DLSS-capable hardware is present;
  // the actual upscaler still needs its backend (checked by the menu gate).
  static const bool detected = [] {
    for (const auto& entry : std::filesystem::directory_iterator("/sys/bus/pci/devices")) {
      std::ifstream vendor(entry.path() / "vendor");
      std::string id;
      if (vendor >> id && id == "0x10de") return true;
    }
    return false;
  }();
  return detected;
}
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
std::atomic<bool> twinSupported{false}, twinDefault{false};
std::atomic<bool> serviceSupported{false};
std::atomic<bool> isTc4{false}, isRidge{false};
std::atomic<bool> testModeOn{false}; // DIP0 state; also tracked for the OSD
std::mutex padNameMutex;             // guards padName (UI writes, CPU reads)
char padName[96] = {};
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
void applyCabinetControl();
void serviceApplyTick();
void applyCabinetControl() {
  for (auto it=cabinetPulses.begin();it!=cabinetPulses.end();) {
    // A long hold trips the test menu's auto-repeat and counts as multiple
    // presses; hold just long enough to latch a single edge.
    if (u32(g_FrameCount-it->started)>=4) {
      ACJV::SetButtonState(0,it->mask,false);
      it=cabinetPulses.erase(it);
    } else ++it;
  }
#ifdef TC3_GAME_MENU
  serviceApplyTick();
#endif
  if (!cabinetControl)
    return;
  if (cabinetControl->quitting()) {
    stopRequested = true;
    if (VMManager::HasValidVM())
      VMManager::SetState(VMState::Stopping);
    return;
  }
  const auto state = VMManager::GetState();
  // Synchronized EEPROM reload + native game soft-reset across both cabinets
  // when either cabinet saves service settings in F10 > Cabinet.
  static std::uint32_t seenResetSeq = cabinetControl->reset_seq();
  const std::uint32_t curResetSeq = cabinetControl->reset_seq();
  if (curResetSeq != seenResetSeq && (state == VMState::Running || state == VMState::Paused)) {
    seenResetSeq = curResetSeq;
    const bool peerTc4 = tc3::service::is_tc4_sram(ACSRAM::buffer);
    if (const char* dir = std::getenv("TC3_CABINET_DIR")) {
      const std::string sharedSram = std::string(dir) + "/shared-sram.bin";
      if (FILE* f = std::fopen(sharedSram.c_str(), "rb")) {
        std::fread(ACSRAM::buffer, 1, ACSRAM_MAX_SIZE, f);
        std::fclose(f);
        ACSRAM::WriteFile();
      }
    }
    if (peerTc4 && eeMem && std::memcmp(eeMem->Main + 0x00c1e900, "TSF100-2-NA", 11) == 0) {
      std::memcpy(eeMem->Main + 0x00c1e900, ACSRAM::buffer, 0x616);
      // Also update the received-peer option shadow (0x00c1df98..0x00c1dfa0)
      // so the test menu never flashes DIFFERENT SETTING on the first frame.
      std::memcpy(eeMem->Main + 0x00c1df98, ACSRAM::buffer + 0x610, 4);
      eeMem->Main[0x00c1df9c] = ACSRAM::buffer[0x032];
      eeMem->Main[0x00c1df9d] = ACSRAM::buffer[0x033];
      eeMem->Main[0x00c1df9e] = ACSRAM::buffer[0x034];
      eeMem->Main[0x00c1df9f] = ACSRAM::buffer[0x030];
      eeMem->Main[0x00c1dfa0] = ACSRAM::buffer[0x031];
      // Trigger TC4LOAD's own native soft-reset (0x00346834: mode=2, next=4),
      // identical to leaving the test menu, so both screens restart the game
      // loop in lockstep with the new settings without losing the link.
      const u32 mode2 = 2, next4 = 4, zero = 0;
      std::memcpy(eeMem->Main + 0x00c1df10, &zero, 4);
      std::memcpy(eeMem->Main + 0x005acc10, &mode2, 4);
      std::memcpy(eeMem->Main + 0x005ac8e8, &next4, 4);
    } else if (eeMem) {
      // TC3 ("TST-03A") has no in-game soft-reset trigger; reload its live
      // EE mirror (which holds the same "TST-03A" block) and reboot the VM.
      for (size_t off = 0; off + 12 < Ps2MemSize::MainRam; off += 4) {
        if (std::memcmp(eeMem->Main + off + 4, "TST-03A ", 8) == 0) {
          std::memcpy(eeMem->Main + off, ACSRAM::buffer, 0x192);
          std::memcpy(eeMem->Main + off + 0x194, ACSRAM::buffer, 0x192);
          break;
        }
      }
      if (state == VMState::Paused)
        VMManager::SetPaused(false);
      VMManager::Reset();
    }
    if (ACJV::GetDIPSwitchState(0))
      ACJV::ToggleDIPSwitchState(0);
    testModeOn = false;
    cabinetControl->set_paused(false);
    if (state == VMState::Paused)
      VMManager::SetPaused(false);
    std::printf("[TC3 service] synchronized cabinet reset (seq=%u)\n", curResetSeq);
    std::fflush(stdout);
    return;
  }
  if (previewFramesRunning && state != VMState::Running)
    previewFramesRunning = false;
  if (!previewFramesRunning && (state == VMState::Running || state == VMState::Paused) &&
      (state == VMState::Paused) != cabinetControl->paused())
    VMManager::SetPaused(cabinetControl->paused());
  // One machine, one set of speakers: the supervisor asks one cabinet to
  // stay silent. Reapplied each field because settings reloads restore the
  // configured volume.
  static const bool cabinetMuted = std::getenv("TC3_CABINET_MUTE") != nullptr;
  if (cabinetMuted && (state == VMState::Running || state == VMState::Paused))
    SPU2::SetOutputVolume(0);
  applyCabinetRendering();
}
#ifdef TC3_GAME_MENU
// ---------------------------------------------------------------------------
// Direct EEPROM service-option writer. Writes TC4 option bytes/bitfields
// directly to ACSRAM::buffer with TC4's exact CRC16 (0x614..0x615) and mirror
// block (0x640..0xc55), persists sram.bin, and triggers a synchronized reset
// on both cabinets so they reboot together with the new settings.
// ---------------------------------------------------------------------------
namespace svc {
std::atomic<int> pubState{0};
char pubNote[128] = {};

int decodeValue(int opt) {
  return tc3::service::read_option(ACSRAM::buffer, opt);
}

void finish(int state, const char* note) {
  {
    std::lock_guard lock(statusMutex);
    std::snprintf(pubNote, sizeof(pubNote), "%s", note);
  }
  pubState = state;
  std::printf("[TC3 service] %s\n", note);
  std::fflush(stdout);
}

void request(const int* targets, int count) {
  // Never run the TC3/TC4 block writers against an unrecognized SRAM layout
  // (e.g. Ridge V's tagged-record EEPROM) — that would corrupt it.
  if (!tc3::service::is_tc4_sram(ACSRAM::buffer) &&
      !tc3::service::is_tc3_sram(ACSRAM::buffer) &&
      !tc3::service::is_ridge_sram(ACSRAM::buffer)) {
    finish(3, "EEPROM format not recognized; settings not applied.");
    return;
  }
  // Sync live EE shadow (0x00c1e900) into ACSRAM::buffer first if valid so we
  // preserve write counters/bookkeeping before applying the edited options.
  if (eeMem && std::memcmp(eeMem->Main + 0x00c1e900, "TSF100-2-NA", 11) == 0)
    std::memcpy(ACSRAM::buffer, eeMem->Main + 0x00c1e900, 0x616);

  tc3::service::write_options_and_commit(ACSRAM::buffer, targets, count);

  // Update the live EE shadow too so the game's 1Hz marshal cannot clobber
  // ACSRAM::buffer before the reset lands.
  if (eeMem && std::memcmp(eeMem->Main + 0x00c1e900, "TSF100-2-NA", 11) == 0)
    std::memcpy(eeMem->Main + 0x00c1e900, ACSRAM::buffer, 0x616);

  ACSRAM::WriteFile();

  if (const char* dir = std::getenv("TC3_CABINET_DIR")) {
    const std::string sharedSram = std::string(dir) + "/shared-sram.bin";
    if (FILE* f = std::fopen(sharedSram.c_str(), "wb")) {
      std::fwrite(ACSRAM::buffer, 1, ACSRAM_MAX_SIZE, f);
      std::fclose(f);
    }
  }

  finish(2, "Settings saved to EEPROM; resetting machine...");

  if (cabinetControl) {
    if (ACJV::GetDIPSwitchState(0)) {
      ACJV::ToggleDIPSwitchState(0);
      cabinetControl->set_test_desired(false);
    }
    testModeOn = false;
    cabinetControl->set_paused(false);
    cabinetControl->request_reset();
  } else {
    if (ACJV::GetDIPSwitchState(0))
      ACJV::ToggleDIPSwitchState(0);
    testModeOn = false;
    if (tc3::service::is_tc4_sram(ACSRAM::buffer) && eeMem &&
        std::memcmp(eeMem->Main + 0x00c1e900, "TSF100-2-NA", 11) == 0) {
      const u32 mode2 = 2, next4 = 4, zero = 0;
      std::memcpy(eeMem->Main + 0x00c1df10, &zero, 4);
      std::memcpy(eeMem->Main + 0x005acc10, &mode2, 4);
      std::memcpy(eeMem->Main + 0x005ac8e8, &next4, 4);
    } else if (VMManager::HasValidVM()) {
      VMManager::Reset();
    }
    if (VMManager::GetState() == VMState::Paused)
      VMManager::SetPaused(false);
  }
}
} // namespace svc
void serviceApplyTick() {}
#endif
void scanSettingsShadow() {
  static const bool enabled = std::getenv("TC3_SETTINGS_SCAN") != nullptr;
  static u64 next = 1500;
  if (enabled && g_FrameCount >= next) {
    next = g_FrameCount + 600;
    static const u8 sig[] = "TSF100-2-NA";
  auto scan = [&](const u8* mem, size_t size, const char* name) {
    for (size_t i = 0; i + sizeof(sig) - 1 <= size; i++)
      if (!std::memcmp(mem + i, sig, sizeof(sig) - 1)) {
        std::printf("[TC3 scan] %s hit at +0x%zx:", name, i);
        for (size_t j = 0; j < 0x40; j++)
          std::printf("%02x", mem[i + j]);
        std::printf(" | opts:");
        for (size_t j = 0x30; j < 0x40; j++)
          std::printf("%02x", mem[i + j]);
        std::printf(" | ctr:%02x%02x ck:%02x%02x | mir:%02x%02x%02x\n",
                    mem[i + 0x60], mem[i + 0x61], mem[i + 0x614],
                    mem[i + 0x615], mem[i + 0x670], mem[i + 0x671],
                    mem[i + 0x672]);
      }
  };
    if (eeMem)
      scan(eeMem->Main, Ps2MemSize::MainRam, "eeMem");
    if (iopMem)
      scan(iopMem->Main, Ps2MemSize::ExposedIopRam, "iopMem");
  }
  // Debug command channel: /tmp/tc3cmd lines "dump <path>" | "poke <eeoff> <off> <val>"
  if (FILE* cf = std::fopen("/tmp/tc3cmd", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), cf)) {
      if (!std::strncmp(line, "dump ", 5)) {
        char path[200] = {};
        if (std::sscanf(line + 5, "%199s", path) == 1 && eeMem) {
          if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(eeMem->Main, 1, Ps2MemSize::MainRam, f);
            std::fclose(f);
            std::printf("[TC3 dump] eeMem -> %s\n", path);
          }
        }
      } else if (!std::strncmp(line, "poke ", 5)) {
        unsigned eeoff, val;
        if (std::sscanf(line + 5, "%x %x", &eeoff, &val) == 2 && eeMem &&
            eeoff < Ps2MemSize::MainRam) {
          eeMem->Main[eeoff] = val & 0xff;
          std::printf("[TC3 poke] eeMem+%x=%02x\n", eeoff, val & 0xff);
        }
      } else if (!std::strncmp(line, "dip ", 4)) {
        unsigned idx;
        if (std::sscanf(line + 4, "%x", &idx) == 1) {
          ACJV::ToggleDIPSwitchState(idx);
          if (idx == 0) testModeOn = ACJV::GetDIPSwitchState(0);
          std::printf("[TC3 dip] toggle %u state=%d\n", idx,
                      ACJV::GetDIPSwitchState(idx));
        }
      } else if (!std::strncmp(line, "snap", 4)) {
        tc3::playable::RequestScreenshot();
      } else if (!std::strncmp(line, "menu ", 5)) {
#ifdef TC3_GAME_MENU
        int open;
        if (std::sscanf(line + 5, "%d", &open) == 1)
          tc3::menu::SetOpen(open != 0);
#endif
      } else if (!std::strncmp(line, "apply ", 6)) {
#ifdef TC3_GAME_MENU
        // apply <target0> <target1> ... — service-option indices, -1 skips.
        int targets[tc3::service::kMaxOptionCount], n = 0, off = 6, v;
        std::fill(std::begin(targets), std::end(targets), -1);
        while (n < tc3::service::kMaxOptionCount &&
               std::sscanf(line + off, "%d", &v) == 1) {
          targets[n++] = v;
          while (line[off] && line[off] != ' ') off++;
          while (line[off] == ' ') off++;
        }
        svc::request(targets, n);
#endif
      } else if (!std::strncmp(line, "jvs ", 4)) {
        unsigned mask;
        if (std::sscanf(line + 4, "%x", &mask) == 1) {
          ACJV::SetButtonState(0, mask & 0xffff, true);
          cabinetPulses.push_back({static_cast<u16>(mask), g_FrameCount});
          std::printf("[TC3 jvs] pulse mask=%04x\n", mask & 0xffff);
        }
      }
    }
    std::fclose(cf);
    std::remove("/tmp/tc3cmd");
  }
  std::fflush(stdout);
}
void telemetry() {
  static uint64_t last = 0;
  const auto now = milliseconds();
  if (now - last < 1000)
    return;
  last = now;
  scanSettingsShadow();
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
  auto snapshot = tc3::playable::GetStatus();
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
  options.aspect = GSConfig.AspectRatio == AspectRatioType::R16_9 ? 1 :
                   GSConfig.AspectRatio == AspectRatioType::Stretch ? 2 : 0;
  options.display_size = appliedDisplaySize.load();
  options.frame_skip = GSConfig.FrameSkip;
  options.dlss_supported = detectDlssCapable();
  options.dlss = appliedDLSS.load();
  renderingReady = options.ready;
  casSupported = options.cas_supported;
  tc3::menu::Snapshot ms{snapshot.game_fps, snapshot.video_fps,
                       snapshot.running, snapshot.fullscreen,
                       float(logicalWidth.load()),
                       float(logicalHeight.load()), options,
                       snapshot.speed_percent, cabinetControl != nullptr,
                       twinSupported.load()};
  ms.twin_default = twinDefault.load();
  ms.sinden_devices = tc3::sinden::detected.load();
  ms.service_supported = serviceSupported.load();
  ms.is_tc4 = isTc4.load();
  ms.is_ridge = isRidge.load();
  ms.has_sinden = !isRidge.load();
  {
    std::lock_guard lock(padNameMutex);
    std::snprintf(ms.pad_name, sizeof(ms.pad_name), "%s", padName);
  }
  ms.service_state = svc::pubState.load();
  {
    std::lock_guard lock(statusMutex);
    std::snprintf(ms.service_note, sizeof(ms.service_note), "%s", svc::pubNote);
  }
  if (serviceSupported) {
    const int optCount = isRidge.load() ? tc3::service::kRidgeOptionCount
                                        : tc3::service::kOptionCount;
    for (int i = 0; i < optCount && i < 8; ++i)
      ms.service_values[i] = svc::decodeValue(i);
  }
  // Keep test mode paired across both cabinets: publish local DIP changes
  // and chase the twin's published state so F9 on one screen enters and
  // exits the test menu on both machines together.
  bool dipState = ACJV::GetDIPSwitchState(0);
  if (cabinetControl) {
    static std::uint32_t seenSeq = 0;
    static bool knownDip = false;
    const std::uint32_t seq = cabinetControl->test_seq();
    const bool desired = cabinetControl->test_desired();
    if (seq != seenSeq) {
      seenSeq = seq;
      if (desired != dipState) {
        ACJV::ToggleDIPSwitchState(0);
        dipState = !dipState;
      }
      knownDip = dipState;
    } else if (dipState != knownDip) {
      cabinetControl->set_test_desired(dipState);
      seenSeq = cabinetControl->test_seq();
      knownDip = dipState;
    }
  }
  ms.test_mode = dipState;
  // Show the loading screen only until the game starts displaying frames
  // (snapshot.game_fps >= 0.5), and report the real live boot stage as it
  // progresses through dongle boot -> IOP modules -> HDD load -> JVS/link init.
  ms.loading = !dipState && (snapshot.game_fps < 0.5);
  if (ms.loading) {
    const bool tc4 = isTc4.load();
    u32 tc4Code = 0, tc4LinkInit = 0;
    if (tc4 && eeMem) {
      std::memcpy(&tc4Code, eeMem->Main + 0x00362bf0, 4);
      std::memcpy(&tc4LinkInit, eeMem->Main + 0x00c1fb40, 4);
    }
    if (g_FrameCount == 0) {
      ms.load_progress = 0.12f;
      std::snprintf(ms.load_stage, sizeof(ms.load_stage),
                    tc4 ? "Starting System 256 VM & Vulkan renderer..."
                        : "Starting System 246 VM & Vulkan renderer...");
    } else if (!ACJV::enabled) {
      ms.load_progress = 0.30f;
      std::snprintf(ms.load_stage, sizeof(ms.load_stage),
                    "Booting security dongle (proverb.elf) & IOP kernel... [field %u]",
                    g_FrameCount);
    } else if (tc4 && tc4Code == 0) {
      ms.load_progress = 0.55f;
      std::snprintf(ms.load_stage, sizeof(ms.load_stage),
                    "Mounting HDD image & loading IOP modules (ACJV / ACSRAM / ATAD)... [field %u]",
                    g_FrameCount);
    } else if (tc4 && tc4LinkInit == 0) {
      ms.load_progress = 0.80f;
      std::snprintf(ms.load_stage, sizeof(ms.load_stage),
                    "Loading TC4LOAD game binary from HDD... [field %u]",
                    g_FrameCount);
    } else if (isRidge.load()) {
      ms.load_progress = 0.72f;
      std::snprintf(ms.load_stage, sizeof(ms.load_stage),
                    "Loading RRV3 game program from CD media & initializing FCA-1 I/O + ILSOCK link... [field %u]",
                    g_FrameCount);
    } else if (!tc4) {
      ms.load_progress = 0.72f;
      std::snprintf(ms.load_stage, sizeof(ms.load_stage),
                    "Loading TST game program from DVD media & initializing NA-JV I/O + ILSOCK link... [field %u]",
                    g_FrameCount);
    } else {
      ms.load_progress = 0.95f;
      std::snprintf(ms.load_stage, sizeof(ms.load_stage),
                    "Initializing NA-JV I/O board & twin-cabinet link... [field %u]",
                    g_FrameCount);
    }
  } else {
    ms.load_progress = 1.0f;
  }
  static const int role = [] {
    const char* r = std::getenv("TC3_CABINET_ROLE");
    return r ? (r[0] == 'l' ? 0 : 1) : -1;
  }();
  ms.cabinet_role = role;
  tc3::menu::PublishSnapshot(ms);
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
  // Leaving the test menu reboots the VM with the dip still set, which
  // would boot straight back into test mode. Only the first boot honours a
  // persisted test-mode dip; a reboot clears it so the machine lands in the
  // game, and the published state drags the twin out with it.
  static bool bootedOnce = false;
  if (bootedOnce && ACJV::GetDIPSwitchState(0)) {
    ACJV::ToggleDIPSwitchState(0);
    if (cabinetControl)
      cabinetControl->set_test_desired(false);
  }
  bootedOnce = true;
  report("VM", "Boot succeeded", false);
  // The arcade cabinet ships linked: unless the user turned the pair off,
  // hand this instance over to the cabinet supervisor once the first VM has
  // proven it can boot. Workers re-enter with TC3_CABINET_ROLE and skip this.
  static bool twinHandoff = false;
  if (!twinHandoff && twinDefault && twinSupported && !cabinetControl) {
    twinHandoff = true;
    report("Twin screens", "Linked cabinet pair enabled by default; use "
           "F10 > Display > Linked cabinet at boot to disable", false);
    tc3::playable::RequestTwinScreens();
  }
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
std::string bootDataRoot, bootManifest; // UI thread only; set once in main().
void RequestTwinScreens() {
  if (!twinSupported) return;
  ui([] {
    const auto launcher = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "tools/run_cabinet_pair.py";
    const auto path = launcher.string();
    const auto source = bootDataRoot, manifest = bootManifest;
    // The prepared data root lives under the game repo's .local; supervisor
    // state (clones, logs, pidfile, report) belongs beside it, not in the
    // foundation checkout.
    const auto stateRoot = std::filesystem::path(source).parent_path().string();
    const char* arguments[] = {"python3", path.c_str(), "--background",
                               "--source", source.c_str(),
                               "--manifest", manifest.c_str(),
                               "--state-root", stateRoot.c_str(), nullptr};
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
void RequestServiceApply(const int* targets, int count) {
  Host::RunOnCPUThread([snapshot = std::vector<int>(targets, targets + count)] {
    if (!VMManager::HasValidVM()) return;
    svc::request(snapshot.data(), int(snapshot.size()));
  });
}
void RequestCabinetAction(tc3::menu::Action action) {
  Host::RunOnCPUThread([action] {
    if (!VMManager::HasValidVM()) return;
    using A=tc3::menu::Action;
    if (action==A::CabinetTest) { ACJV::ToggleDIPSwitchState(0); testModeOn = ACJV::GetDIPSwitchState(0); }
    else if (action==A::CabinetCoin) ACJV::InsertCoin(0);
    else {
      u16 mask=0;
      switch(action) {
      case A::CabinetUp: mask=JVS_BTN_UP;break;
      case A::CabinetDown: mask=JVS_BTN_DOWN;break;
      case A::CabinetLeft: mask=JVS_BTN_LEFT;break;
      case A::CabinetRight: mask=JVS_BTN_RIGHT;break;
      case A::CabinetEnter: mask=ACJV::GetMode()==JVS_MODE::DRIVE ? JVS_BTN_START : JVS_BTN_1;break;
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
      {
        const std::string ar = settings->GetStringValue("EmuCore/GS", "AspectRatio", "4:3");
        current.aspect = ar == "16:9" ? 1 : ar == "Stretch" ? 2 : 0;
      }
      current.display_size = settings->GetIntValue("UI", "DisplaySize", 0);
      current.frame_skip = settings->GetIntValue("EmuCore/GS", "FrameSkip", 0);
      current.dlss = settings->GetIntValue("TC3", "DLSSMode", 0);
      current.dlss_supported = detectDlssCapable();
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
      case tc3::rendering::Option::Widescreen: {
        static const char* aspect_names[] = {"4:3", "16:9", "Stretch"};
        settings->SetStringValue("EmuCore/GS", "AspectRatio", aspect_names[value.aspect]);
        break;
      }
      case tc3::rendering::Option::DisplaySize:
        settings->SetIntValue("UI", "DisplaySize", value.display_size);
        appliedDisplaySize = value.display_size; break;
      case tc3::rendering::Option::FrameSkip:
        settings->SetIntValue("EmuCore/GS", "FrameSkip", value.frame_skip); break;
      case tc3::rendering::Option::DLSS:
        settings->SetIntValue("TC3", "DLSSMode", value.dlss);
        appliedDLSS = value.dlss; break;
      }
    }
    if (option == tc3::rendering::Option::DisplaySize && value.display_size > 0 && !cabinetControl) {
      static const int display_sizes[][2] = {{0, 0}, {960, 720}, {1280, 960}, {1920, 1440}};
      Host::RequestResizeHostDisplay(display_sizes[value.display_size][0],
                                   display_sizes[value.display_size][1]);
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
    {
      static const char* aspect_names[] = {"4:3", "16:9", "Stretch"};
      settings->SetStringValue("EmuCore/GS", "AspectRatio", aspect_names[value->aspect]);
    }
    settings->SetIntValue("UI", "DisplaySize", value->display_size);
    settings->SetIntValue("EmuCore/GS", "FrameSkip", value->frame_skip);
    settings->SetIntValue("TC3", "DLSSMode", value->dlss);
    appliedDisplaySize = value->display_size;
    appliedDLSS = value->dlss;
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
  appliedDisplaySize = settings->GetIntValue("UI", "DisplaySize", 0);
  appliedDLSS = settings->GetIntValue("TC3", "DLSSMode", 0);
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
// ---- Gamepads -------------------------------------------------------------
// The upstream SDL input source stays disabled (it would drain this thread's
// event queue). The frontend opens gamepads itself and translates their
// events into the existing Keyboard/* bindings, so a pad navigates the menu
// and drives the game through exactly the same binding path. TC3_PAD_INDEX
// selects which enumerated pad this process uses; the twin supervisor gives
// the left worker 0 and the right worker 1, so each cabinet has its own pad.
int padTargetIndex = -1;
SDL_Gamepad *pad = nullptr;
SDL_JoystickID padId = 0;
struct PadButtonMap { SDL_GamepadButton button; SDL_Keycode key; };
struct PadAxisMap { SDL_GamepadAxis axis; SDL_Keycode pos, neg; float pos_sent = 0, neg_sent = 0; };
std::vector<PadButtonMap> padButtons;
std::vector<PadAxisMap> padAxes;
SDL_Keycode padKey(const char *section, const char *option,
                   SDL_Keycode fallback) {
  for (const auto &bind : settings->GetStringList(section, option))
    if (bind.rfind("Keyboard/", 0) == 0)
      if (const SDL_Keycode key = SDL_GetKeyFromName(bind.c_str() + 9))
        return key;
  return fallback;
}
void setPadName(const char *name) {
  std::lock_guard lock(padNameMutex);
  std::snprintf(padName, sizeof(padName), "%s", name ? name : "");
}
void padInvoke(SDL_Keycode code, float value) {
  const auto key = InputManager::MakeHostKeyboardKey(code);
  Host::RunOnCPUThread(
      [key, value, code] { invokeTracedInput(key, value, "pad", code); });
}
void padZeroAxes() {
  for (auto &m : padAxes) {
    if (m.pos_sent > 0) { padInvoke(m.pos, 0); m.pos_sent = 0; }
    if (m.neg && m.neg_sent > 0) { padInvoke(m.neg, 0); m.neg_sent = 0; }
  }
}
void refreshGamepads() {
  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);
  const SDL_JoystickID want =
      (ids && padTargetIndex >= 0 && padTargetIndex < count)
          ? ids[padTargetIndex]
          : 0;
  SDL_free(ids);
  if (pad && padId != want) {
    padZeroAxes();
    std::fprintf(stderr, "[TC3 input] gamepad disconnected: %s\n", padName);
    SDL_CloseGamepad(pad);
    pad = nullptr;
    padId = 0;
    setPadName("");
  }
  if (want && padId != want) {
    pad = SDL_OpenGamepad(want);
    if (pad) {
      padId = want;
      setPadName(SDL_GetGamepadName(pad));
      std::fprintf(stderr, "[TC3 input] gamepad %d connected: %s\n",
                   padTargetIndex, padName);
    }
  }
}
void setupGamepad() {
  // Driving cabinets only. Gun games are pointer-aim only (mouse or Sinden):
  // a pad cannot aim, so TC3/TC4 keep keyboard+pointer input exclusively.
  if (!isRidge)
    return;
  const char *env = std::getenv("TC3_PAD_INDEX");
  padTargetIndex = env ? std::atoi(env) : 0;
  if (padTargetIndex < 0)
    return;
  const auto button = [](SDL_GamepadButton b, const char *option,
                         SDL_Keycode fallback, const char *section = "JVS") {
    padButtons.push_back({b, padKey(section, option, fallback)});
  };
  button(SDL_GAMEPAD_BUTTON_DPAD_UP, "P1_Up", SDLK_UP);
  button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, "P1_Down", SDLK_DOWN);
  button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, "P1_Left", SDLK_LEFT);
  button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "P1_Right", SDLK_RIGHT);
  button(SDL_GAMEPAD_BUTTON_START, "P1_Start", SDLK_RETURN);
  button(SDL_GAMEPAD_BUTTON_BACK, "Coin1", SDLK_5);
  button(SDL_GAMEPAD_BUTTON_GUIDE, "P1_Service", SDLK_9);
  button(SDL_GAMEPAD_BUTTON_SOUTH, "P1_Start", SDLK_RETURN);
  button(SDL_GAMEPAD_BUTTON_EAST, "Racing_View_P1", SDLK_V);
  button(SDL_GAMEPAD_BUTTON_WEST, "Racing_ShiftDown_P1", SDLK_Q);
  button(SDL_GAMEPAD_BUTTON_NORTH, "Racing_ShiftUp_P1", SDLK_E);
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "Racing_ShiftDown_P1", SDLK_Q);
  button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "Racing_ShiftUp_P1", SDLK_E);
  padAxes.push_back({SDL_GAMEPAD_AXIS_LEFTX, padKey("JVS", "SteerRight", SDLK_D),
                     padKey("JVS", "SteerLeft", SDLK_A)});
  padAxes.push_back({SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                     padKey("JVS", "Brake", SDLK_S), 0});
  padAxes.push_back({SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                     padKey("JVS", "Gas", SDLK_W), 0});
}
void dispatchGamepad(const SDL_Event &e) {
  if (e.type == SDL_EVENT_GAMEPAD_ADDED ||
      e.type == SDL_EVENT_GAMEPAD_REMOVED) {
    refreshGamepads();
    return;
  }
  if (!pad || padTargetIndex < 0)
    return;
  if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
      e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
    if (e.gbutton.which != padId)
      return;
    for (const auto &m : padButtons) {
      if (m.button != static_cast<SDL_GamepadButton>(e.gbutton.button))
        continue;
      // Re-enter the queue as a real key event so the menu and every
      // Keyboard/* binding handle it identically to a physical key.
      SDL_Event se{};
      se.type = e.gbutton.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      se.key.key = m.key;
      se.key.scancode = SDL_GetScancodeFromKey(m.key, nullptr);
      se.key.down = e.gbutton.down;
      se.key.windowID = SDL_GetWindowID(window);
      SDL_PushEvent(&se);
      return;
    }
  } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
    if (e.gaxis.which != padId)
      return;
    for (auto &m : padAxes) {
      if (m.axis != static_cast<SDL_GamepadAxis>(e.gaxis.axis))
        continue;
      const float v = std::clamp(e.gaxis.value / 32767.f, -1.f, 1.f);
      const float deadzone = m.neg ? 0.12f : 0.05f;
      const float pos = v > deadzone ? v : 0.f;
      const float neg = m.neg && v < -deadzone ? -v : 0.f;
      if (std::abs(pos - m.pos_sent) > 0.005f || (pos == 0) != (m.pos_sent == 0)) {
        m.pos_sent = pos;
        padInvoke(m.pos, pos);
      }
      if (m.neg &&
          (std::abs(neg - m.neg_sent) > 0.005f || (neg == 0) != (m.neg_sent == 0))) {
        m.neg_sent = neg;
        padInvoke(m.neg, neg);
      }
    }
  }
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
    // Drop stale dev commands left by a dead process so a leftover "dip 0"
    // cannot boot the machine into test mode.
    std::remove("/tmp/tc3cmd");
    const char *hardwareError = nullptr;
    if (!VMManager::PerformEarlyHardwareChecks(&hardwareError))
      throw std::runtime_error(hardwareError ? hardwareError
                                             : "Unsupported CPU");
    initializeSettings(dataRoot);
    tc3::playable::bootDataRoot = std::filesystem::absolute(dataRoot).string();
    tc3::playable::bootManifest = std::filesystem::absolute(manifest).string();
    INISettingsInterface gameManifest(std::filesystem::absolute(manifest).string());
    if (!gameManifest.Load()) throw std::runtime_error("Cannot read arcade game manifest");
    const std::string gameId = gameManifest.GetStringValue("game", "gameid", "");
    twinSupported = gameId == "NM00012" || gameId == "NM00032" ||
                    gameId == "NM00001";
    serviceSupported = twinSupported.load();
    isTc4 = gameId == "NM00032";
    isRidge = gameId == "NM00001";
    // The arcade cabinet ships linked; keep the pair on by default. The menu
    // can turn it off; the choice is persisted under the INI key below.
    twinDefault = twinSupported.load() &&
                  settings->GetBoolValue("TC3", "LinkedCabinet", true);
    tc3::sinden::mode = std::clamp(settings->GetIntValue("Lightgun", "SindenBorder", 1),0,2);
    tc3::sinden::width_percent = std::clamp(settings->GetIntValue("Lightgun", "BorderWidth", 2),1,5);
    // Driving cabinets have no lightgun: keep the Sinden tracking border off.
    if (isRidge) tc3::sinden::mode = 0;
    if (const char* role = std::getenv("TC3_CABINET_ROLE")) {
      const char* directory = std::getenv("TC3_CABINET_DIR");
      if (!directory)
        throw std::runtime_error("The cabinet role requires a supervisor session directory");
      cabinetControl = std::make_unique<tc3::cabinet::SharedControl>(
          std::filesystem::path(directory) / "control.bin", std::string_view(role) == "right" ? 2U : 1U);
    }
    setupGamepad();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
      throw std::runtime_error(SDL_GetError());
    uiThread = std::this_thread::get_id();
    const auto gameName = gameManifest.GetStringValue("game", "name", "Time Crisis");
    window = SDL_CreateWindow(gameName.c_str(),
                              1100, 770,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
      throw std::runtime_error(SDL_GetError());
    if (const char* role = std::getenv("TC3_CABINET_ROLE")) {
      SDL_SetWindowTitle(window, std::string(gameName).append(" — ").append(role).c_str());
      // One machine, two flush screens: undecorated and pinned to this
      // worker's share of the combined display. With two monitors each gets
      // a real fullscreen screen; on a single display the pair tiles as
      // borderless halves with no gap between them.
      const bool right = std::string_view(role) == "right";
      int count = 0;
      SDL_DisplayID* displays = SDL_GetDisplays(&count);
      SDL_SetWindowBordered(window, false);
      if (displays && count >= 2) {
        SDL_Rect bounds{};
        SDL_GetDisplayBounds(displays[right ? 1 : 0], &bounds);
        SDL_SetWindowPosition(window, bounds.x, bounds.y);
        SDL_SetWindowSize(window, bounds.w, bounds.h);
        SDL_SetWindowFullscreen(window, true);
      } else {
        SDL_Rect bounds{};
        SDL_GetDisplayUsableBounds(displays ? displays[0] : SDL_GetPrimaryDisplay(),
                                   &bounds);
        bounds.w /= 2;
        if (right) bounds.x += bounds.w;
        SDL_SetWindowPosition(window, bounds.x, bounds.y);
        SDL_SetWindowSize(window, bounds.w, bounds.h);
      }
      SDL_free(displays);
    }
    updateWindow();
    if (padTargetIndex >= 0)
      refreshGamepads();
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
      case tc3::menu::Action::ToggleTwinDefault:
        // Persisted default; takes effect on next launch.
        Host::RunOnCPUThread([] {
          auto lock = Host::GetSettingsLock();
          const bool next = !twinDefault.load();
          settings->SetBoolValue("TC3", "LinkedCabinet", next);
          twinDefault = next;
          Host::CommitBaseSettingChanges();
        });
        break;
      default:
        tc3::playable::RequestCabinetAction(action);
        break;
      }
    });
    tc3::menu::SetOptionCallback(tc3::playable::RequestRenderingOption);
    tc3::menu::SetServiceApplyCallback(tc3::playable::RequestServiceApply);
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
          padZeroAxes();
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
        if (e.type >= SDL_EVENT_GAMEPAD_AXIS_MOTION &&
            e.type <= SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)
          dispatchGamepad(e);
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
