// SPDX-License-Identifier: MIT
// The native driver owner also owns QEMU, including every failure teardown.
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include "qmp_protocol.h"
#include "driver_wire.h"
#include "cpu_aperture.h"
#include "qemu_fence_test.h"
#include <array>
#include <memory>
#include <optional>

namespace driver_qemu {
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Socket {
    SOCKET value = INVALID_SOCKET;
    Socket() = default;
    Socket(const Socket&) = delete;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
struct OwnedProcess {
    Handle handle;
    DWORD exit = STILL_ACTIVE;
    bool forced = false, reaped = false;
    bool exited() const { return handle.value && WaitForSingleObject(handle.value, 0) == WAIT_OBJECT_0; }
    void stop(DWORD grace = 5000) noexcept {
        if (!handle.value || reaped) return;
        if (WaitForSingleObject(handle.value, grace) != WAIT_OBJECT_0) {
            forced = true;
            TerminateProcess(handle.value, 90);
            if (WaitForSingleObject(handle.value, 10000) != WAIT_OBJECT_0) {
                // Holding the native driver objects is safer than returning
                // while this owned VM could still access their mapped pages.
                std::fprintf(stderr, "QEMU_OWNER_STOP_PENDING nativeObjectsRetained=true\n");
                WaitForSingleObject(handle.value, INFINITE);
            }
        }
        GetExitCodeProcess(handle.value, &exit);
        reaped = true;
    }
    ~OwnedProcess() { stop(0); }
};
inline unsigned short reservePort(Socket& socket) {
    socket.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket.value == INVALID_SOCKET) throw std::runtime_error("QMP socket creation failed");
    const BOOL exclusive = TRUE;
    if (setsockopt(socket.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive))
        throw std::runtime_error("QMP port reservation failed");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof address)) throw std::runtime_error("QMP bind failed");
    int size = sizeof address;
    if (getsockname(socket.value, reinterpret_cast<sockaddr*>(&address), &size)) throw std::runtime_error("QMP endpoint failed");
    return ntohs(address.sin_port);
}
class Qmp {
    Socket socket;
    std::string buffered;
    std::uint64_t next = 0;
    void ready(bool reading, ULONGLONG deadline) {
        const auto now = GetTickCount64();
        if (now >= deadline) throw std::runtime_error("QMP control deadline exceeded");
        fd_set set; FD_ZERO(&set); FD_SET(socket.value, &set);
        const auto remaining = deadline - now;
        timeval timeout{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};
        if (select(0, reading ? &set : nullptr, reading ? nullptr : &set, nullptr, &timeout) != 1)
            throw std::runtime_error("QMP control socket failed");
    }
    driver_qmp::Json read(ULONGLONG deadline) {
        for (;;) {
            const auto end = buffered.find('\n');
            if (end != std::string::npos) {
                auto line = buffered.substr(0, end); buffered.erase(0, end + 1);
                return driver_qmp::parse(line);
            }
            if (buffered.size() >= driver_qmp::MaxMessage) throw std::runtime_error("QMP line bound exceeded");
            ready(true, deadline);
            char bytes[1024]; const auto count = recv(socket.value, bytes, sizeof bytes, 0);
            if (count <= 0) throw std::runtime_error("QMP disconnected");
            buffered.append(bytes, static_cast<std::size_t>(count));
        }
    }
public:
    Qmp(unsigned short port, const OwnedProcess& owner, const std::string& name) {
        const auto deadline = GetTickCount64() + 10000;
        for (;;) {
            socket.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket.value == INVALID_SOCKET) throw std::runtime_error("QMP client socket failed");
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
            if (!connect(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof address)) break;
            closesocket(socket.value); socket.value = INVALID_SOCKET;
            if (owner.exited() || GetTickCount64() >= deadline) throw std::runtime_error("Owned QEMU QMP unavailable");
            Sleep(20);
        }
        const DWORD milliseconds = 5000;
        if (setsockopt(socket.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds) ||
            setsockopt(socket.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds))
            throw std::runtime_error("QMP timeout configuration failed");
        const auto greeting = read(deadline);
        if (!greeting.contains("QMP") || !greeting.at("QMP").is_object()) throw std::runtime_error("QMP greeting invalid");
        if (!call("qmp_capabilities").is_object()) throw std::runtime_error("QMP capabilities invalid");
        const auto identity = call("query-name");
        if (!identity.is_object() || !identity.contains("name") || identity.at("name") != name)
            throw std::runtime_error("QMP owned machine name mismatch");
    }
    driver_qmp::Json call(const std::string& operation, driver_qmp::Json arguments = nullptr) {
        if (next == UINT64_MAX) throw std::runtime_error("QMP ID exhausted");
        const auto id = "fence-" + std::to_string(++next);
        driver_qmp::Json command{{"execute", operation}, {"id", id}};
        if (!arguments.is_null()) command["arguments"] = std::move(arguments);
        const auto bytes = command.dump() + '\n';
        if (bytes.size() > driver_qmp::MaxMessage) throw std::runtime_error("QMP request bound exceeded");
        const auto deadline = GetTickCount64() + 5000;
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            ready(false, deadline);
            const auto count = send(socket.value, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
            if (count <= 0) throw std::runtime_error("QMP write failed");
            sent += static_cast<std::size_t>(count);
        }
        for (unsigned eventCount = 0; eventCount < 32; ++eventCount) {
            const auto reply = read(deadline);
            if (driver_qmp::event(reply)) continue;
            return driver_qmp::result(reply, id);
        }
        throw std::runtime_error("QMP asynchronous event bound exceeded");
    }
    void mapping(const std::string& command) {
        const auto reply = call("qom-set", {{"path", "/machine/peripheral/wddm-fences"}, {"property", "fence-mapping"}, {"value", command}});
        if (!reply.is_object() || !reply.empty()) throw std::runtime_error("QMP mapping acknowledgement invalid");
    }
    void allocationMapping(const std::string& command) {
        const auto reply = call("qom-set", {{"path", "/machine/peripheral/wddm-allocations"}, {"property", "allocation-mapping"}, {"value", command}});
        if (!reply.is_object() || !reply.empty()) throw std::runtime_error("QMP allocation acknowledgement invalid");
    }
};
struct FenceLease { std::uint32_t slot; std::uint64_t generation, offset; };
using AllocationLease = driver_cpu::Lease;
class Runtime {
    Handle job;
    OwnedProcess process;
    std::unique_ptr<Qmp> qmp;
    std::array<std::optional<FenceLease>, 64> leases;
    std::size_t allocationSlots = driver_bridge::DefaultVendorCpuSlots;
    driver_cpu::Aperture allocationAperture;
    bool allocationsEnabled = false;
    std::uint64_t generation = 0;
    unsigned mapped = 0, mappedTotal = 0, unmappedTotal = 0;
    bool stopped = false, failed = false;
    std::filesystem::path logfile;
public:
    Runtime(unsigned short driverPort, const qemu_fence::Paths& paths, bool enableCpu = false, bool cpuStoreTest = false, bool cpuEofTest = false, bool hwQueueEofTest = false, std::size_t cpuSlots = driver_bridge::DefaultVendorCpuSlots, bool syncEofTest = false, bool reservationEofTest = false, bool gpuStateEofTest = false, bool cpuSpanEofTest = false, bool syncNoMaxEofTest = false)
        : allocationSlots(cpuSlots), allocationAperture(cpuSlots), allocationsEnabled(enableCpu), logfile(std::filesystem::absolute(paths.log)) {
        if (!driver_bridge::validVendorCpuSlots(cpuSlots) || (!enableCpu && cpuSlots != driver_bridge::DefaultVendorCpuSlots))
            throw std::runtime_error("Invalid configured allocation aperture capacity");
        const auto executable = std::filesystem::absolute(paths.qemu).wstring();
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limit, sizeof limit))
            throw std::runtime_error("Cannot create owned QEMU process job");
        Socket reservation; const auto qmpPort = reservePort(reservation);
        unsigned char random[16]{};
        if (BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot create owned QEMU name");
        std::string name = "wddm-"; const char hex[] = "0123456789abcdef";
        for (const auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
        Handle source, log, input;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &source.value,
                PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, TRUE, 0))
            throw std::runtime_error("Cannot create inherited source process handle");
        SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
        log.value = CreateFileW(logfile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (log.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create fresh QEMU log and input");
        HANDLE inherited[]{source.value, log.value, input.value};
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
        bool initialized = attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &size);
        bool ready = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr);
        std::vector<std::wstring> args{executable, L"-machine", L"q35,accel=whpx", L"-cpu", L"host", L"-m", L"512", L"-smp", L"1",
            L"-display", L"none", L"-monitor", L"none", L"-serial", L"stdio", L"-nodefaults", L"-no-reboot",
            L"-L", std::filesystem::absolute(paths.firmware).wstring(), L"-kernel", std::filesystem::absolute(paths.kernel).wstring(),
            L"-initrd", std::filesystem::absolute(paths.initramfs).wstring(), L"-append",
            cpuStoreTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_cpu_store_test=1" :
            syncEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_sync_eof_test=1" :
            syncNoMaxEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_sync_no_max_eof_test=1" :
            reservationEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_reservation_eof_test=1" :
            gpuStateEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_gpu_state_eof_test=1" :
            cpuSpanEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_cpu_span_eof_test=1" :
            hwQueueEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_hwqueue_eof_test=1" :
            cpuEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_cpu_eof_test=1" : L"console=ttyS0 rdinit=/init panic=1",
            L"-name", std::wstring(name.begin(), name.end()), L"-qmp", L"tcp:127.0.0.1:" + std::to_wstring(qmpPort) + L",server=on,wait=off",
            L"-device", L"wddm-fence-hub,id=wddm-fences,source-process=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(source.value)),
            L"-device", L"virtio-serial-pci,id=bridge-serial", L"-chardev",
            L"socket,id=wddm,host=127.0.0.1,port=" + std::to_wstring(driverPort), L"-device",
            L"virtserialport,bus=bridge-serial.0,chardev=wddm,name=org.7wdev.wddm"};
        if (allocationsEnabled) {
            args.push_back(L"-device");
            args.push_back(L"wddm-allocation-hub,id=wddm-allocations,source-process=" +
                           std::to_wstring(reinterpret_cast<std::uintptr_t>(source.value)) +
                           (allocationSlots == driver_bridge::DefaultVendorCpuSlots ? L"" : L",slot-count=" + std::to_wstring(allocationSlots)));
        }
        std::wstring command;
        for (const auto& argument : args) { if (!command.empty()) command += L' '; command += qemu_fence::quote(argument); }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup; startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value; startup.StartupInfo.hStdOutput = log.value; startup.StartupInfo.hStdError = log.value;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION child{};
        closesocket(reservation.value); reservation.value = INVALID_SOCKET;
        const auto started = ready && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
        if (initialized) DeleteProcThreadAttributeList(attributes);
        if (attributes) HeapFree(GetProcessHeap(), 0, attributes);
        if (!started) throw std::runtime_error("Cannot start owned QEMU runtime");
        process.handle.value = child.hProcess;
        const bool assigned = AssignProcessToJobObject(job.value, child.hProcess) != FALSE;
        const bool resumed = assigned && ResumeThread(child.hThread) != static_cast<DWORD>(-1);
        CloseHandle(child.hThread);
        if (!resumed) throw std::runtime_error("Cannot start QEMU within its owned job");
        std::cout << "{\"ownedQemuPid\":" << child.dwProcessId << "}\n" << std::flush;
        qmp = std::make_unique<Qmp>(qmpPort, process, name);
    }
    ~Runtime() { stop(); }
    FenceLease map(volatile std::uint64_t* fence) {
        if (stopped || !fence || generation == UINT64_MAX) throw std::runtime_error("Fence mapping owner unavailable");
        std::uint32_t slot = 0;
        for (; slot < leases.size(); ++slot) if (!leases[slot]) break;
        if (slot == leases.size()) throw std::runtime_error("Fence slot quota exceeded");
        const auto source = reinterpret_cast<std::uintptr_t>(fence);
        const FenceLease lease{slot, ++generation, slot * 4096ull + (source & 4095)};
        try { qmp->mapping("map:" + std::to_string(slot) + ':' + std::to_string(source) + ':' + std::to_string(lease.generation)); }
        catch (...) { failed = true; stop(0); throw; }
        leases[slot] = lease; ++mapped; ++mappedTotal; return lease;
    }
    void unmap(FenceLease lease) {
        if (stopped) return; // stop() verified owned VM exit before clearing pages.
        if (lease.slot >= leases.size() || !leases[lease.slot] || leases[lease.slot]->generation != lease.generation)
            throw std::runtime_error("Native fence lease ownership mismatch");
        try { qmp->mapping("unmap:" + std::to_string(lease.slot) + ':' + std::to_string(lease.generation)); }
        catch (...) { failed = true; stop(0); throw; }
        leases[lease.slot].reset(); --mapped; ++unmappedTotal;
    }
    void stop(DWORD grace = 5000) noexcept {
        if (stopped) return;
        process.stop(grace); // Every driver owner must outlive this step.
        qmp.reset();
        for (auto& lease : leases) lease.reset();
        allocationAperture.clearAfterVmExit();
        mapped = 0; stopped = true;
    }
    bool cleanExit() const { return stopped && !failed && !process.forced && process.exit == 0; }
    bool hasStopped() const { return stopped; }
    unsigned liveMappings() const { return mapped; }
    bool canMapFence() const { return !stopped && generation != UINT64_MAX && mapped < leases.size(); }
    std::size_t allocationSlotLimit() const { return allocationSlots; }
    bool canMapAllocation(std::uint32_t bytes) const { return allocationsEnabled && !stopped && allocationAperture.canMap(bytes); }
    AllocationLease mapAllocation(void* data, std::uint32_t bytes) {
        const auto source = reinterpret_cast<std::uintptr_t>(data);
        if (!canMapAllocation(bytes) || !source || source % 4096 || source > (1ull << 47) - bytes)
            throw std::runtime_error("Invalid owned allocation mapping");
        return allocationAperture.map(bytes, [this, source](std::uint32_t slot, std::uint64_t offset, std::uint32_t chunk, std::uint64_t chunkGeneration) {
            qmp->allocationMapping("map:" + std::to_string(slot) + ':' + std::to_string(source + offset) + ':' +
                                    std::to_string(chunk) + ':' + std::to_string(chunkGeneration));
        }, [this] { failed = true; stop(0); });
    }
    void unmapAllocation(AllocationLease lease) {
        if (stopped) return; // VM exit was proved before its foreign pages were cleared.
        allocationAperture.release(lease, [this](std::uint32_t slot, std::uint64_t chunkGeneration) {
            qmp->allocationMapping("unmap:" + std::to_string(slot) + ':' + std::to_string(chunkGeneration));
        }, [this] { failed = true; stop(0); });
    }
    void report() const {
        std::cout << "{\"ownedQemuExited\":" << (stopped ? "true" : "false") << ",\"qemuExit\":" << process.exit
                  << ",\"qemuForcedStop\":" << (process.forced ? "true" : "false")
                  << ",\"fenceControlFailed\":" << (failed ? "true" : "false")
                  << ",\"liveFenceMappings\":" << mapped << ",\"fenceMappingsCreated\":" << mappedTotal
                  << ",\"fenceUnmapAcknowledgements\":" << unmappedTotal
                  << ",\"liveAllocationMappings\":" << allocationAperture.groups() << ",\"liveAllocationMappedBytes\":" << allocationAperture.bytes()
                  << ",\"allocationMappingsCreated\":" << allocationAperture.maps()
                  << ",\"liveAllocationSlotMappings\":" << allocationAperture.chunks()
                  << ",\"allocationSlotMappingsCreated\":" << allocationAperture.chunkMaps()
                  << ",\"allocationSlotUnmapAcknowledgements\":" << allocationAperture.chunkUnmaps()
                  << ",\"allocationApertureSlots\":" << allocationSlots
                  << ",\"allocationUnmapAcknowledgements\":" << allocationAperture.unmaps() << "}\n";
    }
};
} // namespace driver_qemu
