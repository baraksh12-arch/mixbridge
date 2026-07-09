#pragma once
//==============================================================================
// BonjourDiscovery.h
// Bonjour/mDNS service browser for the VST3 plugin.
//
// On macOS: Uses dns_sd.h (native, zero dependencies)
// On Windows: Uses the Bonjour SDK (dns_sd.h from Apple's Bonjour for Windows)
//             - Requires the Bonjour SDK to be installed
//             - Falls back to UDP broadcast discovery if Bonjour unavailable
//
// The plugin advertises itself as _mixbridge._udp.local. and also browses
// for iPhone receivers advertising the same service type.
//==============================================================================

#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif
#include "../Network/UDPSocket.h"
#include "../../../Shared/Protocol/MixBridgeProtocol.h"
#include "../../../Shared/Common/HighResClock.h"
// Platform detection for Bonjour availability
#ifndef MIXBRIDGE_HAS_BONJOUR
  #if defined(__APPLE__)
    #define MIXBRIDGE_HAS_BONJOUR 1
  #elif defined(_WIN32)
    #if __has_include(<dns_sd.h>)
      #define MIXBRIDGE_HAS_BONJOUR 1
    #else
      #define MIXBRIDGE_HAS_BONJOUR 0
    #endif
  #else
    #define MIXBRIDGE_HAS_BONJOUR 0
  #endif
#endif

#if MIXBRIDGE_HAS_BONJOUR
  #include <dns_sd.h>
#endif

namespace MixBridge {

//==============================================================================
// Discovered receiver entry
//==============================================================================

struct ReceiverInfo
{
    std::string name;        // Bonjour service name (device name)
    std::string hostName;    // Resolved host
    std::string address;     // Resolved IP address
    uint16_t    port = 0;
    uint64_t    lastSeenNs = 0;
    bool        isResolved = false;

    bool operator==(const ReceiverInfo& o) const noexcept
    {
        return name == o.name;
    }
};

//==============================================================================
// Callbacks
//==============================================================================

struct DiscoveryCallbacks
{
    std::function<void(const ReceiverInfo&)> onReceiverFound;
    std::function<void(const std::string& name)> onReceiverLost;
    std::function<void(const std::vector<ReceiverInfo>&)> onReceiverListUpdated;
};

//==============================================================================
// BonjourDiscovery
//==============================================================================

class BonjourDiscovery
{
public:
    BonjourDiscovery() = default;
    ~BonjourDiscovery() noexcept { stop(); }

    BonjourDiscovery(const BonjourDiscovery&)            = delete;
    BonjourDiscovery& operator=(const BonjourDiscovery&) = delete;

    //==========================================================================
    // Start discovery. Plugin advertises itself and browses for receivers.
    // port = the UDP port the plugin is listening on for control messages.
    //==========================================================================
    void start(uint16_t port, const DiscoveryCallbacks& callbacks)
    {
        callbacks_ = callbacks;
        myPort_    = port;
        running_.store(true, std::memory_order_release);

#if MIXBRIDGE_HAS_BONJOUR
        startBonjour();
#else
        startFallbackDiscovery();
#endif

        // Start expiry watchdog
        watchdogThread_ = std::thread(&BonjourDiscovery::watchdogLoop, this);
    }

    //==========================================================================
    // Stop all discovery.
    //==========================================================================
    void stop() noexcept
    {
        running_.store(false, std::memory_order_release);

#if MIXBRIDGE_HAS_BONJOUR
        stopBonjour();
#endif

        if (watchdogThread_.joinable()) watchdogThread_.join();
        if (browseThread_.joinable())   browseThread_.join();
    }

    //==========================================================================
    // Get current list of discovered receivers (thread-safe).
    //==========================================================================
    std::vector<ReceiverInfo> getReceivers() const
    {
        std::lock_guard<std::mutex> lock(receiversMutex_);
        std::vector<ReceiverInfo> result;
        result.reserve(receivers_.size());
        for (const auto& [name, info] : receivers_)
            if (info.isResolved)
                result.push_back(info);
        return result;
    }

    bool isOwnService(const std::string& serviceName) const noexcept
    {
        return !ownInstanceName_.empty() && serviceName == ownInstanceName_;
    }

private:

#if MIXBRIDGE_HAS_BONJOUR

    struct ResolveContext
    {
        BonjourDiscovery* self       = nullptr;
        std::string       serviceName;
        std::string       host;
        uint16_t          port = 0;
    };

    //==========================================================================
    // Bonjour implementation
    //==========================================================================

    void startBonjour()
    {
        ownInstanceName_ = "MixBridge-" + getComputerName();
        browseThread_ = std::thread(&BonjourDiscovery::serviceLoop, this);
        fallbackResponderThread_ = std::thread(&BonjourDiscovery::fallbackResponderLoop, this);
    }

    void stopBonjour() noexcept
    {
        if (fallbackResponderThread_.joinable())
            fallbackResponderThread_.join();
    }

    // Register + browse share one DNSServiceRef; ProcessResult must run on it.
    void serviceLoop()
    {
        DNSServiceRef sdRef = nullptr;
        const DNSServiceErrorType connErr = DNSServiceCreateConnection(&sdRef);
        if (connErr != kDNSServiceErr_NoError || !sdRef)
            return;

        registrationRef_ = sdRef;
        browseRef_       = sdRef;

        const DNSServiceErrorType regErr = DNSServiceRegister(
            &sdRef,
            0,
            0,
            ownInstanceName_.c_str(),
            Protocol::kServiceType,
            nullptr,
            nullptr,
            htons(myPort_),
            0, nullptr,
            registerCallback,
            this);

        if (regErr != kDNSServiceErr_NoError) {
            DNSServiceRefDeallocate(sdRef);
            registrationRef_ = nullptr;
            browseRef_       = nullptr;
            return;
        }

        const DNSServiceErrorType browseErr = DNSServiceBrowse(
            &sdRef,
            0,
            0,
            Protocol::kServiceType,
            nullptr,
            browseCallback,
            this);

        if (browseErr != kDNSServiceErr_NoError) {
            DNSServiceRefDeallocate(sdRef);
            registrationRef_ = nullptr;
            browseRef_       = nullptr;
            return;
        }

        while (running_.load(std::memory_order_acquire) && sdRef) {
            const int fd = DNSServiceRefSockFD(sdRef);
            if (fd < 0)
                break;

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(fd, &readSet);

            timeval timeout{0, 100'000}; // 100ms
            const int result = ::select(fd + 1, &readSet, nullptr, nullptr, &timeout);

            if (result > 0 && FD_ISSET(fd, &readSet))
                DNSServiceProcessResult(sdRef);
        }

        DNSServiceRefDeallocate(sdRef);
        registrationRef_ = nullptr;
        browseRef_       = nullptr;
    }

    // Bonjour browse callback - called when a service is found/lost
    static void DNSSD_API browseCallback(
        DNSServiceRef       /*ref*/,
        DNSServiceFlags     flags,
        uint32_t            interfaceIndex,
        DNSServiceErrorType error,
        const char*         serviceName,
        const char*         /*regtype*/,
        const char*         /*replyDomain*/,
        void*               context)
    {
        if (error != kDNSServiceErr_NoError) return;
        auto* self = static_cast<BonjourDiscovery*>(context);

        if (flags & kDNSServiceFlagsAdd) {
            if (self->isOwnService(serviceName))
                return;
            self->resolveService(serviceName, interfaceIndex);
        } else {
            // Receiver went away
            std::lock_guard<std::mutex> lock(self->receiversMutex_);
            self->receivers_.erase(serviceName);

            if (self->callbacks_.onReceiverLost)
                self->callbacks_.onReceiverLost(serviceName);
        }
    }

    void resolveService(const std::string& name, uint32_t interfaceIndex)
    {
        // Add placeholder entry while resolving
        {
            std::lock_guard<std::mutex> lock(receiversMutex_);
            ReceiverInfo info;
            info.name       = name;
            info.lastSeenNs = HighResClock::nowNs();
            info.isResolved = false;
            receivers_[name] = info;
        }

        auto* ctx = new ResolveContext{ this, name, {}, 0 };

        DNSServiceRef resolveRef = nullptr;
        const DNSServiceErrorType err = DNSServiceResolve(&resolveRef,
            0,
            interfaceIndex,
            name.c_str(),
            Protocol::kServiceType,
            "local.",
            resolveCallback,
            ctx);

        if (err != kDNSServiceErr_NoError || !resolveRef) {
            delete ctx;
            return;
        }

        std::thread([resolveRef, ctx, this]() {
            if (!resolveRef) { delete ctx; return; }

            for (int i = 0; i < 50 && running_.load(); ++i) {
                int fd = DNSServiceRefSockFD(resolveRef);
                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET(fd, &readSet);
                timeval tv{0, 50'000};
                if (::select(fd + 1, &readSet, nullptr, nullptr, &tv) > 0) {
                    DNSServiceProcessResult(resolveRef);
                    break;
                }
            }
            DNSServiceRefDeallocate(resolveRef);
            delete ctx;
        }).detach();
    }

    static void DNSSD_API resolveCallback(
        DNSServiceRef       /*ref*/,
        DNSServiceFlags     /*flags*/,
        uint32_t            interfaceIndex,
        DNSServiceErrorType error,
        const char*         /*fullname*/,
        const char*         hosttarget,
        uint16_t            port,
        uint16_t            /*txtLen*/,
        const unsigned char* /*txtRecord*/,
        void*               context)
    {
        if (error != kDNSServiceErr_NoError) return;
        auto* ctx = static_cast<ResolveContext*>(context);
        if (!ctx || !ctx->self) return;

        ctx->port = ntohs(port);
        ctx->host = hosttarget ? hosttarget : "";
        ctx->self->resolveHost(*ctx, interfaceIndex);
    }

    void resolveHost(const ResolveContext& ctx, uint32_t interfaceIndex)
    {
        auto* heapCtx = new ResolveContext{ ctx.self, ctx.serviceName, ctx.host, ctx.port };

        DNSServiceRef getAddrRef = nullptr;
        DNSServiceGetAddrInfo(&getAddrRef,
            0,
            interfaceIndex,
            kDNSServiceProtocol_IPv4,
            heapCtx->host.c_str(),
            getAddressCallback,
            heapCtx);

        std::thread([getAddrRef, heapCtx, this]() {
            if (!getAddrRef) { delete heapCtx; return; }

            for (int i = 0; i < 50 && running_.load(); ++i) {
                int fd = DNSServiceRefSockFD(getAddrRef);
                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET(fd, &readSet);
                timeval tv{0, 50'000};
                if (::select(fd + 1, &readSet, nullptr, nullptr, &tv) > 0) {
                    DNSServiceProcessResult(getAddrRef);
                    break;
                }
            }
            DNSServiceRefDeallocate(getAddrRef);
            delete heapCtx;
        }).detach();
    }

    static void DNSSD_API getAddressCallback(
        DNSServiceRef       /*ref*/,
        DNSServiceFlags     /*flags*/,
        uint32_t            /*interfaceIndex*/,
        DNSServiceErrorType error,
        const char*         /*hostname*/,
        const struct sockaddr* address,
        uint32_t            /*ttl*/,
        void*               context)
    {
        if (error != kDNSServiceErr_NoError) return;
        if (!address || address->sa_family != AF_INET) return;

        auto* ctx = static_cast<ResolveContext*>(context);
        if (!ctx || !ctx->self) return;

        char ipStr[INET_ADDRSTRLEN] = {};
        const sockaddr_in* addr4 = reinterpret_cast<const sockaddr_in*>(address);
        ::inet_ntop(AF_INET, &addr4->sin_addr, ipStr, sizeof(ipStr));

        auto* self = ctx->self;

        std::lock_guard<std::mutex> lock(self->receiversMutex_);
        auto it = self->receivers_.find(ctx->serviceName);
        if (it == self->receivers_.end())
            return;

        it->second.address    = ipStr;
        it->second.hostName   = ctx->host;
        it->second.port       = ctx->port;
        it->second.isResolved = true;
        it->second.lastSeenNs = HighResClock::nowNs();

        const ReceiverInfo resolved = it->second;

        if (self->callbacks_.onReceiverFound)
            self->callbacks_.onReceiverFound(resolved);
        std::vector<ReceiverInfo> all;
        for (const auto& [n, r] : self->receivers_)
            if (r.isResolved) all.push_back(r);

        if (self->callbacks_.onReceiverListUpdated)
            self->callbacks_.onReceiverListUpdated(all);
    }

    static void DNSSD_API registerCallback(
        DNSServiceRef       /*ref*/,
        DNSServiceFlags     /*flags*/,
        DNSServiceErrorType error,
        const char*         name,
        const char*         /*regtype*/,
        const char*         /*domain*/,
        void*               context)
    {
        (void)error;
        (void)name;
        (void)context;
    }

#else // !MIXBRIDGE_HAS_BONJOUR

    void startFallbackDiscovery()
    {
        browseThread_ = std::thread(&BonjourDiscovery::fallbackResponderLoop, this);
    }

#endif // MIXBRIDGE_HAS_BONJOUR

    void fallbackResponderLoop()
    {
        UDPSocket listenSock;
        if (!listenSock.open(51235, false)) {
            return;
        }
        listenSock.setReceiveTimeout(200);

        static constexpr uint8_t kDiscoveryQuery[] = { 'M','X','B','R','D','?', 0, 0 };

        while (running_.load(std::memory_order_acquire)) {
            uint8_t buf[256];
            EndPoint sender;
            const int r = listenSock.receiveFrom(buf, sizeof(buf), sender);
            if (r < static_cast<int>(sizeof(kDiscoveryQuery))) continue;
            if (::memcmp(buf, kDiscoveryQuery, 6) != 0) continue;

            uint8_t reply[8] = { 'M','X','B','R','D','!',
                                 static_cast<uint8_t>((myPort_ >> 8) & 0xFF),
                                 static_cast<uint8_t>(myPort_ & 0xFF) };
            listenSock.sendTo(sender, reply, sizeof(reply));
        }
    }

    //==========================================================================
    // Watchdog: expire unresolved entries stuck in resolve
    //==========================================================================

    void watchdogLoop()
    {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));

            const uint64_t now     = HighResClock::nowNs();
            const uint64_t timeout = 30'000'000'000ULL; // 30 seconds for unresolved only

            std::lock_guard<std::mutex> lock(receiversMutex_);
            for (auto it = receivers_.begin(); it != receivers_.end();) {
                if (!it->second.isResolved &&
                    now - it->second.lastSeenNs > timeout) {
                    const std::string name = it->first;
                    it = receivers_.erase(it);

                    if (callbacks_.onReceiverLost)
                        callbacks_.onReceiverLost(name);
                } else {
                    ++it;
                }
            }
        }
    }

    //==========================================================================
    // Platform helpers
    //==========================================================================

    static std::string getComputerName()
    {
#ifdef _WIN32
        char name[256] = {};
        DWORD size = sizeof(name);
        ::GetComputerNameA(name, &size);
        return name;
#else
        char name[256] = {};
        ::gethostname(name, sizeof(name) - 1);
        std::string host(name);
        if (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0)
            host.resize(host.size() - 6);
        for (char& c : host)
            if (c == '.') c = '-';
        return host;
#endif
    }

    //==========================================================================
    // Members
    //==========================================================================

    DiscoveryCallbacks               callbacks_;
    uint16_t                         myPort_ = 0;
    std::atomic<bool>                running_{false};
    mutable std::mutex               receiversMutex_;
    std::map<std::string, ReceiverInfo> receivers_;
    std::thread                      browseThread_;
    std::thread                      watchdogThread_;
#if MIXBRIDGE_HAS_BONJOUR
    void*                            registrationRef_ = nullptr;
    void*                            browseRef_       = nullptr;
    std::string                      ownInstanceName_;
    std::thread                      fallbackResponderThread_;
#endif
};

} // namespace MixBridge
