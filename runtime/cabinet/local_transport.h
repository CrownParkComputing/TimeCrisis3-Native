// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "packet.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace tc3::cabinet {
// One endpoint per core process. All calls are nonblocking: an empty receive
// stays pending in the future guest adapter, rather than blocking EE execution.
// This class does not fabricate peer presence, acknowledgements, or game frames.
class LocalTransport {
public:
    struct Received {
        std::optional<Packet> packet;
        std::error_code error;
    };
    LocalTransport(const std::filesystem::path& local, const std::filesystem::path& peer)
        : path_(local.string()), peer_(make_address(peer.string())) {
        auto address=make_address(path_);
        fd_=::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if(fd_<0) throw std::system_error(errno,std::generic_category(),"cabinet socket");
        if(::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address))<0) {
            auto failure=errno; ::close(fd_); fd_=-1;
            throw std::system_error(failure,std::generic_category(),"cabinet bind");
        }
        if(::lstat(path_.c_str(), &owned_)<0) {
            auto failure=errno; ::close(fd_); fd_=-1; ::unlink(path_.c_str());
            throw std::system_error(failure,std::generic_category(),"cabinet endpoint identity");
        }
    }
    LocalTransport(const LocalTransport&)=delete;
    LocalTransport& operator=(const LocalTransport&)=delete;
    ~LocalTransport() {
        if(fd_>=0) ::close(fd_);
        struct stat now{};
        if(::lstat(path_.c_str(), &now)==0 && now.st_dev==owned_.st_dev && now.st_ino==owned_.st_ino)
            ::unlink(path_.c_str());
    }
    int descriptor() const {return fd_;}
    std::error_code send(const Packet& packet) {
        auto bytes=encode_packet(packet);
        if(!bytes) return std::make_error_code(std::errc::message_size);
        auto sent=::sendto(fd_, bytes->data(), bytes->size(), MSG_NOSIGNAL,
                          reinterpret_cast<const sockaddr*>(&peer_), sizeof(peer_));
        if(sent<0) return {errno,std::generic_category()};
        if(static_cast<std::size_t>(sent)!=bytes->size())
            return std::make_error_code(std::errc::io_error);
        return {};
    }
    Received receive() {
        std::array<std::uint8_t,header_size+default_max_payload> bytes{};
        sockaddr_un sender{};
        iovec vector{bytes.data(), bytes.size()};
        msghdr message{};
        message.msg_name=&sender; message.msg_namelen=sizeof(sender);
        message.msg_iov=&vector; message.msg_iovlen=1;
        auto got=::recvmsg(fd_,&message,MSG_DONTWAIT | MSG_TRUNC);
        if(got<0) return {{},{errno,std::generic_category()}};
        if(message.msg_namelen>sizeof(sender) || sender.sun_family!=AF_UNIX ||
           std::memcmp(sender.sun_path,peer_.sun_path,sizeof(sender.sun_path))!=0)
            return {{},std::make_error_code(std::errc::permission_denied)};
        if((message.msg_flags&MSG_TRUNC) || static_cast<std::size_t>(got)>bytes.size())
            return {{},std::make_error_code(std::errc::message_size)};
        auto packet=decode_packet({bytes.data(),static_cast<std::size_t>(got)});
        if(!packet) return {{},std::make_error_code(std::errc::bad_message)};
        return {std::move(packet),{}};
    }
private:
    static sockaddr_un make_address(const std::string& path) {
        sockaddr_un address{}; address.sun_family=AF_UNIX;
        if(path.empty() || path.size()>=sizeof(address.sun_path) || path.find('\0')!=std::string::npos)
            throw std::invalid_argument("Cabinet endpoint path is empty or too long");
        std::memcpy(address.sun_path,path.c_str(),path.size()+1);
        return address;
    }
    std::string path_;
    sockaddr_un peer_{};
    int fd_=-1;
    struct stat owned_{};
};
} // namespace tc3::cabinet
