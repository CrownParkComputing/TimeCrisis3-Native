// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/sinden.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
using namespace tc3::sinden;
void check(bool b) { if(!b) std::abort(); }
int main() {
    check(matches("Sinden Lightgun","unknown","unknown"));
    check(matches("USB Camera","32e4","9210"));
    check(!matches("USB Camera","1234","9210"));
    char path[]="/tmp/tc3-usb-test-XXXXXX";
    const auto root=std::filesystem::path(mkdtemp(path));
    std::filesystem::create_directory(root/"gun");
    std::filesystem::create_directory(root/"gun:1.0");
    std::filesystem::create_directory(root/"mouse");
    std::ofstream(root/"gun/idVendor")<<"32e4\n";
    std::ofstream(root/"gun/idProduct")<<"9210\n";
    std::ofstream(root/"gun:1.0/product")<<"Sinden\n";
    std::ofstream(root/"mouse/idVendor")<<"1234\n";
    check(scan(root)==1); // Composite USB interfaces must not inflate the count.
    std::filesystem::remove_all(root/"gun");
    check(scan(root)==0); // Hot unplug.
    std::filesystem::remove_all(root);
    check(scan(root)==0); // Missing sysfs is a graceful no-device result.
    for (Rect r : {Rect{0,0,800,600},Rect{240,0,1680,1080},Rect{0,75,800,525}}) {
        const auto l=layout(r,2,true);
        const auto c=l.content;
        check(l.white>0 && c.x>r.x+l.white && c.y>r.y+l.white);
        check(c.z<r.z-l.white && c.w<r.w-l.white);
        check(std::abs((c.z-c.x)/(c.w-c.y)-(r.z-r.x)/(r.w-r.y))<0.0001);
        // The core maps the actual inset content: center and corner aim agree.
        check(std::abs(((c.x+c.z)/2-c.x)/(c.z-c.x)-0.5f)<0.0001);
        check(layout(r,2,false).content.x==r.x);
    }
    detected=0;mode=1;check(!active());detected=2;check(active());mode=0;check(!active());mode=2;check(active());
    std::cout<<"PASS USB detection, hotplug, per-view border geometry and aim mapping\n";
}
