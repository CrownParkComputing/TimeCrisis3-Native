// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace tc3::sinden {
inline std::atomic<int> detected{0}, mode{1}, width_percent{2};
struct Rect { float x, y, z, w; };
struct Layout { Rect outer, content; float white; };
// GS thread only. Input continues to use GSRenderer::GetLastDrawRect(), which
// records this same inset content rectangle in both active and paused presents.
inline Layout current{};
inline bool active() { return mode.load() == 2 || (mode.load() == 1 && detected.load() > 0); }
inline Layout layout(Rect r, int percent, bool enabled) {
    const float shorter = std::min(r.z-r.x, r.w-r.y);
    if (!enabled || shorter < 32) return {r, r, 0};
    const float white = std::max(1.f, std::floor(shorter * std::clamp(percent, 1, 5) / 100.f));
    const float scale = 1.f - 2.f * (white + std::max(1.f, white/3)) / shorter;
    const float dx = (r.z-r.x) * (1-scale)/2, dy = (r.w-r.y) * (1-scale)/2;
    return {r, {r.x+dx, r.y+dy, r.z-dx, r.w-dy}, white};
}
template<class Vector> Vector inset(Vector r) {
    current = layout({r.x,r.y,r.z,r.w}, width_percent.load(), active());
    const auto c=current.content;
    return Vector(c.x,c.y,c.z,c.w);
}
inline std::string read(const std::filesystem::path& p) {
    std::ifstream f(p); std::string s; std::getline(f,s); return s;
}
inline bool matches(std::string product, const std::string& vendor, const std::string& id) {
    std::transform(product.begin(),product.end(),product.begin(),[](unsigned char c){return std::tolower(c);});
    // VID/PID published in the manufacturer's Linux 99-sinden-lightgun.rules.
    return product.find("sinden") != std::string::npos || (vendor=="32e4" && id=="9210");
}
inline int scan(const std::filesystem::path& root="/sys/bus/usb/devices") {
    int count=0; std::error_code error;
    for (std::filesystem::directory_iterator it(root,error), end; !error && it!=end; it.increment(error)) {
        const auto p=it->path();
        // USB interfaces lack idVendor. Count physical devices once, not each
        // camera/HID/serial interface belonging to the same gun.
        const auto vendor=read(p/"idVendor");
        if (!vendor.empty() && matches(read(p/"product"),vendor,read(p/"idProduct"))) ++count;
    }
    return count;
}
} // namespace tc3::sinden
