// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/cabinet/control.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <poll.h>
#include <sys/wait.h>
using namespace tc3::cabinet;
static void require(bool value,const char* why) {
    if(!value) {std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}
}
static void exact_write(int fd,const void* data,std::size_t size) {
    auto p=static_cast<const char*>(data);
    while(size) {auto n=::write(fd,p,size);if(n<0&&errno==EINTR)continue;require(n>0,"write test protocol");p+=n;size-=n;}
}
static void exact_read(int fd,void* data,std::size_t size) {
    auto p=static_cast<char*>(data);
    while(size) {auto n=::read(fd,p,size);if(n<0&&errno==EINTR)continue;require(n>0,"read test protocol");p+=n;size-=n;}
}
int main() {
    char temporary[]="/tmp/tc3-control-test-XXXXXX";
    require(::mkdtemp(temporary)!=nullptr,"private temporary directory");
    auto root=std::filesystem::path(temporary),path=root/"control.bin";
    int fd=::open(path.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);require(fd>=0,"create supervisor file");
    const std::array<std::uint32_t,5> initial{SharedControl::magic,0,0,0,0};
    exact_write(fd,initial.data(),sizeof(initial));::close(fd);
    auto left=std::make_unique<SharedControl>(path,1);
    require(left->worker_mask()==1&&!left->paused()&&!left->quitting(),"initial worker and control state");
    bool duplicate=false;
    try {SharedControl again(path,1);} catch(const std::system_error&) {duplicate=true;}
    require(duplicate&&left->worker_mask()==1,"duplicate worker cannot clear the existing owner");
    int commands[2],replies[2];require(::pipe(commands)==0&&::pipe(replies)==0,"create protocol pipes");
    pid_t pid=::fork();require(pid>=0,"fork second cabinet");
    if(pid==0) {
        ::close(commands[1]);::close(replies[0]);
        auto right=std::make_unique<SharedControl>(path,2);
        auto report=[&] {std::array<std::uint32_t,4> state{right->pause_mask(),right->worker_mask(),right->quitting(),right->rendering_bits()};exact_write(replies[1],state.data(),sizeof(state));};
        report();
        while(true) {
            char operation;exact_read(commands[0],&operation,1);
            if(operation=='P') right->set_paused(true);
            if(operation=='R') right->set_paused(false);
            if(operation=='T') right->set_rendering_bits(0x8000cd34);
            if(operation=='B') {
                right->set_rendering_bits(0x80001234);report();
                for(unsigned i=0;i<500000;++i) right->set_rendering_bits((i&1)?0x80001234:0x8000cdef);
                report();continue;
            }
            if(operation=='X') {right.reset();char done='X';exact_write(replies[1],&done,1);_exit(0);}
            report();
        }
    }
    ::close(commands[0]);::close(replies[1]);
    auto state=[&] {std::array<std::uint32_t,4> result;exact_read(replies[0],result.data(),sizeof(result));return result;};
    auto command=[&](char operation) {exact_write(commands[1],&operation,1);return state();};
    require(state()[1]==3,"both processes register independently");
    require(left->rendering_bits()==0,"rendering publication initially unset");
    left->set_rendering_bits(0x8000ab12);
    require(command('Q')[3]==0x8000ab12,"left rendering publication visible in right process");
    require(command('T')[3]==0x8000cd34 && left->rendering_bits()==0x8000cd34,
            "right rendering publication visible in left process");
    require(command('B')[3]==0x80001234,"concurrent rendering publication begins atomically");
    pollfd ready{replies[0],POLLIN,0};
    while(::poll(&ready,1,0)==0) {
        auto bits=left->rendering_bits();
        require(bits==0x80001234 || bits==0x8000cdef,"packed rendering word never tears across processes");
    }
    auto rendered=state();
    require(rendered[3]==0x80001234 && left->rendering_bits()==0x80001234,"final rendering publication coherent");
    left->set_paused(true);require(command('Q')[0]==1,"left menu pauses the right process");
    require(command('P')[0]==3,"each process owns its own pause bit");
    left->set_paused(false);require(command('Q')[0]==2&&left->paused(),"resuming left cannot override right menu");
    require(command('R')[0]==0&&!left->paused(),"both resume only after every owner clears pause");
    require(command('P')[0]==2,"right pauses independently");
    left.reset();auto remaining=command('Q');
    require(remaining[0]==2&&remaining[1]==2,"closing left preserves right pause and registration");
    left=std::make_unique<SharedControl>(path,1);left->set_paused(true);
    left->request_quit();require(command('Q')[2]==1,"shared quit observed in the other process");
    exact_write(commands[1],"X",1);char done;exact_read(replies[0],&done,1);
    require(done=='X'&&left->pause_mask()==1&&left->worker_mask()==1,"closing right clears only its own bits");
    int status=0;require(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"second process exited cleanly");
    left.reset();::close(commands[1]);::close(replies[0]);
    require(std::filesystem::exists(path),"worker destructors never unlink supervisor file");
    fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);require(fd>=0,"read final control state");
    std::array<std::uint32_t,5> final{};exact_read(fd,final.data(),sizeof(final));::close(fd);
    require(final[1]==0&&final[2]==0&&final[3]==1,"last worker cleanup preserves sticky quit");
    // The supervisor file format is accepted only after a validated header.
    fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);require(fd>=0,"open invalid header fixture");
    auto invalid=initial;invalid[0]^=1;exact_write(fd,invalid.data(),sizeof(invalid));::close(fd);
    bool rejected=false;try {SharedControl bad(path,1);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"invalid magic rejected");
    ::symlink(path.filename().c_str(),(root/"alias.bin").c_str());
    rejected=false;try {SharedControl bad(root/"alias.bin",1);}catch(const std::exception&){rejected=true;}
    require(rejected,"symlink control file rejected");
    ::symlink(root.c_str(),(root/"aliasdir").c_str());
    rejected=false;try {SharedControl bad(root/"aliasdir/control.bin",1);}catch(const std::exception&){rejected=true;}
    require(rejected,"symlink ancestor rejected");
    fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);require(fd>=0,"open short fixture");::ftruncate(fd,4);::close(fd);
    rejected=false;try {SharedControl bad(path,1);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"short control mapping rejected before mmap");
    std::filesystem::remove(root/"alias.bin");std::filesystem::remove(root/"aliasdir");std::filesystem::remove(path);std::filesystem::remove(root);
    std::puts("PASS two-process pause ownership, atomic rendering publication, shared quit, worker cleanup, duplicate role and invalid control-file rejection");
}
