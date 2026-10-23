// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/cabinet/local_transport.h"
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <sys/wait.h>
using namespace tc3::cabinet;
static void require(bool value,const char* why) {
    if(!value) {std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}
}
int main() {
    Packet packet{{{0x12,0x34},{1,2,3,4,5,6,7,8}},{{0xab,0xcd},{8,7,6,5,4,3,2,1}},{0,0xff,0x20,0x30}};
    auto bytes=encode_packet(packet);
    std::vector<std::uint8_t> expected{1,2,3,4,5,6,7,8,0x12,0x34,0xab,0xcd,8,7,6,5,4,3,2,1,0,0xff,0x20,0x30};
    require(bytes && *bytes==expected,"original ILSOCK envelope bytes");
    require(decode_packet(expected)==packet,"packet roundtrip");
    require(!decode_packet(std::span<const std::uint8_t>(expected).first(19)),"truncated header rejected");
    auto address=encode_address(packet.source);
    require(decode_address(address)==packet.source,"12-byte sockaddr preserved");
    address[1]=2; require(!decode_address(address),"non-ILSOCK sockaddr rejected");
    auto max=packet;max.payload.resize(default_max_payload);
    require(encode_packet(max)->size()==512,"largest default packet accepted");
    max.payload.push_back(0);require(!encode_packet(max),"oversized payload rejected");
    char temp[]="/tmp/tc3-cabinet-test-XXXXXX";
    require(::mkdtemp(temp)!=nullptr,"private temporary directory");
    auto left=std::filesystem::path(temp)/"left", right=std::filesystem::path(temp)/"right";
    {
        LocalTransport a(left,right);
        require(a.receive().error==std::errc::resource_unavailable_try_again,"empty receive stays pending");
        require(bool(a.send(packet)),"missing peer cannot report success");
        bool duplicate=false;
        try {LocalTransport collision(left,right);} catch(const std::system_error& e) {duplicate=e.code()==std::errc::address_in_use;}
        require(duplicate,"duplicate endpoint does not overwrite existing cabinet");
        pid_t pid=fork();require(pid>=0,"fork second process");
        if(pid==0) {
            int result=0;
            {
                LocalTransport b(right,left);
                pollfd wait{b.descriptor(),POLLIN,0};
                if(::poll(&wait,1,3000)!=1) result=1;
                auto received=b.receive();
                if(received.error || received.packet!=packet) result=2;
                if(b.send(packet)) result=3;
            }
            _exit(result);
        }
        // Wait boundedly for the peer to bind, then send one real datagram.
        std::error_code sent;
        for(int i=0;i<300;++i) {sent=a.send(packet);if(!sent)break;::usleep(10000);}
        require(!sent,"send to second process");
        pollfd wait{a.descriptor(),POLLIN,0};
        require(::poll(&wait,1,3000)==1,"peer reply arrives");
        auto reply=a.receive();require(!reply.error && reply.packet==packet,"opaque packet exchanged across processes");
        int status=0;require(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,"peer process completed");
        require(a.receive().error==std::errc::resource_unavailable_try_again,"packet delivered exactly once");
        require(bool(a.send(packet)),"disconnected peer cannot report success");
    }
    require(!std::filesystem::exists(left)&&!std::filesystem::exists(right),"owned endpoints cleaned up");
    std::filesystem::remove(temp);
    std::puts("PASS original ILSOCK codec; nonblocking two-process transport; absent/disconnected peer; duplicate endpoint; payload limits");
}
