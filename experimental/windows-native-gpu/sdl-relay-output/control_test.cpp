// SPDX-License-Identifier: MIT
#include <iostream>
#include "control.h"

static void require(bool value) { if(!value) throw std::runtime_error("SDL output control regression failed"); }
int main() {
    driver_sdl::Control control{"00112233445566778899aabbccddeeff","c61e010000000000",{}};
    require(driver_sdl::canonicalHex(control.session,32));
    require(!driver_sdl::canonicalHex("A0112233445566778899aabbccddeeff",32));
    const unsigned char luid[]{0xc6,0x1e,1,0,0,0,0,0};require(driver_sdl::luidText(luid)==control.adapterLuid);
    const auto live=driver_qmp::Json::parse(R"({"attached":true,"poisoned":false,"everAttached":true,
      "rootAdapterVerified":true,"generation":1,"importContextLive":true,"localScanoutLive":true,
      "liveImportedObjects":4,"channelProgramLive":true,"adapterLuid":"c61e010000000000",
      "vendor":"NVIDIA Corporation","renderer":"NVIDIA GPU","sourceWidth":3840,"sourceHeight":2160,
      "sourceFormat":"bgra8","lastQueuedFrame":300,"convertedFrames":300,"auditFrames":2,
      "independentFences":true,"cpuTextureUpload":false,"physicalInputVerified":false,
      "physicalScanoutVerified":false,"guestGpuScanoutVerified":false,"nearNativePerformanceVerified":false})");
    driver_sdl::validateState(control,live,300,2,true);
    for(const auto& bad:std::vector<std::pair<std::string,driver_qmp::Json>>{
        {"convertedFrames",299},{"lastQueuedFrame",299},{"lastQueuedFrame",300.0},{"auditFrames",3},
        {"sourceFormat","rgba8"},{"adapterLuid","0000000000000000"},{"rootAdapterVerified",false},
        {"cpuTextureUpload",true},{"physicalInputVerified",true},{"physicalScanoutVerified",true},
        {"guestGpuScanoutVerified",true},{"nearNativePerformanceVerified",true},{"independentFences",false}}) {
        auto changed=live;changed[bad.first]=bad.second;bool rejected=false;
        try { driver_sdl::validateState(control,changed,300,2,true); } catch(const std::exception&) { rejected=true; }
        require(rejected);
    }
    auto detached=live;
    for(const auto* name:{"attached","importContextLive","localScanoutLive","channelProgramLive"}) detached[name]=false;
    detached["liveImportedObjects"]=0;
    driver_sdl::validateState(control,detached,300,2,false);
    for(const auto* name:{"importContextLive","localScanoutLive","channelProgramLive"}) {
        auto changed=detached;changed[name]=true;bool rejected=false;
        try { driver_sdl::validateState(control,changed,300,2,false); } catch(const std::exception&) { rejected=true; }
        require(rejected);
    }
    std::cout<<"SDL_OUTPUT_CONTROL adapter=true monotonicFrames=true gpuConversion=true liveDetach=true broaderClaimsRejected=true gpu=false\n";
}
