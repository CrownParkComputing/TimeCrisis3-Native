// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_menu.h"
#include "../rendering/capabilities.h"
#include "../input/sinden.h"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_IMPLEMENTATION
#include "nuklear.h"

namespace tc3::menu {
namespace {
struct ButtonEvent { float x, y; bool down; };
struct Shared {
    std::mutex mutex;
    Snapshot snapshot;
    std::function<void(Action)> callback;
    std::function<void(rendering::Option, int)> option_callback;
    float x = 0, y = 0, wheel_x = 0, wheel_y = 0;
    bool down = false;
    std::vector<ButtonEvent> buttons;
    std::vector<SDL_Keycode> keys;
} shared;
std::atomic<bool> open{false};

// Nuklear and all font/draw-list state belong exclusively to the GS thread.
nk_context context{};
nk_user_font font{};
nk_buffer commands{}, vertices{}, indices{};
ImFontBaked* baked_font = nullptr;
bool initialized = false;
int page = 0;
int selected = -1;

void Dispatch(Action action)
{
    std::function<void(Action)> callback;
    { std::lock_guard lock(shared.mutex); callback = shared.callback; }
    if (callback) callback(action);
}

void SetOpen(bool value)
{
    if (open.exchange(value) == value) return;
    {
        std::lock_guard lock(shared.mutex);
        shared.buttons.clear();
        shared.keys.clear();
        shared.down = false;
        shared.wheel_x = shared.wheel_y = 0;
    }
    Dispatch(value ? Action::Pause : Action::Resume);
}

float TextWidth(nk_handle, float, const char* text, int length)
{
    float result = 0;
    // The menu strings are ASCII; keep the adapter independent of atlas baking.
    for (int i = 0; i < length; ++i)
        result += baked_font->FindGlyph(static_cast<unsigned char>(text[i]))->AdvanceX;
    return result;
}

void QueryGlyph(nk_handle, float, nk_user_font_glyph* out, nk_rune codepoint, nk_rune)
{
    const ImFontGlyph* glyph = baked_font->FindGlyph(static_cast<ImWchar>(codepoint));
    out->width = glyph->X1 - glyph->X0;
    out->height = glyph->Y1 - glyph->Y0;
    out->offset = nk_vec2(glyph->X0, glyph->Y0);
    out->xadvance = glyph->AdvanceX;
    out->uv[0] = nk_vec2(glyph->U0, glyph->V0);
    out->uv[1] = nk_vec2(glyph->U1, glyph->V1);
}

void AppendGeometry()
{
    const nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(ImDrawVert, pos)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(ImDrawVert, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(ImDrawVert, col)},
        {NK_VERTEX_LAYOUT_END}
    };
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    nk_convert_config config{};
    config.global_alpha = 1;
    config.shape_AA = config.line_AA = NK_ANTI_ALIASING_ON;
    config.circle_segment_count = config.arc_segment_count = config.curve_segment_count = 22;
    config.tex_null = {nk_handle_id(1), nk_vec2(white.x, white.y)};
    config.vertex_layout = layout;
    config.vertex_size = sizeof(ImDrawVert);
    config.vertex_alignment = alignof(ImDrawVert);
    static_assert(sizeof(ImDrawIdx) == sizeof(nk_draw_index));
    if (nk_convert(&context, &commands, &vertices, &indices, &config) != NK_CONVERT_SUCCESS)
        return;
    const auto count = vertices.allocated / sizeof(ImDrawVert);
    if (!count || count > std::numeric_limits<ImDrawIdx>::max()) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!(draw->Flags & ImDrawListFlags_AllowVtxOffset) &&
        draw->_VtxCurrentIdx + count > std::numeric_limits<ImDrawIdx>::max()) return;

    // ImGui 1.92 owns/uploads this atlas. Retain its texture reference, not a
    // prematurely resolved GPU texture ID; it may still need an atlas upload.
    draw->PushTexture(ImGui::GetFont()->OwnerAtlas->TexRef);
    draw->PrimReserve(0, static_cast<int>(count));
    const unsigned base = draw->_VtxCurrentIdx; // PrimReserve can change VtxOffset.
    std::memcpy(draw->_VtxWritePtr, nk_buffer_memory_const(&vertices), vertices.allocated);
    draw->_VtxWritePtr += count;
    draw->_VtxCurrentIdx += static_cast<unsigned>(count);
    const auto* source = static_cast<const nk_draw_index*>(nk_buffer_memory_const(&indices));
    const nk_draw_command* command;
    nk_draw_foreach(command, &context, &commands) {
        if (!command->elem_count) continue;
        const auto& r = command->clip_rect;
        draw->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), true);
        draw->PrimReserve(static_cast<int>(command->elem_count), 0);
        for (unsigned i = 0; i < command->elem_count; ++i)
            draw->_IdxWritePtr[i] = static_cast<ImDrawIdx>(base + source[i]);
        draw->_IdxWritePtr += command->elem_count;
        source += command->elem_count;
        draw->PopClipRect();
    }
    draw->PopTexture();
}
} // namespace

void SetActionCallback(std::function<void(Action)> callback)
{
    std::lock_guard lock(shared.mutex);
    shared.callback = std::move(callback);
}

void SetOptionCallback(std::function<void(rendering::Option, int)> callback)
{
    std::lock_guard lock(shared.mutex);
    shared.option_callback = std::move(callback);
}

void PublishSnapshot(const Snapshot& snapshot)
{
    std::lock_guard lock(shared.mutex);
    shared.snapshot = snapshot;
}

bool IsOpen() { return open.load(); }

bool HandleEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F10) {
        SetOpen(!IsOpen());
        return true;
    }
    if (!IsOpen()) return false;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_BACKSPACE)) {
        SetOpen(false);
        return true;
    }
    std::lock_guard lock(shared.mutex);
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        shared.x = event.motion.x; shared.y = event.motion.y;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        shared.x = event.button.x; shared.y = event.button.y;
        if (event.button.button == SDL_BUTTON_LEFT) {
            shared.down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            // Bounded queue; the final button state is also sent every frame.
            if (shared.buttons.size() < 128)
                shared.buttons.push_back({shared.x, shared.y, shared.down});
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        shared.wheel_x += event.wheel.x; shared.wheel_y += event.wheel.y;
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        shared.down = false;
        shared.buttons.clear();
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (shared.keys.size() < 64 &&
            (!event.key.repeat || event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT ||
             event.key.key == SDLK_UP || event.key.key == SDLK_DOWN))
            shared.keys.push_back(event.key.key == SDLK_TAB && (event.key.mod & SDL_KMOD_SHIFT)
                                     ? SDLK_UNKNOWN : event.key.key);
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    default:
        return false;
    }
}

void Initialize()
{
    if (initialized || !ImGui::GetCurrentContext()) return;
    font.height = ImGui::GetFontSize();
    font.width = TextWidth;
    font.query = QueryGlyph;
    font.texture = nk_handle_id(1);
    if (!nk_init_default(&context, &font)) return;
    nk_buffer_init_default(&commands);
    nk_buffer_init_default(&vertices);
    nk_buffer_init_default(&indices);
    initialized = true;
}

void Shutdown()
{
    if (!initialized) return;
    nk_free(&context);
    nk_buffer_free(&commands);
    nk_buffer_free(&vertices);
    nk_buffer_free(&indices);
    initialized = false;
    baked_font = nullptr;
    open.store(false);
}

void Render()
{
    if (!ImGui::GetCurrentContext()) return;
    Initialize();
    if (!initialized) return;
    Snapshot snapshot;
    float x, y, wheel_x, wheel_y;
    bool down;
    std::vector<ButtonEvent> buttons;
    std::vector<SDL_Keycode> keys;
    {
        std::lock_guard lock(shared.mutex);
        snapshot = shared.snapshot;
        x = shared.x; y = shared.y; down = shared.down;
        wheel_x = std::exchange(shared.wheel_x, 0);
        wheel_y = std::exchange(shared.wheel_y, 0);
        buttons.swap(shared.buttons);
        keys.swap(shared.keys);
    }
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    // Each cabinet owns its own GS rectangle. Draw the tracking border after
    // game presentation, before the menu, including during paused idle frames.
    const auto border = sinden::current;
    if (border.white > 0) {
        auto* draw = ImGui::GetBackgroundDrawList();
        const auto r = border.outer;
        const float t = border.white;
        draw->AddRectFilled({r.x,r.y},{r.z,r.y+t},IM_COL32_WHITE);
        draw->AddRectFilled({r.x,r.w-t},{r.z,r.w},IM_COL32_WHITE);
        draw->AddRectFilled({r.x,r.y+t},{r.x+t,r.w-t},IM_COL32_WHITE);
        draw->AddRectFilled({r.z-t,r.y+t},{r.z,r.w-t},IM_COL32_WHITE);
    }
    const float sx = snapshot.input_width > 0 ? size.x / snapshot.input_width : 1;
    const float sy = snapshot.input_height > 0 ? size.y / snapshot.input_height : 1;
    nk_input_begin(&context);
    nk_input_motion(&context, static_cast<int>(x*sx), static_cast<int>(y*sy));
    for (const auto& event : buttons)
        nk_input_button(&context, NK_BUTTON_LEFT, static_cast<int>(event.x*sx),
            static_cast<int>(event.y*sy), event.down);
    nk_input_button(&context, NK_BUTTON_LEFT, static_cast<int>(x*sx), static_cast<int>(y*sy), down);
    nk_input_scroll(&context, nk_vec2(wheel_x, wheel_y));
    nk_input_end(&context);
    if (!IsOpen() || size.x < 80 || size.y < 80) {
        nk_clear(&context);
        return;
    }

    // Refresh every frame: PCSX2 may rebuild its atlas on DPI changes. Warm the
    // complete ASCII menu alphabet before recording any texture coordinates.
    baked_font = ImGui::GetFontBaked();
    for (unsigned c = 32; c < 127; ++c) baked_font->FindGlyph(static_cast<ImWchar>(c));
    font.height = ImGui::GetFontSize();
    // Menubar and dropdown layout follows DriveClub-Linux's Nuklear menu
    // (runtime-linux/gpu/shim/nk_menu.cpp, GPL-2.0-or-later).
    const float zoom = std::clamp(size.y / 1080.0f, 1.0f, 2.5f);
    const float row = std::max(26.0f * zoom, font.height + 6);
    const char* tabs[] = {"File", "Display", "Performance", "Rendering", "Cabinet / Test"};
    const int counts[] = {2, 4, 5, 12, 8};
    const Action cabinet_actions[] = {Action::CabinetTest,Action::CabinetUp,Action::CabinetDown,
        Action::CabinetLeft,Action::CabinetRight,Action::CabinetEnter,Action::CabinetService,Action::CabinetCoin};
    const char* cabinet_labels[] = {"Toggle original test menu", "Test: Up", "Test: Down",
        "Test: Left", "Test: Right", "Test: Enter / Button 1", "Service credit", "Insert coin"};
    int cabinet_action = -1;
    bool resume = false, quit = false, fullscreen = false, start_twin = false;
    std::vector<std::pair<rendering::Option, int>> changes;
    auto change = [&](rendering::Option option, int direction) {
        const auto& r = snapshot.rendering;
        if (!r.ready || ((option == rendering::Option::CAS || option == rendering::Option::Sharpness) &&
                         !r.cas_supported) || (option == rendering::Option::Sharpness && r.cas == 0)) return;
        changes.emplace_back(option, direction);
    };
    for (auto key : keys) {
        if (key == SDLK_TAB || key == SDLK_UNKNOWN ||
            ((key == SDLK_LEFT || key == SDLK_RIGHT) && selected < 0)) {
            page = (page + (key == SDLK_LEFT || key == SDLK_UNKNOWN ? 4 : 1)) % 5;
            selected = -1;
        } else if (key == SDLK_DOWN) selected = (selected + 1) % counts[page];
        else if (key == SDLK_UP) selected = selected <= 0 ? -1 : selected - 1;
        else if (key == SDLK_HOME) selected = 0;
        else if (key == SDLK_END) selected = counts[page] - 1;
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_LEFT || key == SDLK_RIGHT) {
            if (selected < 0) { selected = 0; continue; }
            const int direction = key == SDLK_LEFT ? -1 : 1;
            if (page == 0 && selected == 0) resume = true;
            if (page == 0 && selected == 1) quit = true;
            if (page == 1 && selected == 0 && !snapshot.twin_mode) fullscreen = true;
            if (page == 1 && selected == 1 && !snapshot.twin_mode && snapshot.twin_supported) start_twin = true;
            if (page == 1 && selected == 2) change(rendering::Option::SindenBorder, direction);
            if (page == 1 && selected == 3) change(rendering::Option::BorderWidth, direction);
            if (page == 2 && selected == 0) change(rendering::Option::VSync, direction);
            if (page == 2 && selected == 1) change(rendering::Option::QueueDepth, direction);
            if (page == 3 && selected >= 0 && selected < 4) {
                const rendering::Option options[] = {rendering::Option::Resolution,
                    rendering::Option::FXAA, rendering::Option::CAS, rendering::Option::Sharpness};
                change(options[selected], direction);
            }
            if (page == 4 && selected >= 0) cabinet_action = selected;
        }
    }
    float tab_x[5], tab_width[5], cursor = 4 * zoom;
    for (int i = 0; i < 5; ++i) {
        tab_x[i] = cursor;
        tab_width[i] = (30 + 9 * std::strlen(tabs[i])) * zoom;
        cursor += tab_width[i] + 4 * zoom;
    }
    const auto bar = nk_rect(0, 0, size.x, row + 8 * zoom);
    if (nk_begin(&context, "menubar", bar, NK_WINDOW_NO_SCROLLBAR)) {
        nk_window_set_bounds(&context, "menubar", bar);
        nk_menubar_begin(&context);
        nk_layout_row_begin(&context, NK_STATIC, row - 4 * zoom, 6);
        for (int i = 0; i < 5; ++i) {
            nk_layout_row_push(&context, tab_width[i]);
            nk_bool active = page == i;
            if (nk_selectable_label(&context, tabs[i], NK_TEXT_CENTERED, &active)) {
                page = i; selected = -1;
            }
        }
        nk_layout_row_push(&context, std::max(1.0f, size.x - cursor - 20));
        nk_label(&context, "Arrows, Enter    Backspace: resume    Esc: close", NK_TEXT_RIGHT);
        nk_layout_row_end(&context);
        nk_menubar_end(&context);
    }
    nk_end(&context);
    const float width = std::min((page == 0 ? 200.0f : 510.0f) * zoom, size.x - 8);
    const float height = std::min(48 * zoom + counts[page] * (row + 4 * zoom) +
                                  (page == 0 ? 0 : 80 * zoom), size.y - row - 12);
    const auto bounds = nk_rect(std::clamp(tab_x[page], 4.0f, size.x - width - 4),
                                row + 10 * zoom, width, height);
    char name[16]; std::snprintf(name, sizeof(name), "drop%d", page);
    if (nk_begin(&context, name, bounds, NK_WINDOW_BORDER)) {
        nk_window_set_bounds(&context, name, bounds);
        struct nk_rect selected_bounds{};
        bool selected_visible = false;
        auto action = [&](const char* label, int i) {
            nk_layout_row_dynamic(&context, row, 1);
            char text[96]; std::snprintf(text, sizeof(text), "%s%s", selected == i ? ">  " : "", label);
            return nk_button_label(&context, text) != 0;
        };
        auto option = [&](const char* label, const char* value, int i, bool enabled) {
            nk_layout_row_template_begin(&context, row);
            nk_layout_row_template_push_static(&context, 240 * zoom);
            nk_layout_row_template_push_static(&context, 24 * zoom);
            nk_layout_row_template_push_dynamic(&context);
            nk_layout_row_template_push_static(&context, 24 * zoom);
            nk_layout_row_template_end(&context);
            if (selected == i) {
                selected_bounds = nk_widget_bounds(&context);
                selected_visible = true;
            }
            nk_bool active = selected == i;
            if (nk_selectable_label(&context, label, NK_TEXT_LEFT, &active)) selected = i;
            if (!enabled) nk_widget_disable_begin(&context);
            int direction = nk_button_symbol(&context, NK_SYMBOL_TRIANGLE_LEFT) ? -1 : 0;
            if (nk_button_label(&context, value)) direction = 1;
            if (nk_button_symbol(&context, NK_SYMBOL_TRIANGLE_RIGHT)) direction = 1;
            if (!enabled) nk_widget_disable_end(&context);
            if (enabled && direction) selected = i;
            return enabled ? direction : 0;
        };
        const char* note = "";
        if (page == 0) {
            resume |= action("Resume", 0);
            quit |= action("Quit game", 1);
        } else if (page == 1) {
            fullscreen |= option("Window mode", snapshot.twin_mode ? "Arcade pair" :
                                  snapshot.fullscreen ? "Fullscreen" : "Windowed", 0, !snapshot.twin_mode);
            start_twin |= option("Twin arcade screens", snapshot.twin_mode ? "On" :
                                 snapshot.twin_supported ? "Start twin screens" : "Unavailable", 1,
                                 !snapshot.twin_mode && snapshot.twin_supported);
            const char* border_modes[] = {"Off", "Auto detect", "On"};
            int direction;
            if ((direction = option("Sinden white border", border_modes[std::clamp(snapshot.rendering.sinden_border,0,2)],
                                    2,snapshot.rendering.ready)))
                change(rendering::Option::SindenBorder,direction);
            char border_width[24]; std::snprintf(border_width,sizeof(border_width),"%d%%",snapshot.rendering.border_width);
            if ((direction = option("Border width",border_width,3,snapshot.rendering.ready)))
                change(rendering::Option::BorderWidth,direction);
            char devices[96]; std::snprintf(devices,sizeof(devices),"Sinden USB devices: %d (driver must run separately)",snapshot.sinden_devices);
            nk_layout_row_dynamic(&context,row,1); nk_label(&context,devices,NK_TEXT_LEFT);
            note = snapshot.twin_mode ? "Linked left and right cabinets. The menu pauses both screens." :
                   snapshot.twin_supported ? "Start twin screens restarts into the linked arcade pair." :
                   "Linked cabinet support has not been implemented for this game.";
            if (selected >= 2) note = "Auto detects Sinden USB hardware. A border surrounds each game view. Install and start the official Linux driver separately.";
        } else if (page == 4) {
            for (int i=0; i<8; ++i) if (action(cabinet_labels[i],i)) cabinet_action=i;
            note = "Controls the original cabinet test system on this screen. Each action resumes the game; F10 reopens this menu. Use the original gun, sound and link tests for maintenance.";
        } else {
            const auto& r = snapshot.rendering;
            const char* detail = "";
            const int offset = page == 2 ? 2 : 4;
            int direction;
            if (page == 2) {
                if ((direction = option("VSync", r.vsync ? "On" : "Off", 0, r.ready)))
                    change(rendering::Option::VSync, direction);
                char queue[32]; std::snprintf(queue, sizeof(queue), "%d frame%s", r.queue_depth,
                                              r.queue_depth == 1 ? "" : "s");
                if ((direction = option("Presentation queue", queue, 1, r.ready)))
                    change(rendering::Option::QueueDepth, direction);
                detail = selected == 0 ? "Synchronize presentation to the display refresh." :
                         "Lower queues reduce latency; larger queues allow more overlap.";
            } else {
                char scale[32]; std::snprintf(scale, sizeof(scale), "%dx native", r.resolution);
                if ((direction = option("Internal resolution", scale, 0, r.ready)))
                    change(rendering::Option::Resolution, direction);
                if ((direction = option("FXAA", r.fxaa ? "On" : "Off", 1, r.ready)))
                    change(rendering::Option::FXAA, direction);
                const char* modes[] = {"Off", "Sharpen", "Sharpen + resize"};
                if ((direction = option("CAS sharpening", modes[std::clamp(r.cas, 0, 2)], 2,
                                        r.ready && r.cas_supported)))
                    change(rendering::Option::CAS, direction);
                char sharp[32]; std::snprintf(sharp, sizeof(sharp), "%d%%", r.sharpness);
                if ((direction = option("Sharpness", sharp, 3, r.ready && r.cas_supported && r.cas != 0)))
                    change(rendering::Option::Sharpness, direction);
                const char* notes[] = {"Updates the scene briefly at the new resolution, then pauses again.",
                    "Smooth edges with the active Vulkan FXAA pass.",
                    "Contrast Adaptive Sharpening; resize mode also scales the final image.",
                    "Adjust CAS strength. Enable CAS sharpening first."};
                detail = selected >= 0 && selected < 4 ? notes[selected] :
                         "Changes preview immediately on both screens and are saved automatically.";
            }
            const int first = page == 2 ? 8 : 0;
            const int last = page == 2 ? 11 : 8;
            for (int i = first; i < last; ++i) {
                const auto& cap = rendering::capabilities[i];
                option(cap.name, "Unavailable", i - first + offset, false);
            }
            note = selected >= offset ? rendering::capabilities[first + selected - offset].reason : detail;
        }
        if (page != 0) {
            nk_layout_row_dynamic(&context, 60 * zoom, 1);
            nk_label_wrap(&context, note);
            if (page == 2 && snapshot.valid) {
                char rate[96];
                std::snprintf(rate, sizeof(rate), "Game %.2f FPS | Video %.2f Hz | Speed %.1f%%",
                              snapshot.game_fps, snapshot.video_fps, snapshot.speed_percent);
                nk_layout_row_dynamic(&context, row, 1);
                nk_label(&context, rate, NK_TEXT_LEFT);
            }
            // Keep a row reached with the keyboard visible in short windows.
            if (selected_visible && !keys.empty()) {
                const auto clip = nk_window_get_content_region(&context);
                nk_uint scroll_x, scroll_y;
                nk_window_get_scroll(&context, &scroll_x, &scroll_y);
                if (selected_bounds.y < clip.y)
                    scroll_y = static_cast<nk_uint>(std::max(0.0f,
                        float(scroll_y) + selected_bounds.y - clip.y));
                else if (selected_bounds.y + selected_bounds.h > clip.y + clip.h)
                    scroll_y += static_cast<nk_uint>(selected_bounds.y + selected_bounds.h - clip.y - clip.h);
                nk_window_set_scroll(&context, scroll_x, scroll_y);
            }
        }
    }
    nk_end(&context);
    AppendGeometry();
    nk_clear(&context);
    nk_buffer_clear(&commands);
    nk_buffer_clear(&vertices);
    nk_buffer_clear(&indices);
    std::function<void(rendering::Option, int)> option_callback;
    { std::lock_guard lock(shared.mutex); option_callback = shared.option_callback; }
    if (option_callback)
        for (const auto& [option, direction] : changes) option_callback(option, direction);
    if (fullscreen) Dispatch(Action::ToggleFullscreen);
    if (start_twin) Dispatch(Action::StartTwin);
    if (cabinet_action >= 0) {
        Dispatch(cabinet_actions[cabinet_action]);
        SetOpen(false);
    }
    if (quit) Dispatch(Action::Quit);
    else if (resume) SetOpen(false);
}
} // namespace tc3::menu
