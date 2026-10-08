// SPDX-License-Identifier: MIT
// Tracing for umd_context_probe only. Captured opaque data is local test input,
// never a portable driver ABI or a redistributable fixture.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
namespace {
std::mutex traceGuard;
unsigned contextSequence = 0;
unsigned counts[256]{};
std::map<unsigned,unsigned> contextIds;
std::vector<std::pair<std::uint64_t,std::uint64_t>> gpuRanges;
void reportReferences(const char* label,const void* data,unsigned size) {
    if(!data || size>4096) return;
    for(unsigned i=0;i+8<=size;i+=4) {
        std::uint64_t value{}; std::memcpy(&value,static_cast<const unsigned char*>(data)+i,8);
        for(const auto& range:gpuRanges)
            if(value>=range.first && value-range.first<range.second)
                std::fprintf(stderr,"DXG %s gpuReferenceOffset=%u allocationBytes=%llu\n",label,i,static_cast<unsigned long long>(range.second));
    }
}
struct ContextHeader {
    std::uint32_t magic, node, engine, flags, clientHint, bytes;
};
struct Summary {
    ~Summary() {
        for (unsigned n = 0; n < 256; ++n)
            if (counts[n]) std::fprintf(stderr,"DXG summary nr=%u count=%u\n",n,counts[n]);
    }
} summary;
}
extern "C" int ioctl(int fd, unsigned long request, ...) noexcept {
    va_list args; va_start(args,request); void* data=va_arg(args,void*); va_end(args);
    using Function=int(*)(int,unsigned long,...);
    static auto real=reinterpret_cast<Function>(dlsym(RTLD_NEXT,"ioctl"));
    if(!real) return -1;
    unsigned sequence = 0;
    if(_IOC_TYPE(request)=='G' && data) {
        std::lock_guard<std::mutex> lock(traceGuard);
        ++counts[_IOC_NR(request)];
        if(_IOC_NR(request)==4 && _IOC_SIZE(request)==sizeof(D3DKMT_CREATECONTEXTVIRTUAL)) {
            const auto& a=*static_cast<D3DKMT_CREATECONTEXTVIRTUAL*>(data);
            sequence=++contextSequence;
            std::fprintf(stderr,"DXG contextInput sequence=%u node=%u engine=%u flags=%u hint=%u privateBytes=%u\n",
                         sequence,a.NodeOrdinal,a.EngineAffinity,a.Flags.Value,static_cast<unsigned>(a.ClientHint),a.PrivateDriverDataSize);
            if(a.pPrivateDriverData && a.PrivateDriverDataSize && a.PrivateDriverDataSize<=4096) {
                char path[80]; std::snprintf(path,sizeof path,"wsl-context-input-%u.bin",sequence);
                if(auto file=std::fopen(path,"wb")) {
                    const ContextHeader h{0x31585443,a.NodeOrdinal,a.EngineAffinity,a.Flags.Value,static_cast<unsigned>(a.ClientHint),a.PrivateDriverDataSize};
                    std::fwrite(&h,1,sizeof h,file);
                    std::fwrite(a.pPrivateDriverData,1,a.PrivateDriverDataSize,file); std::fclose(file);
                }
            }
        } else if(_IOC_NR(request)==24 && _IOC_SIZE(request)==sizeof(D3DKMT_CREATEHWQUEUE)) {
            const auto& a=*static_cast<D3DKMT_CREATEHWQUEUE*>(data);
            const auto found=contextIds.find(a.hHwContext);
            const unsigned context=found==contextIds.end()?0:found->second;
            unsigned flags=0; static_assert(sizeof a.Flags == sizeof flags);
            std::memcpy(&flags,&a.Flags,sizeof flags);
            std::fprintf(stderr,"DXG queueInput contextSequence=%u flags=%u privateBytes=%u\n",context,flags,a.PrivateDriverDataSize);
            reportReferences("queue",a.pPrivateDriverData,a.PrivateDriverDataSize);
            if(context && a.PrivateDriverDataSize<=4096) {
                char path[80]; std::snprintf(path,sizeof path,"wsl-queue-input-%u.bin",context);
                if(auto file=std::fopen(path,"wb")) {
                    const unsigned header[]{0x31554551,context,flags,a.PrivateDriverDataSize};
                    std::fwrite(header,1,sizeof header,file);
                    if(a.PrivateDriverDataSize) std::fwrite(a.pPrivateDriverData,1,a.PrivateDriverDataSize,file);
                    std::fclose(file);
                }
            }
        } else if(_IOC_NR(request)==2 && _IOC_SIZE(request)==sizeof(D3DKMT_CREATEDEVICE)) {
            const auto& a=*static_cast<D3DKMT_CREATEDEVICE*>(data);
            unsigned flags=0; static_assert(sizeof a.Flags == sizeof flags);
            std::memcpy(&flags,&a.Flags,sizeof flags);
            std::fprintf(stderr,"DXG deviceInput flags=%u\n",flags);
        }
    }
    const auto result=real(fd,request,data);
    const int savedErrno=errno;
    if(result>=0 && _IOC_TYPE(request)=='G' && _IOC_NR(request)==12 && _IOC_SIZE(request)==sizeof(D3DDDI_MAPGPUVIRTUALADDRESS) && data) {
        const auto& a=*static_cast<D3DDDI_MAPGPUVIRTUALADDRESS*>(data);
        std::lock_guard<std::mutex> lock(traceGuard);
        gpuRanges.emplace_back(a.VirtualAddress,a.SizeInPages*4096);
    }
    if(sequence) {
        std::lock_guard<std::mutex> lock(traceGuard);
        if(result>=0) {
            const auto& a=*static_cast<D3DKMT_CREATECONTEXTVIRTUAL*>(data);
            contextIds[a.hContext]=sequence;
            if(a.pPrivateDriverData && a.PrivateDriverDataSize && a.PrivateDriverDataSize<=4096) {
                char path[80]; std::snprintf(path,sizeof path,"wsl-context-output-%u.bin",sequence);
                if(auto file=std::fopen(path,"wb")) {
                    std::fwrite(a.pPrivateDriverData,1,a.PrivateDriverDataSize,file); std::fclose(file);
                }
            }
        }
        std::fprintf(stderr,"DXG contextResult sequence=%u return=%d\n",sequence,result);
    }
    errno=savedErrno;
    return result;
}
