// SPDX-License-Identifier: MIT
// View authority derives exclusively from successful native wire receipts.
#pragma once
#include "driver_wire.h"
#include "guest_broker_client.h"
#include <map>
#include <set>

namespace guest_broker {
class Views {
public:
    struct Grant { ViewRequest view; std::uint64_t cookie = 0; };
private:
    std::map<std::uint32_t, Grant> grants;
    std::map<std::uint32_t, std::uint32_t> parents, resources;
    static void require(bool condition, const char* message) {
        if (!condition) throw std::system_error(EPROTO, std::generic_category(), message);
    }
    template<class T> static T at(const std::vector<std::uint8_t>& packet, std::size_t offset) {
        require(offset <= packet.size() && sizeof(T) <= packet.size() - offset, "Truncated native receipt");
        T value{}; std::memcpy(&value, packet.data() + offset, sizeof(value)); return value;
    }
    void parent(std::uint32_t object, std::uint32_t owner) {
        require(object && object != owner && parents.size() < 512 && !parents.count(object), "Native object identity collision");
        if (owner) require(parents.count(owner) != 0, "Native object parent absent");
        parents.emplace(object, owner);
    }
    bool below(std::uint32_t object, std::uint32_t owner) const {
        for (std::size_t n = 0; object && n <= parents.size(); ++n) {
            if (object == owner) return true;
            const auto found = parents.find(object);
            if (found == parents.end()) return false;
            object = found->second;
        }
        return false;
    }
    std::vector<std::uint32_t> retiring(const std::vector<std::uint8_t>& packet) const {
        using namespace driver_bridge;
        const auto h = at<Header>(packet, 0); const auto op = static_cast<Op>(h.type);
        if (op == Op::DestroyVendorAllocations) {
            const auto desc = at<DestroyVendorDesc>(packet, sizeof(h));
            require(desc.count && desc.count <= ExpandedVendorAllocations &&
                    packet.size() == sizeof(h) + sizeof(desc) + desc.count * 4, "Malformed native allocation destruction");
            std::vector<std::uint32_t> ids(desc.count);
            std::memcpy(ids.data(), packet.data() + sizeof(h) + sizeof(desc), desc.count * 4); return ids;
        }
        if (op == Op::DestroyVendorResource) {
            const auto found = resources.find(h.handle);
            return {found == resources.end() ? h.handle : found->second};
        }
        if (op == Op::DestroyPagingQueue || op == Op::DestroyHwQueue || op == Op::DestroySync ||
            op == Op::UnlockVendorAllocation || op == Op::DestroyDevice || op == Op::DestroyContext || op == Op::CloseAdapter)
            return {h.handle};
        return {};
    }
    void grant(std::uint32_t object, ViewRequest view) {
        using namespace driver_bridge;
        require(parents.count(object) && grants.size() < WDDM_APERTURE_MAX_LEASES && !grants.count(object), "Native view identity collision");
        if (view.kind == WDDM_APERTURE_FENCE)
            require(!view.reserved && view.bytes == 4096 && !(view.offset % 4096) && view.offset < FenceApertureBytes && !view.generation,
                    "Native fence view out of bounds");
        else require(view.kind == WDDM_APERTURE_ALLOCATION && !view.reserved &&
                     validVendorCpuReply(view.offset, VendorCpuReply{static_cast<std::uint32_t>(view.bytes),0,view.generation}),
                     "Native allocation view out of bounds");
        for (const auto& other : grants) {
            const auto& b = other.second.view;
            require(view.kind != b.kind || view.offset >= b.offset + b.bytes || b.offset >= view.offset + view.bytes,
                    "Native view overlap");
        }
        grants.emplace(object, Grant{view,0});
    }
public:
    void approve(const std::vector<std::uint8_t>& packet) const {
        using namespace driver_bridge;
        require(packet.size() >= sizeof(Header) && packet.size() <= native_gpu::MaxPacket, "Native packet bounds");
        const auto h = at<Header>(packet,0); const auto op = static_cast<Op>(h.type);
        require(!h.status && !h.padding, "Invalid native request header");
        // Until the retained capability store is connected, do not expose a
        // native reference whose recipients could outlive this single client.
        if ((op >= Op::BeginResourceSharing && op <= Op::ImportResource) ||
            op == Op::BeginSyncSharing || op == Op::ExportSync || op == Op::ImportSync ||
            op == Op::CloseSharedSync || op == Op::QuerySharedSyncInfo)
            throw std::system_error(EOPNOTSUPP,std::generic_category(),"Broker native capability store pending");
        for (const auto target : retiring(packet)) for (const auto& entry : grants)
            if (entry.second.cookie && below(entry.first,target))
                throw std::system_error(EBUSY,std::generic_category(),"Native owner still has a CPU view");
    }
    void observe(const std::vector<std::uint8_t>& request, const std::vector<std::uint8_t>& response) {
        using namespace driver_bridge;
        const auto h = at<Header>(request,0), r = at<Header>(response,0);
        require(response.size() <= native_gpu::MaxPacket && r.type == h.type && !r.padding && r.status <= 0,
                "Native reply identity mismatch");
        if (r.status) { require(response.size() == sizeof(Header),"Native wire error payload"); return; }
        const auto op = static_cast<Op>(h.type);
        const auto retired = retiring(request);
        const bool creating = op == Op::OpenAdapter || op == Op::CreateDevice || op == Op::CreateContext ||
            op == Op::CreateVendorAllocation || op == Op::CreateVendorResourceAllocation ||
            op == Op::CreateSharedVendorResourceAllocation || op == Op::CreateNativeSharedVendorResourceAllocation ||
            op == Op::CreateGuestPagingQueue || op == Op::CreateHwQueue || op == Op::CreateSync || op == Op::LockVendorAllocation;
        // Other operations can have non-Reply payloads or legitimate pending
        // native statuses (GPU VA mapping). They confer no CPU-view authority.
        if (!creating && retired.empty()) return;
        const auto result = at<Reply>(response,sizeof(Header));
        require(!result.reserved && result.ntstatus <= 0,"Native reply result mismatch");
        if (result.ntstatus) return;
        const auto payload = sizeof(Header) + sizeof(Reply);
        if (op == Op::OpenAdapter) parent(r.handle,0);
        if (op == Op::CreateDevice || op == Op::CreateContext) parent(r.handle,h.handle);
        if (op == Op::CreateVendorAllocation || op == Op::CreateVendorResourceAllocation ||
            op == Op::CreateSharedVendorResourceAllocation || op == Op::CreateNativeSharedVendorResourceAllocation) {
            parent(r.handle,h.handle);
            if (op != Op::CreateVendorAllocation) {
                const auto resource = at<VendorResourceReply>(response,payload);
                require(!resource.reserved,"Native resource reply padding"); parent(resource.resource,r.handle);
                resources.emplace(resource.resource,r.handle);
            }
        }
        if (op == Op::CreateGuestPagingQueue) {
            const auto value = at<PagingReply>(response,payload);
            require(response.size() == payload + sizeof(value) && !value.reserved && !(value.offset % 8),"Native paging receipt");
            parent(r.handle,h.handle); parent(value.sync,r.handle);
            grant(r.handle,{WDDM_APERTURE_FENCE,0,value.offset & ~4095ull,4096,0});
        }
        if (op == Op::CreateHwQueue) {
            const auto value = at<HwQueueReply>(response,payload);
            require(response.size() == payload + sizeof(value) && !value.reserved && !(value.offset % 8),"Native queue receipt");
            parent(r.handle,h.handle); parent(value.sync,r.handle);
            grant(r.handle,{WDDM_APERTURE_FENCE,0,value.offset & ~4095ull,4096,0});
        }
        if (op == Op::CreateSync) {
            const auto desc = at<SyncDesc>(request,sizeof(Header)); parent(r.handle,h.handle);
            if (desc.type == 5) {
                const auto value = at<SyncReply>(response,payload);
                require(response.size() == payload + sizeof(value) && !(value.offset % 8),"Native sync receipt");
                grant(r.handle,{WDDM_APERTURE_FENCE,0,value.offset & ~4095ull,4096,0});
            }
        }
        if (op == Op::LockVendorAllocation) {
            const auto value = at<VendorCpuReply>(response,payload);
            require(response.size() == payload + sizeof(value) && r.handle == h.handle && validVendorCpuReply(result.value,value),
                    "Native CPU lock receipt");
            grant(h.handle,{WDDM_APERTURE_ALLOCATION,0,result.value,value.bytes,value.generation});
        }
        for (const auto target : retired) {
            for (auto i = grants.begin(); i != grants.end();) {
                if (below(i->first,target)) { require(!i->second.cookie,"Native released live CPU lease"); i=grants.erase(i); }
                else ++i;
            }
            if (op != Op::UnlockVendorAllocation) {
                std::set<std::uint32_t> removed;
                for (const auto& i : parents) if (below(i.first,target)) removed.insert(i.first);
                for (const auto id : removed) { parents.erase(id); resources.erase(id); }
            }
        }
    }
    Grant& acquire(ViewRequest view) {
        require(!view.reserved,"Reserved view request field");
        for (auto& entry : grants) {
            auto& g = entry.second;
            if (g.view.kind == view.kind && g.view.offset == view.offset && g.view.bytes == view.bytes && g.view.generation == view.generation) {
                if (g.cookie) throw std::system_error(EBUSY,std::generic_category(),"Native CPU view already granted");
                return g;
            }
        }
        throw std::system_error(EPERM,std::generic_category(),"No native receipt authorizes this CPU view");
    }
    Grant& cookie(std::uint64_t cookie) {
        if (cookie) for (auto& entry : grants) if (entry.second.cookie == cookie) return entry.second;
        throw std::system_error(EBADF,std::generic_category(),"Unknown CPU view cookie");
    }
    std::size_t active() const {
        return std::count_if(grants.begin(),grants.end(),[](const auto& i){return i.second.cookie != 0;});
    }
};
} // namespace guest_broker
