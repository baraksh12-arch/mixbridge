#pragma once
//==============================================================================
// UDPSocket.h
// Cross-platform UDP socket wrapper (macOS / Windows).
// Non-blocking receive, blocking send.
// Supports unicast and broadcast.
//==============================================================================

#include <cstdint>
#include <cstring>
#include <string>
#include <optional>
#include <array>
#include <functional>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  using SocketHandle = SOCKET;
  static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  using SocketHandle = int;
  static constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace MixBridge {

struct EndPoint
{
    std::string  address;
    uint16_t     port = 0;

    bool operator==(const EndPoint& o) const noexcept
    {
        return address == o.address && port == o.port;
    }

    bool isValid() const noexcept { return !address.empty() && port > 0; }
};

class UDPSocket
{
public:
    UDPSocket() noexcept = default;

    ~UDPSocket() noexcept { close(); }

    // Non-copyable, movable
    UDPSocket(const UDPSocket&)            = delete;
    UDPSocket& operator=(const UDPSocket&) = delete;

    UDPSocket(UDPSocket&& o) noexcept : socket_(o.socket_)
    {
        o.socket_ = kInvalidSocket;
    }

    //==========================================================================
    // Open socket, bind to port. Pass 0 to bind to any available port.
    // enableBroadcast = true for discovery broadcasts.
    //==========================================================================
    [[nodiscard]] bool open(uint16_t bindPort = 0, bool enableBroadcast = false) noexcept
    {
        platformInit();
        close();

        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kInvalidSocket) return false;

        // Allow address reuse
        int reuse = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

        // Broadcast support
        if (enableBroadcast) {
            int bc = 1;
            ::setsockopt(socket_, SOL_SOCKET, SO_BROADCAST,
                         reinterpret_cast<const char*>(&bc), sizeof(bc));
        }

        // Set send/receive buffer sizes (2MB each for high-sample-rate streams)
        int bufSize = 2 * 1024 * 1024;
        ::setsockopt(socket_, SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

        // Set non-blocking
        setNonBlocking();

        // Bind
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(bindPort);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }

        // Retrieve actual bound port
        sockaddr_in boundAddr{};
        socklen_t len = sizeof(boundAddr);
        ::getsockname(socket_, reinterpret_cast<sockaddr*>(&boundAddr), &len);
        boundPort_ = ntohs(boundAddr.sin_port);

        return true;
    }

    //==========================================================================
    // Send packet to endpoint. Returns bytes sent, or -1 on error.
    //==========================================================================
    int sendTo(const EndPoint& ep, const void* data, size_t size) noexcept
    {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port   = htons(ep.port);
        ::inet_pton(AF_INET, ep.address.c_str(), &dest.sin_addr);

        return static_cast<int>(::sendto(
            socket_,
            static_cast<const char*>(data),
            static_cast<int>(size),
            0,
            reinterpret_cast<sockaddr*>(&dest),
            sizeof(dest)));
    }

    //==========================================================================
    // Non-blocking receive. Returns bytes read and fills sender endpoint.
    // Returns 0 if no data available, -1 on error.
    //==========================================================================
    int receiveFrom(void* buffer, size_t maxSize, EndPoint& senderOut) noexcept
    {
        sockaddr_in sender{};
        socklen_t   senderLen = sizeof(sender);

        const int received = static_cast<int>(::recvfrom(
            socket_,
            static_cast<char*>(buffer),
            static_cast<int>(maxSize),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderLen));

        if (received > 0) {
            char ipStr[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &sender.sin_addr, ipStr, sizeof(ipStr));
            senderOut.address = ipStr;
            senderOut.port    = ntohs(sender.sin_port);
        }

        return received;
    }

    //==========================================================================
    // Set receive timeout in milliseconds (0 = non-blocking).
    //==========================================================================
    void setReceiveTimeout(uint32_t ms) noexcept
    {
        if (socket_ == kInvalidSocket) return;
#ifdef _WIN32
        DWORD timeout = static_cast<DWORD>(ms);
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval tv{};
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
    }

    void close() noexcept
    {
        if (socket_ != kInvalidSocket) {
#ifdef _WIN32
            ::closesocket(socket_);
#else
            ::close(socket_);
#endif
            socket_ = kInvalidSocket;
        }
    }

    bool isOpen()       const noexcept { return socket_ != kInvalidSocket; }
    uint16_t boundPort()const noexcept { return boundPort_; }

private:
    void setNonBlocking() noexcept
    {
#ifdef _WIN32
        u_long mode = 1;
        ::ioctlsocket(socket_, FIONBIO, &mode);
#else
        int flags = ::fcntl(socket_, F_GETFL, 0);
        ::fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    static void platformInit() noexcept
    {
#ifdef _WIN32
        static bool initialized = false;
        if (!initialized) {
            WSADATA wsaData;
            ::WSAStartup(MAKEWORD(2, 2), &wsaData);
            initialized = true;
        }
#endif
    }

    SocketHandle socket_    = kInvalidSocket;
    uint16_t     boundPort_ = 0;
};

} // namespace MixBridge
