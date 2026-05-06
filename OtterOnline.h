#pragma once

// ============================================================
//  OtterOnline.h
//  Header-only UDP rendezvous / NAT traversal helper for Otter.
//
//  Notes:
//    - Two private-network peers cannot discover each other with no public
//      coordination point at all. This header provides that coordination point
//      as RendezvousServer, plus a Peer client with UDP hole punching.
//    - Direct UDP is attempted first. If it fails, packets can be relayed
//      through the rendezvous server.
//    - This is transport/signaling only. Remote control payloads should add
//      authentication and encryption above this layer before exposure.
// ============================================================

#include "OtterPlatform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Otter::Online
{
    namespace detail
    {
        inline uint64_t now_ms()
        {
            return Platform::monotonic_ms();
        }

        inline uint64_t random_u64()
        {
            std::random_device rd;
            std::mt19937_64 rng(
                (static_cast<uint64_t>(rd()) << 32u) ^ Platform::random_seed());
            uint64_t value = rng();
            return value ? value : 1u;
        }

        inline void append_u8(std::vector<uint8_t>& out, uint8_t value)
        {
            out.push_back(value);
        }

        inline void append_u16(std::vector<uint8_t>& out, uint16_t value)
        {
            out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
            out.push_back(static_cast<uint8_t>(value & 0xffu));
        }

        inline void append_u32(std::vector<uint8_t>& out, uint32_t value)
        {
            out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
            out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
            out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
            out.push_back(static_cast<uint8_t>(value & 0xffu));
        }

        inline void append_u64(std::vector<uint8_t>& out, uint64_t value)
        {
            append_u32(out, static_cast<uint32_t>(value >> 32u));
            append_u32(out, static_cast<uint32_t>(value & 0xffffffffu));
        }

        inline bool read_u8(const uint8_t*& p, const uint8_t* end, uint8_t& value)
        {
            if (p + 1 > end) return false;
            value = *p++;
            return true;
        }

        inline bool read_u16(const uint8_t*& p, const uint8_t* end, uint16_t& value)
        {
            if (p + 2 > end) return false;
            value = static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | p[1]);
            p += 2;
            return true;
        }

        inline bool read_u32(const uint8_t*& p, const uint8_t* end, uint32_t& value)
        {
            if (p + 4 > end) return false;
            value =
                (static_cast<uint32_t>(p[0]) << 24u) |
                (static_cast<uint32_t>(p[1]) << 16u) |
                (static_cast<uint32_t>(p[2]) << 8u) |
                static_cast<uint32_t>(p[3]);
            p += 4;
            return true;
        }

        inline bool read_u64(const uint8_t*& p, const uint8_t* end, uint64_t& value)
        {
            uint32_t hi = 0;
            uint32_t lo = 0;
            if (!read_u32(p, end, hi) || !read_u32(p, end, lo)) return false;
            value = (static_cast<uint64_t>(hi) << 32u) | lo;
            return true;
        }
    }

    class SocketSystem
    {
    public:
        SocketSystem()
        {
            if (!system_.ok())
                throw std::runtime_error("network socket system initialization failed");
        }

        ~SocketSystem() noexcept = default;

        SocketSystem(const SocketSystem&) = delete;
        SocketSystem& operator=(const SocketSystem&) = delete;

    private:
        Platform::NetworkSystem system_;
    };

    struct Endpoint
    {
        sockaddr_in addr{};

        Endpoint()
        {
            addr.sin_family = AF_INET;
        }

        explicit Endpoint(sockaddr_in value) : addr(value)
        {
            addr.sin_family = AF_INET;
        }

        static Endpoint any(uint16_t port)
        {
            Endpoint ep;
            ep.addr.sin_addr.s_addr = htonl(INADDR_ANY);
            ep.addr.sin_port = htons(port);
            return ep;
        }

        static std::optional<Endpoint> resolve(std::string host, uint16_t port)
        {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;

            addrinfo* result = nullptr;
            const std::string service = std::to_string(port);
            if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || !result)
                return std::nullopt;

            Endpoint ep;
            ep.addr = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
            freeaddrinfo(result);
            return ep;
        }

        std::string host() const
        {
            char text[INET_ADDRSTRLEN]{};
            if (!Platform::inet_ntop_v4(addr.sin_addr, text, sizeof(text)))
                return {};
            return text;
        }

        uint16_t port() const
        {
            return ntohs(addr.sin_port);
        }

        uint32_t ipv4_be() const
        {
            return addr.sin_addr.s_addr;
        }

        std::string to_string() const
        {
            return host() + ":" + std::to_string(port());
        }

        friend bool operator==(const Endpoint& a, const Endpoint& b)
        {
            return a.addr.sin_addr.s_addr == b.addr.sin_addr.s_addr
                && a.addr.sin_port == b.addr.sin_port;
        }
    };

    class UdpSocket
    {
    public:
        UdpSocket() = default;
        ~UdpSocket() noexcept { close(); }

        UdpSocket(const UdpSocket&) = delete;
        UdpSocket& operator=(const UdpSocket&) = delete;

        UdpSocket(UdpSocket&& other) noexcept
            : socket_(std::exchange(other.socket_, Platform::invalid_socket))
        {
        }

        UdpSocket& operator=(UdpSocket&& other) noexcept
        {
            if (this != &other)
            {
                close();
                socket_ = std::exchange(other.socket_, Platform::invalid_socket);
            }
            return *this;
        }

        bool open(uint16_t local_port = 0, bool nonblocking = true)
        {
            close();
            if (!Platform::network_system_ready())
                return false;
            socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket_ == Platform::invalid_socket)
                return false;

            int reuse = 1;
            setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

            const Endpoint bind_ep = Endpoint::any(local_port);
            if (::bind(socket_, reinterpret_cast<const sockaddr*>(&bind_ep.addr), sizeof(bind_ep.addr)) != 0)
            {
                close();
                return false;
            }

            if (nonblocking)
            {
                if (!Platform::set_socket_nonblocking(socket_, true))
                {
                    close();
                    return false;
                }
            }
            return true;
        }

        void close() noexcept
        {
            if (socket_ != Platform::invalid_socket)
            {
                Platform::close_socket(socket_);
                socket_ = Platform::invalid_socket;
            }
        }

        bool valid() const { return socket_ != Platform::invalid_socket; }

        int send_to(const Endpoint& endpoint, const void* data, size_t size) const
        {
            if (!valid() || !data || size == 0) return Platform::socket_error;
            return static_cast<int>(sendto(
                socket_,
                reinterpret_cast<const char*>(data),
                static_cast<int>(size),
                0,
                reinterpret_cast<const sockaddr*>(&endpoint.addr),
                sizeof(endpoint.addr)));
        }

        int recv_from(void* data, size_t size, Endpoint& from) const
        {
            if (!valid() || !data || size == 0) return Platform::socket_error;
            Platform::SockLen from_len = static_cast<Platform::SockLen>(sizeof(from.addr));
            return static_cast<int>(recvfrom(
                socket_,
                reinterpret_cast<char*>(data),
                static_cast<int>(size),
                0,
                reinterpret_cast<sockaddr*>(&from.addr),
                &from_len));
        }

        Endpoint local_endpoint() const
        {
            Endpoint ep;
            if (!valid()) return ep;
            Platform::SockLen len = static_cast<Platform::SockLen>(sizeof(ep.addr));
            getsockname(socket_, reinterpret_cast<sockaddr*>(&ep.addr), &len);
            if (ep.addr.sin_addr.s_addr == htonl(INADDR_ANY))
            {
                const uint32_t lan_ip = first_lan_ipv4();
                if (lan_ip != 0)
                    ep.addr.sin_addr.s_addr = lan_ip;
            }
            return ep;
        }

    private:
        static uint32_t first_lan_ipv4()
        {
            char hostname[256]{};
            if (gethostname(hostname, sizeof(hostname)) != 0)
                return 0;

            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result)
                return 0;

            uint32_t chosen = 0;
            for (addrinfo* it = result; it; it = it->ai_next)
            {
                auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
                const uint32_t host_order = ntohl(addr->sin_addr.s_addr);
                const bool loopback = (host_order >> 24u) == 127u;
                const bool link_local = (host_order & 0xffff0000u) == 0xa9fe0000u;
                if (!loopback && !link_local)
                {
                    chosen = addr->sin_addr.s_addr;
                    break;
                }
            }
            freeaddrinfo(result);
            return chosen;
        }

        Platform::NativeSocket socket_ = Platform::invalid_socket;
    };

    struct StunResult
    {
        Endpoint mapped;
        bool ok = false;
    };

    class StunClient
    {
    public:
        static StunResult query(
            UdpSocket& socket,
            const std::string& host = "stun.l.google.com",
            uint16_t port = 19302,
            int timeout_ms = 1200)
        {
            StunResult result;
            auto server = Endpoint::resolve(host, port);
            if (!server)
                return result;

            uint8_t request[20]{};
            request[0] = 0x00;
            request[1] = 0x01; // Binding Request
            request[4] = 0x21;
            request[5] = 0x12;
            request[6] = 0xA4;
            request[7] = 0x42;
            for (int i = 8; i < 20; ++i)
                request[i] = static_cast<uint8_t>(detail::random_u64() >> ((i % 8) * 8));

            socket.send_to(*server, request, sizeof(request));

            const uint64_t deadline = detail::now_ms() + static_cast<uint64_t>(timeout_ms);
            uint8_t buffer[1024]{};
            while (detail::now_ms() < deadline)
            {
                Endpoint from;
                const int received = socket.recv_from(buffer, sizeof(buffer), from);
                if (received >= 20 && parse_response(buffer, static_cast<size_t>(received), result.mapped))
                {
                    result.ok = true;
                    return result;
                }
                Platform::sleep_ms(10);
            }
            return result;
        }

    private:
        static bool parse_response(const uint8_t* data, size_t size, Endpoint& mapped)
        {
            if (!data || size < 20) return false;
            if (data[0] != 0x01 || data[1] != 0x01) return false;
            const uint32_t magic =
                (static_cast<uint32_t>(data[4]) << 24u) |
                (static_cast<uint32_t>(data[5]) << 16u) |
                (static_cast<uint32_t>(data[6]) << 8u) |
                static_cast<uint32_t>(data[7]);
            if (magic != 0x2112A442u) return false;

            size_t offset = 20;
            while (offset + 4 <= size)
            {
                const uint16_t type = static_cast<uint16_t>((data[offset] << 8u) | data[offset + 1]);
                const uint16_t len = static_cast<uint16_t>((data[offset + 2] << 8u) | data[offset + 3]);
                offset += 4;
                if (offset + len > size) return false;

                if ((type == 0x0020 || type == 0x0001) && len >= 8 && data[offset + 1] == 0x01)
                {
                    uint16_t port = static_cast<uint16_t>((data[offset + 2] << 8u) | data[offset + 3]);
                    uint32_t ip =
                        (static_cast<uint32_t>(data[offset + 4]) << 24u) |
                        (static_cast<uint32_t>(data[offset + 5]) << 16u) |
                        (static_cast<uint32_t>(data[offset + 6]) << 8u) |
                        static_cast<uint32_t>(data[offset + 7]);

                    if (type == 0x0020)
                    {
                        port ^= 0x2112u;
                        ip ^= 0x2112A442u;
                    }

                    mapped.addr.sin_family = AF_INET;
                    mapped.addr.sin_port = htons(port);
                    mapped.addr.sin_addr.s_addr = htonl(ip);
                    return true;
                }

                offset += (len + 3u) & ~size_t(3u);
            }
            return false;
        }
    };

    enum class MessageType : uint8_t
    {
        Hello = 1,
        PeerList = 2,
        Punch = 3,
        PunchAck = 4,
        Data = 5,
        Relay = 6,
        RelayData = 7,
        KeepAlive = 8,
    };

    struct Packet
    {
        MessageType type = MessageType::Hello;
        uint64_t session = 0;
        uint64_t from = 0;
        uint64_t to = 0;
        uint32_t seq = 0;
        std::vector<uint8_t> payload;
    };

    inline constexpr uint32_t kMagic = 0x4F544F4Eu; // OTON
    inline constexpr uint8_t kVersion = 1;

    inline std::vector<uint8_t> encode_packet(const Packet& packet)
    {
        std::vector<uint8_t> out;
        out.reserve(34 + packet.payload.size());
        detail::append_u32(out, kMagic);
        detail::append_u8(out, kVersion);
        detail::append_u8(out, static_cast<uint8_t>(packet.type));
        detail::append_u16(out, 0);
        detail::append_u64(out, packet.session);
        detail::append_u64(out, packet.from);
        detail::append_u64(out, packet.to);
        detail::append_u32(out, packet.seq);
        const size_t payload_size = (std::min)(packet.payload.size(), size_t(0xffff));
        detail::append_u16(out, static_cast<uint16_t>(payload_size));
        out.insert(out.end(), packet.payload.begin(), packet.payload.begin() + payload_size);
        return out;
    }

    inline bool decode_packet(const uint8_t* data, size_t size, Packet& packet)
    {
        if (!data || size < 38) return false;
        const uint8_t* p = data;
        const uint8_t* end = data + size;

        uint32_t magic = 0;
        uint8_t version = 0;
        uint8_t type = 0;
        uint16_t flags = 0;
        uint16_t payload_size = 0;
        if (!detail::read_u32(p, end, magic) || magic != kMagic) return false;
        if (!detail::read_u8(p, end, version) || version != kVersion) return false;
        if (!detail::read_u8(p, end, type)) return false;
        if (!detail::read_u16(p, end, flags)) return false;
        (void)flags;
        if (!detail::read_u64(p, end, packet.session)) return false;
        if (!detail::read_u64(p, end, packet.from)) return false;
        if (!detail::read_u64(p, end, packet.to)) return false;
        if (!detail::read_u32(p, end, packet.seq)) return false;
        if (!detail::read_u16(p, end, payload_size)) return false;
        if (p + payload_size > end) return false;
        packet.type = static_cast<MessageType>(type);
        packet.payload.assign(p, p + payload_size);
        return true;
    }

    struct Candidate
    {
        uint64_t peer_id = 0;
        Endpoint endpoint;
        bool relay_observed = false;
    };

    inline void append_endpoint(std::vector<uint8_t>& out, const Endpoint& endpoint)
    {
        detail::append_u32(out, ntohl(endpoint.addr.sin_addr.s_addr));
        detail::append_u16(out, endpoint.port());
    }

    inline bool read_endpoint(const uint8_t*& p, const uint8_t* end, Endpoint& endpoint)
    {
        uint32_t ip = 0;
        uint16_t port = 0;
        if (!detail::read_u32(p, end, ip) || !detail::read_u16(p, end, port))
            return false;
        endpoint.addr.sin_family = AF_INET;
        endpoint.addr.sin_addr.s_addr = htonl(ip);
        endpoint.addr.sin_port = htons(port);
        return true;
    }

    class RendezvousServer
    {
    public:
        struct Config
        {
            uint16_t port = 37640;
            uint64_t peer_timeout_ms = 30000;
        };

        explicit RendezvousServer(Config config = {}) : config_(config) {}
        ~RendezvousServer() { stop(); }

        bool start()
        {
            if (running_) return true;
            if (!socket_.open(config_.port, true))
                return false;
            running_ = true;
            worker_ = std::thread([this] { loop(); });
            return true;
        }

        void stop()
        {
            running_ = false;
            socket_.close();
            if (worker_.joinable())
                worker_.join();
        }

    private:
        struct PeerRecord
        {
            uint64_t session = 0;
            uint64_t peer_id = 0;
            Endpoint observed;
            Endpoint local;
            uint64_t last_seen_ms = 0;
        };

        struct PeerKey
        {
            uint64_t session = 0;
            uint64_t peer = 0;
            bool operator==(const PeerKey& other) const
            {
                return session == other.session && peer == other.peer;
            }
        };

        struct PeerKeyHash
        {
            size_t operator()(const PeerKey& key) const
            {
                return std::hash<uint64_t>{}(key.session ^ (key.peer + 0x9e3779b97f4a7c15ull));
            }
        };

        void loop()
        {
            uint8_t buffer[65536]{};
            while (running_)
            {
                Endpoint from;
                const int received = socket_.recv_from(buffer, sizeof(buffer), from);
                if (received <= 0)
                {
                    Platform::sleep_ms(4);
                    continue;
                }

                Packet packet;
                if (!decode_packet(buffer, static_cast<size_t>(received), packet))
                    continue;

                if (packet.type == MessageType::Hello || packet.type == MessageType::KeepAlive)
                {
                    update_peer(packet, from);
                    send_peer_list(packet.session, packet.from);
                }
                else if (packet.type == MessageType::Relay)
                {
                    relay(packet);
                }
                cleanup();
            }
        }

        void update_peer(const Packet& packet, const Endpoint& observed)
        {
            PeerRecord record;
            record.session = packet.session;
            record.peer_id = packet.from;
            record.observed = observed;
            record.local = observed;
            record.last_seen_ms = detail::now_ms();

            const uint8_t* p = packet.payload.data();
            const uint8_t* end = p + packet.payload.size();
            Endpoint local;
            if (read_endpoint(p, end, local))
                record.local = local;

            std::lock_guard<std::mutex> lock(mutex_);
            peers_[PeerKey{ packet.session, packet.from }] = record;
        }

        void send_peer_list(uint64_t session, uint64_t target_peer)
        {
            std::vector<PeerRecord> records;
            Endpoint target_endpoint;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto target_it = peers_.find(PeerKey{ session, target_peer });
                if (target_it == peers_.end())
                    return;
                target_endpoint = target_it->second.observed;
                for (const auto& item : peers_)
                {
                    if (item.second.session == session && item.second.peer_id != target_peer)
                        records.push_back(item.second);
                }
            }

            std::vector<uint8_t> payload;
            detail::append_u16(payload, static_cast<uint16_t>((std::min)(records.size(), size_t(128))));
            for (const PeerRecord& record : records)
            {
                detail::append_u64(payload, record.peer_id);
                append_endpoint(payload, record.observed);
                append_endpoint(payload, record.local);
            }

            Packet response;
            response.type = MessageType::PeerList;
            response.session = session;
            response.from = 0;
            response.to = target_peer;
            response.payload = std::move(payload);
            const auto bytes = encode_packet(response);
            socket_.send_to(target_endpoint, bytes.data(), bytes.size());
        }

        void relay(const Packet& packet)
        {
            Endpoint target;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = peers_.find(PeerKey{ packet.session, packet.to });
                if (it == peers_.end())
                    return;
                target = it->second.observed;
            }

            Packet relayed = packet;
            relayed.type = MessageType::RelayData;
            const auto bytes = encode_packet(relayed);
            socket_.send_to(target, bytes.data(), bytes.size());
        }

        void cleanup()
        {
            const uint64_t now = detail::now_ms();
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = peers_.begin(); it != peers_.end();)
            {
                if (now - it->second.last_seen_ms > config_.peer_timeout_ms)
                    it = peers_.erase(it);
                else
                    ++it;
            }
        }

        Config config_;
        UdpSocket socket_;
        std::atomic_bool running_{ false };
        std::thread worker_;
        std::mutex mutex_;
        std::unordered_map<PeerKey, PeerRecord, PeerKeyHash> peers_;
    };

    class Peer
    {
    public:
        struct Config
        {
            uint64_t session = detail::random_u64();
            uint64_t peer_id = detail::random_u64();
            std::string rendezvous_host = "127.0.0.1";
            uint16_t rendezvous_port = 37640;
            uint16_t local_port = 0;
            bool use_stun = true;
            std::string stun_host = "stun.l.google.com";
            uint16_t stun_port = 19302;
            uint64_t direct_timeout_ms = 5000;
        };

        explicit Peer(Config config) : config_(std::move(config)) {}
        ~Peer() { stop(); }

        Peer(const Peer&) = delete;
        Peer& operator=(const Peer&) = delete;

        void on_connected(std::function<void(Endpoint)> callback)
        {
            on_connected_ = std::move(callback);
        }

        void on_data(std::function<void(std::vector<uint8_t>, uint64_t)> callback)
        {
            on_data_ = std::move(callback);
        }

        bool start()
        {
            if (running_) return true;
            auto server = Endpoint::resolve(config_.rendezvous_host, config_.rendezvous_port);
            if (!server)
                return false;
            server_ = *server;

            if (!socket_.open(config_.local_port, true))
                return false;

            local_ = socket_.local_endpoint();
            if (config_.use_stun)
            {
                StunResult stun = StunClient::query(socket_, config_.stun_host, config_.stun_port);
                if (stun.ok)
                    mapped_ = stun.mapped;
            }

            running_ = true;
            worker_ = std::thread([this] { loop(); });
            return true;
        }

        void stop()
        {
            running_ = false;
            socket_.close();
            if (worker_.joinable())
                worker_.join();
        }

        bool connected() const { return direct_connected_; }
        uint64_t peer_id() const { return config_.peer_id; }
        uint64_t session() const { return config_.session; }
        Endpoint local_endpoint() const { return local_; }
        std::optional<Endpoint> mapped_endpoint() const { return mapped_; }

        bool send(const void* data, size_t size)
        {
            if (!data || size == 0 || size > 60000)
                return false;

            Packet packet;
            packet.type = direct_connected_ ? MessageType::Data : MessageType::Relay;
            packet.session = config_.session;
            packet.from = config_.peer_id;
            packet.to = remote_peer_;
            packet.seq = ++seq_;
            packet.payload.assign(
                reinterpret_cast<const uint8_t*>(data),
                reinterpret_cast<const uint8_t*>(data) + size);

            const auto bytes = encode_packet(packet);
            const Endpoint target = direct_connected_ ? remote_direct_ : server_;
            return socket_.send_to(target, bytes.data(), bytes.size()) > 0;
        }

        bool send(const std::vector<uint8_t>& data)
        {
            return send(data.data(), data.size());
        }

    private:
        void loop()
        {
            uint64_t last_hello = 0;
            uint64_t last_punch = 0;
            uint8_t buffer[65536]{};

            while (running_)
            {
                const uint64_t now = detail::now_ms();
                if (now - last_hello > 1000)
                {
                    send_hello(now == last_hello ? MessageType::Hello : MessageType::KeepAlive);
                    last_hello = now;
                }
                if (!direct_connected_ && now - last_punch > 120)
                {
                    punch_all();
                    last_punch = now;
                }

                Endpoint from;
                const int received = socket_.recv_from(buffer, sizeof(buffer), from);
                if (received > 0)
                {
                    Packet packet;
                    if (decode_packet(buffer, static_cast<size_t>(received), packet)
                        && packet.session == config_.session)
                    {
                        handle_packet(packet, from);
                    }
                }
                else
                {
                    Platform::sleep_ms(4);
                }
            }
        }

        void send_hello(MessageType type)
        {
            std::vector<uint8_t> payload;
            append_endpoint(payload, local_);

            Packet packet;
            packet.type = type;
            packet.session = config_.session;
            packet.from = config_.peer_id;
            packet.payload = std::move(payload);
            const auto bytes = encode_packet(packet);
            socket_.send_to(server_, bytes.data(), bytes.size());
        }

        void punch_all()
        {
            std::vector<Candidate> candidates;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                candidates = candidates_;
            }

            for (const Candidate& candidate : candidates)
            {
                Packet packet;
                packet.type = MessageType::Punch;
                packet.session = config_.session;
                packet.from = config_.peer_id;
                packet.to = candidate.peer_id;
                packet.seq = ++seq_;
                const auto bytes = encode_packet(packet);
                socket_.send_to(candidate.endpoint, bytes.data(), bytes.size());
            }
        }

        void handle_packet(const Packet& packet, const Endpoint& from)
        {
            switch (packet.type)
            {
            case MessageType::PeerList:
                update_candidates(packet.payload);
                break;
            case MessageType::Punch:
                remote_peer_ = packet.from;
                remote_direct_ = from;
                direct_connected_ = true;
                send_punch_ack(packet.from, from);
                if (on_connected_) on_connected_(from);
                break;
            case MessageType::PunchAck:
                remote_peer_ = packet.from;
                remote_direct_ = from;
                if (!direct_connected_ && on_connected_) on_connected_(from);
                direct_connected_ = true;
                break;
            case MessageType::Data:
            case MessageType::RelayData:
                remote_peer_ = packet.from;
                if (on_data_) on_data_(packet.payload, packet.from);
                break;
            default:
                break;
            }
        }

        void update_candidates(const std::vector<uint8_t>& payload)
        {
            const uint8_t* p = payload.data();
            const uint8_t* end = p + payload.size();
            uint16_t count = 0;
            if (!detail::read_u16(p, end, count))
                return;

            std::lock_guard<std::mutex> lock(mutex_);
            for (uint16_t i = 0; i < count; ++i)
            {
                uint64_t peer = 0;
                Endpoint observed;
                Endpoint local;
                if (!detail::read_u64(p, end, peer)
                    || !read_endpoint(p, end, observed)
                    || !read_endpoint(p, end, local))
                {
                    return;
                }
                remote_peer_ = peer;
                add_candidate_unlocked(peer, observed, true);
                add_candidate_unlocked(peer, local, false);
            }
        }

        void add_candidate_unlocked(uint64_t peer, Endpoint endpoint, bool observed)
        {
            if (endpoint.port() == 0)
                return;
            for (const Candidate& candidate : candidates_)
            {
                if (candidate.peer_id == peer && candidate.endpoint == endpoint)
                    return;
            }
            candidates_.push_back(Candidate{ peer, endpoint, observed });
        }

        void send_punch_ack(uint64_t peer, const Endpoint& target)
        {
            Packet packet;
            packet.type = MessageType::PunchAck;
            packet.session = config_.session;
            packet.from = config_.peer_id;
            packet.to = peer;
            packet.seq = ++seq_;
            const auto bytes = encode_packet(packet);
            socket_.send_to(target, bytes.data(), bytes.size());
        }

        Config config_;
        UdpSocket socket_;
        Endpoint server_;
        Endpoint local_;
        std::optional<Endpoint> mapped_;
        std::atomic_bool running_{ false };
        std::thread worker_;
        std::atomic_bool direct_connected_{ false };
        std::atomic<uint32_t> seq_{ 0 };
        uint64_t remote_peer_ = 0;
        Endpoint remote_direct_;
        std::mutex mutex_;
        std::vector<Candidate> candidates_;
        std::function<void(Endpoint)> on_connected_;
        std::function<void(std::vector<uint8_t>, uint64_t)> on_data_;
    };
}
