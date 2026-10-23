// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tc3::cabinet {
// Supervisor-created format: five little-endian u32s, offsets 0/4/8/12/16.
// Python initializes magic and four zero words before launching either worker.
// Only the supervisor creates/unlinks the file. Workers atomically own one bit.
class SharedControl {
public:
    static constexpr std::uint32_t magic=0x54433301;
    static constexpr std::size_t file_bytes=20;
    static_assert(std::endian::native==std::endian::little);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic_ref<std::uint32_t>::required_alignment<=4);

    explicit SharedControl(const std::filesystem::path& path,std::uint32_t role_mask)
        : role_(role_mask) {
        if(role_!=1 && role_!=2) throw std::invalid_argument("Cabinet control role must be 1 or 2");
        const int descriptor=open_regular(path);
        void* view=::mmap(nullptr,file_bytes,PROT_READ|PROT_WRITE,MAP_SHARED,descriptor,0);
        const auto failure=errno;
        ::close(descriptor);
        if(view==MAP_FAILED) throw std::system_error(failure,std::generic_category(),"map cabinet control");
        words_=static_cast<std::uint32_t*>(view);
        if(words_[0]!=magic || (word(1).load(std::memory_order_acquire)&~3U)!=0 ||
           (word(2).load(std::memory_order_acquire)&~3U)!=0 ||
           word(3).load(std::memory_order_acquire)>1) {
            ::munmap(words_,file_bytes);words_=nullptr;
            throw std::invalid_argument("Invalid cabinet control header");
        }
        const auto previous=word(1).fetch_or(role_,std::memory_order_acq_rel);
        if(previous&role_) {
            // The bit belongs to another live worker. Do not clear its state.
            ::munmap(words_,file_bytes);words_=nullptr;
            throw std::system_error(std::make_error_code(std::errc::device_or_resource_busy),"cabinet role already registered");
        }
    }
    SharedControl(const SharedControl&)=delete;
    SharedControl& operator=(const SharedControl&)=delete;
    SharedControl(SharedControl&&)=delete;
    SharedControl& operator=(SharedControl&&)=delete;
    ~SharedControl() noexcept {
        if(!words_) return;
        word(2).fetch_and(~role_,std::memory_order_acq_rel);
        word(1).fetch_and(~role_,std::memory_order_acq_rel);
        ::munmap(words_,file_bytes);
    }
    void set_paused(bool value) {
        if(value) word(2).fetch_or(role_,std::memory_order_acq_rel);
        else word(2).fetch_and(~role_,std::memory_order_acq_rel);
    }
    bool paused() const {return word(2).load(std::memory_order_acquire)!=0;}
    std::uint32_t pause_mask() const {return word(2).load(std::memory_order_acquire);}
    std::uint32_t worker_mask() const {return word(1).load(std::memory_order_acquire);}
    void request_quit() {word(3).store(1,std::memory_order_release);}
    bool quitting() const {return word(3).load(std::memory_order_acquire)!=0;}
    // The host owns packing and validation. Publishing one complete word avoids
    // exposing combinations of old/new resolution and post-process settings.
    void set_rendering_bits(std::uint32_t value) {word(4).store(value,std::memory_order_release);}
    std::uint32_t rendering_bits() const {return word(4).load(std::memory_order_acquire);}
private:
    std::atomic_ref<std::uint32_t> word(std::size_t index) const {
        return std::atomic_ref<std::uint32_t>(words_[index]);
    }
    static int open_regular(const std::filesystem::path& path) {
        if(path.empty() || path.filename().empty()) throw std::invalid_argument("Empty cabinet control path");
        // Reject symlinks in every component, including ancestors. O_NONBLOCK
        // keeps an unexpected FIFO from blocking before fstat can reject it.
        int directory=::open(path.is_absolute()?"/":".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(directory<0) throw std::system_error(errno,std::generic_category(),"open cabinet control directory");
        try {
            for(const auto& component:path.parent_path().relative_path()) {
                if(component==".") continue;
                if(component=="..") throw std::invalid_argument("Parent traversal in cabinet control path");
                int next=::openat(directory,component.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
                if(next<0) throw std::system_error(errno,std::generic_category(),"open cabinet control ancestor");
                ::close(directory);directory=next;
            }
            const int descriptor=::openat(directory,path.filename().c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
            if(descriptor<0) throw std::system_error(errno,std::generic_category(),"open cabinet control file");
            struct stat information{};
            if(::fstat(descriptor,&information)<0) {
                auto failure=errno;::close(descriptor);
                throw std::system_error(failure,std::generic_category(),"inspect cabinet control file");
            }
            if(!S_ISREG(information.st_mode) || information.st_size<static_cast<off_t>(file_bytes) ||
               information.st_uid!=::geteuid() || (information.st_mode&0022)!=0) {
                ::close(descriptor);
                throw std::invalid_argument("Cabinet control must be an owned regular file of at least 20 bytes, without group/world write permission");
            }
            ::close(directory);return descriptor;
        } catch(...) {::close(directory);throw;}
    }
    std::uint32_t* words_=nullptr;
    std::uint32_t role_;
};
} // namespace tc3::cabinet
