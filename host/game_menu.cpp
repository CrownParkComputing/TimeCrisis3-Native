// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_menu.h"
#include "service_options.h"
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
    std::function<void(const int*, int)> service_apply_callback;
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

} // namespace
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
namespace {

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

void SetServiceApplyCallback(std::function<void(const int* targets, int count)> callback)
{
    std::lock_guard lock(shared.mutex);
    shared.service_apply_callback = std::move(callback);
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
    // Custom splash shown while the ROM still loads in the background: the
    // GS presents long before the game produces its first frame. Each cabinet
    // worker draws its own copy, tagged with its link side. The linked
    // cabinet also shows it over the test menu — settings live on the
    // primary screen only.
    const bool serviceCover = snapshot.test_mode && snapshot.cabinet_role == 1;
    if ((snapshot.loading || serviceCover) && !IsOpen() && size.x >= 80 && size.y >= 80) {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.015f, 0.02f, 0.05f, 1.0f));
        if (ImGui::Begin("##boot_splash", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing)) {
            const float cx = size.x * 0.5f;
            auto* draw = ImGui::GetWindowDrawList();
            ImFont* fnt = ImGui::GetFont();
            auto centered = [&](const char* text, float y, ImU32 color, float px) {
                const ImVec2 w = fnt->CalcTextSizeA(px, FLT_MAX, 0, text);
                draw->AddText(fnt, px, {cx - w.x * 0.5f, y}, color, text);
            };
            const float big = std::clamp(size.y * 0.09f, 24.0f, 96.0f);
            const float mid = std::clamp(size.y * 0.034f, 15.0f, 34.0f);
            const float sml = std::clamp(size.y * 0.024f, 13.0f, 24.0f);
            const ImU32 accent = snapshot.is_tc4 ? IM_COL32(255, 40, 40, 255)
                                                 : IM_COL32(60, 160, 255, 255);
            centered(snapshot.is_tc4 ? "TIME CRISIS 4" : "TIME CRISIS 3",
                     size.y * 0.30f, accent, big);
            centered(snapshot.is_tc4 ? "NAMCO SYSTEM 256" : "NAMCO SYSTEM 246",
                     size.y * 0.30f + big * 1.3f,
                     IM_COL32(200, 200, 210, 255), mid);
            const char* status_text = serviceCover ? "SERVICE MODE (LINKED STANDBY)"
                                                   : (snapshot.load_stage[0] ? snapshot.load_stage
                                                                             : "Initializing...");
            centered(status_text, size.y * 0.58f, IM_COL32(255, 220, 90, 255), sml);
            // Live stage progress bar (or full bar in service standby).
            const float bw = std::min(size.x * 0.46f, 560.0f), bh = 12.0f;
            const float bx = cx - bw * 0.5f, by = size.y * 0.68f;
            draw->AddRect({bx, by}, {bx + bw, by + bh}, IM_COL32(120, 120, 140, 255));
            const float fill = serviceCover ? 1.0f : std::clamp(snapshot.load_progress, 0.05f, 1.0f);
            draw->AddRectFilled({bx + 2, by + 2},
                                {bx + 2 + (bw - 4) * fill, by + bh - 2},
                                accent);
            if (snapshot.cabinet_role >= 0)
                centered(snapshot.cabinet_role == 0 ? "CABINET 1P (LEFT)" : "CABINET 2P (RIGHT)",
                         size.y * 0.86f, IM_COL32(140, 160, 255, 255), mid);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        nk_clear(&context);
        return;
    }
    // Controls cheat-sheet popout shown whenever the original cabinet test
    // menu is on screen, independent of the Nuklear menu being open. On the
    // twin cabinet it stays on the primary screen only — the linked display
    // shows the clean menu.
    if (snapshot.test_mode && snapshot.cabinet_role != 1) {
        const float w = 330.0f, h = 250.0f;
        ImGui::SetNextWindowPos(ImVec2(size.x - w - 18, 18), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.82f);
        if (ImGui::Begin("##cabinet_help", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::TextUnformatted("CABINET TEST MENU");
            ImGui::Separator();
            ImGui::BulletText("Move cursor:      Arrow Up / Down");
            ImGui::BulletText("Adjust value:     Arrow Left / Right");
            ImGui::BulletText("Select / confirm: E");
            ImGui::BulletText("Start:            Return");
            ImGui::BulletText("Service credit:   9");
            ImGui::BulletText("Insert coin:      5");
            ImGui::BulletText("Leave test menu:  F9 or F10 > Cabinet");
            ImGui::Spacing();
            ImGui::TextDisabled("Set options directly in F10 > Cabinet.");
        }
        ImGui::End();
    }
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
    constexpr int kTabCount = 3;
    const char* tabs[] = {"File", "Display & Rendering", "Cabinet / Service"};
    const auto* service_opts = snapshot.is_ridge ? tc3::service::kRidgeOptions
                                                 : tc3::service::kOptions;
    const int service_opt_count = snapshot.is_ridge ? tc3::service::kRidgeOptionCount
                                                    : tc3::service::kOptionCount;
    const int service_count = snapshot.service_supported ? service_opt_count : 0;
    const int has_save = service_count > 0 ? 1 : 0;
    // Driving cabinets have no lightgun: the Sinden border rows do not exist
    // on their Display & Rendering page.
    const int counts[] = {2, snapshot.has_sinden ? 11 : 9, service_count + has_save + 2};
    if (page >= kTabCount) page = 0;
    const Action cabinet_actions[] = {Action::CabinetService, Action::CabinetCoin};
    const char* cabinet_labels[] = {"Service credit (9)", "Insert coin (5)"};
    static int pending[tc3::service::kMaxOptionCount]; // -2 = untouched
    static int pending_page = -1;
    if (page != 2) pending_page = -1;
    else if (pending_page != 2) {
        pending_page = 2;
        for (int i = 0; i < service_count; ++i) pending[i] = -2;
    }
    int cabinet_action = -1;
    bool service_apply = false, twin_default_toggle = false;
    bool resume = false, quit = false, fullscreen = false, start_twin = false;
    std::vector<std::pair<rendering::Option, int>> changes;
    auto change = [&](rendering::Option option, int direction) {
        const auto& r = snapshot.rendering;
        if (!r.ready) return;
        if (option == rendering::Option::DLSS && !r.dlss_supported) return;
        changes.emplace_back(option, direction);
    };
    for (auto key : keys) {
        if (key == SDLK_TAB || key == SDLK_UNKNOWN ||
            ((key == SDLK_LEFT || key == SDLK_RIGHT) && selected < 0)) {
            page = (page + (key == SDLK_LEFT || key == SDLK_UNKNOWN ? kTabCount - 1 : 1)) % kTabCount;
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
            if (page == 1) {
                if (selected == 0 && !snapshot.twin_mode) fullscreen = true;
                else if (selected == 1 && !snapshot.twin_mode && snapshot.twin_supported) start_twin = true;
                else if (selected == 2 && snapshot.twin_supported) twin_default_toggle = true;
                else if (selected == 3) change(rendering::Option::DisplaySize, direction);
                else if (selected == 4) change(rendering::Option::Widescreen, direction);
                else if (selected == 5) change(rendering::Option::Resolution, direction);
                else if (selected == 6) change(rendering::Option::DLSS, direction);
                else if (selected == 7) change(rendering::Option::FrameSkip, direction);
                else if (selected == 8) change(rendering::Option::VSync, direction);
                else if (selected == 9) change(rendering::Option::SindenBorder, direction);
                else if (selected == 10) change(rendering::Option::BorderWidth, direction);
            }
            if (page == 2 && selected >= 0) {
                if (selected < service_count) {
                    int& slot = pending[selected];
                    if (slot == -2) slot = snapshot.service_values[selected];
                    const int n = service_opts[selected].num_values;
                    slot = ((slot < 0 ? 0 : slot) + direction + n) % n;
                } else if (has_save && selected == service_count) service_apply = true;
                else if (selected < service_count + has_save + 2)
                    cabinet_action = selected - service_count - has_save;
            }
        }
    }
    float tab_x[kTabCount], tab_width[kTabCount], cursor = 4 * zoom;
    for (int i = 0; i < kTabCount; ++i) {
        tab_x[i] = cursor;
        tab_width[i] = (30 + 9 * std::strlen(tabs[i])) * zoom;
        cursor += tab_width[i] + 4 * zoom;
    }
    const auto bar = nk_rect(0, 0, size.x, row + 8 * zoom);
    if (nk_begin(&context, "menubar", bar, NK_WINDOW_NO_SCROLLBAR)) {
        nk_window_set_bounds(&context, "menubar", bar);
        nk_menubar_begin(&context);
        nk_layout_row_begin(&context, NK_STATIC, row - 4 * zoom, kTabCount + 1);
        for (int i = 0; i < kTabCount; ++i) {
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
    const float width = std::min((page == 0 ? 200.0f : 530.0f) * zoom, size.x - 8);
    const float height = std::min(48 * zoom + counts[page] * (row + 4 * zoom) +
                                  (page == 0 ? 0 : 90 * zoom), size.y - row - 12);
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
            const auto& r = snapshot.rendering;
            int direction;
            fullscreen |= option("Window mode", snapshot.twin_mode ? "Arcade pair (Borderless)" :
                                  snapshot.fullscreen ? "Fullscreen" : "Windowed", 0, !snapshot.twin_mode);
            start_twin |= option("Twin arcade screens", snapshot.twin_mode ? "Linked (Active)" :
                                 snapshot.twin_supported ? "Start twin screens" : "Unavailable", 1,
                                 !snapshot.twin_mode && snapshot.twin_supported);
            if (option("Linked cabinet at boot", snapshot.twin_default ? "On" : "Off",
                       2, snapshot.twin_supported))
                twin_default_toggle = true;
            const char* size_names[] = {"Fit window", "960x720", "1280x960", "1920x1440"};
            if ((direction = option("Display size", size_names[std::clamp(r.display_size, 0, 3)],
                                    3, r.ready && !snapshot.twin_mode)))
                change(rendering::Option::DisplaySize, direction);
            const char* aspect_names[] = {"4:3 (Cabinet)", "16:9 (Widescreen)", "Stretch fill"};
            if ((direction = option("Aspect ratio", aspect_names[std::clamp(r.aspect, 0, 2)],
                                    4, r.ready)))
                change(rendering::Option::Widescreen, direction);
            char scale[32]; std::snprintf(scale, sizeof(scale), "%dx native", r.resolution);
            if ((direction = option("Resolution scale", scale, 5, r.ready)))
                change(rendering::Option::Resolution, direction);
            const char* dlss_modes[] = {"Off", "Quality", "Balanced", "Performance"};
            if ((direction = option("NVIDIA DLSS", r.dlss_supported ?
                                    dlss_modes[std::clamp(r.dlss, 0, 3)] : "Unsupported GPU",
                                    6, r.ready && r.dlss_supported)))
                change(rendering::Option::DLSS, direction);
            const char* fs_names[] = {"Off (60 fps)", "1/2 (30 fps)", "1/3 (20 fps)", "1/4 (15 fps)"};
            if ((direction = option("Frame skip", fs_names[std::clamp(r.frame_skip, 0, 3)], 7, r.ready)))
                change(rendering::Option::FrameSkip, direction);
            if ((direction = option("VSync", r.vsync ? "On" : "Off", 8, r.ready)))
                change(rendering::Option::VSync, direction);
            if (snapshot.has_sinden) {
                const char* border_modes[] = {"Off", "Auto detect", "On"};
                if ((direction = option("Sinden white border", border_modes[std::clamp(r.sinden_border, 0, 2)],
                                        9, r.ready)))
                    change(rendering::Option::SindenBorder, direction);
                char border_width[24]; std::snprintf(border_width, sizeof(border_width), "%d%%", r.border_width);
                if ((direction = option("Border width", border_width, 10, r.ready)))
                    change(rendering::Option::BorderWidth, direction);
            }
            note = "Display & rendering settings apply immediately across both linked cabinet screens.";
            if (selected == 2) note = "Boot as a linked left/right cabinet pair by default, like the arcade machine.";
            else if (selected == 3) note = "Fixed window resolution when running single-screen windowed, or Fit to fill.";
            else if (selected == 4) note = "4:3 original arcade aspect, 16:9 widescreen, or full-panel stretch.";
            else if (selected == 5) note = "Internal 3D rendering multiplier (1x = native PS2/System 256, 4x = 4K-class).";
            else if (selected == 6) note = r.dlss_supported ? "NVIDIA RTX GPU detected; selects the DLSS upscaler preset."
                                                            : "Requires a compatible NVIDIA RTX GPU.";
            else if (selected == 7) note = "Present every Nth frame while keeping full-speed 60 Hz game simulation.";
            else if (selected >= 9) note = "White reference border for Sinden lightgun camera tracking.";
        } else if (page == 2) {
            for (int i = 0; i < service_count; ++i) {
                const auto& opt = service_opts[i];
                const int shown = pending[i] != -2 ? pending[i] : snapshot.service_values[i];
                const char* value = shown >= 0 && shown < opt.num_values ? opt.values[shown] : "?";
                char label[64];
                std::snprintf(label, sizeof(label), "%s%s", pending[i] != -2 ? "* " : "", opt.label);
                int direction;
                if ((direction = option(label, value, i, true))) {
                    int& slot = pending[i];
                    if (slot == -2) slot = snapshot.service_values[i];
                    slot = ((slot < 0 ? 0 : slot) + direction + opt.num_values) % opt.num_values;
                }
            }
            if (service_count) {
                if (action("Save & Reset Machine", service_count)) service_apply = true;
            }
            for (int i = 0; i < 2; ++i)
                if (action(cabinet_labels[i], service_count + has_save + i)) cabinet_action = i;
            if (snapshot.service_state == 2 || snapshot.service_state == 3)
                note = snapshot.service_note;
            else
                note = snapshot.is_ridge
                    ? "Writes directly to cabinet EEPROM (FCAI record checksum) and resets both linked screens together so they pick up the same settings."
                    : snapshot.is_tc4
                    ? "Writes directly to cabinet EEPROM with a valid TC4 CRC16 checksum and resets both linked screens together so they pick up the same settings."
                    : "Writes directly to cabinet EEPROM (TST-03A checksum) and resets both linked screens together so they pick up the same settings.";
        }
        if (page != 0) {
            nk_layout_row_dynamic(&context, 54 * zoom, 1);
            nk_label_wrap(&context, note);
            if (page == 1 && snapshot.valid) {
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
    if (twin_default_toggle) Dispatch(Action::ToggleTwinDefault);
    if (service_apply) {
        int targets[tc3::service::kMaxOptionCount];
        for (int i = 0; i < service_opt_count; ++i)
            targets[i] = pending[i] == -2 ? -1 : pending[i];
        std::function<void(const int*, int)> apply_callback;
        { std::lock_guard lock(shared.mutex); apply_callback = shared.service_apply_callback; }
        if (apply_callback) apply_callback(targets, service_opt_count);
        for (int i = 0; i < tc3::service::kMaxOptionCount; ++i) pending[i] = -2;
        SetOpen(false);
    }
    if (cabinet_action >= 0) {
        Dispatch(cabinet_actions[cabinet_action]);
        SetOpen(false);
    }
    if (quit) Dispatch(Action::Quit);
    else if (resume) SetOpen(false);
}
} // namespace tc3::menu
