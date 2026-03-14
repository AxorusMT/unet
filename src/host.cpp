#include "unet/host.hpp"

#include "detail/address_native.hpp"
#include "detail/platform.hpp"
#include "detail/socket.hpp"
#include "detail/upnp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace unet {

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kMaxUdpPacketSize = 64 * 1024;

enum class PacketType : std::uint16_t {
    ConnectRequest = 1,
    ConnectAccept = 2,
    Disconnect = 3,
    Ping = 4,
    Pong = 5,
    Data = 6,
    AckOnly = 7
};

struct WireHeader {
    std::uint32_t protocol_id{};
    PacketType type{PacketType::AckOnly};
    PeerId peer_id{invalid_peer_id};
    std::uint8_t channel{};
    Delivery delivery{Delivery::ReliableOrdered};
    std::uint32_t packet_sequence{};
    std::uint32_t ack{};
    std::uint32_t ack_bits{};
    std::uint32_t message_sequence{};
    std::uint16_t fragment_index{};
    std::uint16_t fragment_count{};
    std::uint16_t payload_size{};
};

struct DecodedPacket {
    WireHeader header{};
    std::span<const std::byte> payload{};
};

constexpr std::size_t kWireHeaderSize = 36;

void write_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

void write_u32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

void write_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
    }
}

std::uint16_t read_u16(std::span<const std::byte> input, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[offset]) << 8) |
        static_cast<std::uint16_t>(input[offset + 1]));
}

std::uint32_t read_u32(std::span<const std::byte> input, std::size_t offset) {
    return (static_cast<std::uint32_t>(input[offset]) << 24) |
           (static_cast<std::uint32_t>(input[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(input[offset + 2]) << 8) |
           static_cast<std::uint32_t>(input[offset + 3]);
}

std::uint64_t read_u64(std::span<const std::byte> input, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(input[offset + i]);
    }
    return value;
}

std::vector<std::byte> serialize_packet(const WireHeader& header, std::span<const std::byte> payload) {
    std::vector<std::byte> bytes;
    bytes.reserve(kWireHeaderSize + payload.size());

    write_u32(bytes, header.protocol_id);
    write_u16(bytes, kProtocolVersion);
    write_u16(bytes, static_cast<std::uint16_t>(header.type));
    write_u16(bytes, header.peer_id);
    bytes.push_back(static_cast<std::byte>(header.channel));
    bytes.push_back(static_cast<std::byte>(header.delivery));
    write_u32(bytes, header.packet_sequence);
    write_u32(bytes, header.ack);
    write_u32(bytes, header.ack_bits);
    write_u32(bytes, header.message_sequence);
    write_u16(bytes, header.fragment_index);
    write_u16(bytes, header.fragment_count);
    write_u16(bytes, header.payload_size);
    write_u16(bytes, 0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::expected<DecodedPacket, Error> decode_packet(std::span<const std::byte> bytes) {
    if (bytes.size() < kWireHeaderSize) {
        return std::unexpected(Error{ErrorCode::InvalidPacket, "Datagram too short"});
    }

    DecodedPacket packet{};
    packet.header.protocol_id = read_u32(bytes, 0);
    const std::uint16_t version = read_u16(bytes, 4);
    if (version != kProtocolVersion) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Unsupported protocol version"});
    }

    packet.header.type = static_cast<PacketType>(read_u16(bytes, 6));
    packet.header.peer_id = read_u16(bytes, 8);
    packet.header.channel = static_cast<std::uint8_t>(bytes[10]);
    packet.header.delivery = static_cast<Delivery>(bytes[11]);
    packet.header.packet_sequence = read_u32(bytes, 12);
    packet.header.ack = read_u32(bytes, 16);
    packet.header.ack_bits = read_u32(bytes, 20);
    packet.header.message_sequence = read_u32(bytes, 24);
    packet.header.fragment_index = read_u16(bytes, 28);
    packet.header.fragment_count = read_u16(bytes, 30);
    packet.header.payload_size = read_u16(bytes, 32);

    const std::size_t payload_size = static_cast<std::size_t>(packet.header.payload_size);
    if (kWireHeaderSize + payload_size != bytes.size()) {
        return std::unexpected(Error{ErrorCode::InvalidPacket, "Invalid payload size"});
    }

    packet.payload = bytes.subspan(kWireHeaderSize, payload_size);
    return packet;
}

std::vector<std::vector<std::byte>> split_fragments(std::span<const std::byte> bytes, std::size_t fragment_size) {
    std::vector<std::vector<std::byte>> fragments;
    if (fragment_size == 0) {
        return fragments;
    }

    if (bytes.empty()) {
        fragments.emplace_back();
        return fragments;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t chunk = std::min(fragment_size, bytes.size() - offset);
        std::vector<std::byte> fragment(chunk);
        std::memcpy(fragment.data(), bytes.data() + offset, chunk);
        fragments.emplace_back(std::move(fragment));
        offset += chunk;
    }
    return fragments;
}

std::uint64_t make_message_key(std::uint8_t channel, std::uint32_t sequence) {
    return (static_cast<std::uint64_t>(channel) << 32) | sequence;
}

std::uint64_t make_assembly_key(std::uint8_t channel, Delivery delivery, std::uint32_t sequence) {
    return (static_cast<std::uint64_t>(channel) << 56) |
           (static_cast<std::uint64_t>(static_cast<std::uint8_t>(delivery)) << 48) |
           sequence;
}

bool all_true(const std::vector<bool>& bits) {
    return std::all_of(bits.begin(), bits.end(), [](bool bit) { return bit; });
}

std::optional<Delivery> delivery_from_byte(std::uint8_t value) {
    switch (static_cast<Delivery>(value)) {
    case Delivery::Unreliable:
    case Delivery::UnreliableSequenced:
    case Delivery::ReliableOrdered:
        return static_cast<Delivery>(value);
    default:
        return std::nullopt;
    }
}

constexpr std::uint32_t kFileMagic = 0x55465430u;  // UFT0

enum class FileControlType : std::uint8_t {
    Offer = 1,
    Accept = 2,
    Reject = 3,
    Chunk = 4,
    Complete = 5
};

std::optional<FileControlType> file_control_type(std::uint8_t value) {
    switch (static_cast<FileControlType>(value)) {
    case FileControlType::Offer:
    case FileControlType::Accept:
    case FileControlType::Reject:
    case FileControlType::Chunk:
    case FileControlType::Complete:
        return static_cast<FileControlType>(value);
    default:
        return std::nullopt;
    }
}

std::vector<std::byte> encode_file_offer(std::uint64_t transfer_id, std::uint64_t total_bytes, std::string_view filename) {
    std::vector<std::byte> payload;
    payload.reserve(4 + 1 + 8 + 8 + 2 + filename.size());
    write_u32(payload, kFileMagic);
    payload.push_back(static_cast<std::byte>(FileControlType::Offer));
    write_u64(payload, transfer_id);
    write_u64(payload, total_bytes);
    write_u16(payload, static_cast<std::uint16_t>(filename.size()));
    for (char c : filename) {
        payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return payload;
}

std::vector<std::byte> encode_file_accept(std::uint64_t transfer_id) {
    std::vector<std::byte> payload;
    payload.reserve(4 + 1 + 8);
    write_u32(payload, kFileMagic);
    payload.push_back(static_cast<std::byte>(FileControlType::Accept));
    write_u64(payload, transfer_id);
    return payload;
}

std::vector<std::byte> encode_file_reject(std::uint64_t transfer_id, std::string_view reason) {
    std::vector<std::byte> payload;
    payload.reserve(4 + 1 + 8 + 2 + reason.size());
    write_u32(payload, kFileMagic);
    payload.push_back(static_cast<std::byte>(FileControlType::Reject));
    write_u64(payload, transfer_id);
    write_u16(payload, static_cast<std::uint16_t>(reason.size()));
    for (char c : reason) {
        payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return payload;
}

std::vector<std::byte> encode_file_chunk(std::uint64_t transfer_id, std::uint64_t offset, std::span<const std::byte> chunk) {
    std::vector<std::byte> payload;
    payload.reserve(4 + 1 + 8 + 8 + 2 + chunk.size());
    write_u32(payload, kFileMagic);
    payload.push_back(static_cast<std::byte>(FileControlType::Chunk));
    write_u64(payload, transfer_id);
    write_u64(payload, offset);
    write_u16(payload, static_cast<std::uint16_t>(chunk.size()));
    payload.insert(payload.end(), chunk.begin(), chunk.end());
    return payload;
}

std::vector<std::byte> encode_file_complete(std::uint64_t transfer_id) {
    std::vector<std::byte> payload;
    payload.reserve(4 + 1 + 8);
    write_u32(payload, kFileMagic);
    payload.push_back(static_cast<std::byte>(FileControlType::Complete));
    write_u64(payload, transfer_id);
    return payload;
}

std::string decode_text(std::span<const std::byte> payload, std::size_t offset, std::size_t size) {
    std::string text(size, '\0');
    if (size > 0) {
        std::memcpy(text.data(), payload.data() + offset, size);
    }
    return text;
}

std::filesystem::path sanitize_filename(std::string_view name) {
    std::filesystem::path raw(name);
    std::string filename = raw.filename().string();
    if (filename.empty()) {
        filename = "download.bin";
    }
    return std::filesystem::path(filename);
}

constexpr std::size_t kTcpFrameHeaderSize = 2;

bool is_would_block_error(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK;
#else
    return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

bool is_connect_in_progress_error(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
    return err == EINPROGRESS || err == EALREADY || err == EWOULDBLOCK || err == EAGAIN;
#endif
}

std::expected<detail::SocketHandle, Error> open_tcp_socket(std::string_view bind_ip, std::uint16_t port, bool enable_ipv6) {
    const auto resolved = Address::resolve(bind_ip, port, enable_ipv6);
    if (!resolved.has_value()) {
        return std::unexpected(resolved.error());
    }

    const Address bind_address = *resolved;
    const sockaddr* native = detail::NativeAddressAccess::sockaddr_ref(bind_address);
    const int family = native->sa_family;

    detail::SocketHandle handle = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (handle == detail::kInvalidSocket) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOpenFailed,
            .message = detail::socket_error_message(detail::last_socket_error())
        });
    }

    const int one = 1;
    (void)setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), static_cast<detail::SockLen>(sizeof(one)));

    if (const auto non_blocking = detail::set_non_blocking(handle); !non_blocking.has_value()) {
        const Error error = non_blocking.error();
        detail::close_socket(handle);
        return std::unexpected(error);
    }

#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
    if (family == AF_INET6) {
        const int dual_stack = 0;
        (void)setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&dual_stack), static_cast<detail::SockLen>(sizeof(dual_stack)));
    }
#endif

    if (::bind(handle, native, detail::NativeAddressAccess::length(bind_address)) != 0) {
        const Error error{
            .code = ErrorCode::SocketBindFailed,
            .message = detail::socket_error_message(detail::last_socket_error())
        };
        detail::close_socket(handle);
        return std::unexpected(error);
    }

    return handle;
}

std::expected<std::optional<std::pair<detail::SocketHandle, Address>>, Error> tcp_accept(
    detail::SocketHandle listen_socket) {
    Address address{};
    detail::SockLen len = detail::NativeAddressAccess::capacity();
    detail::SocketHandle client = ::accept(
        listen_socket,
        detail::NativeAddressAccess::sockaddr_mut(address),
        &len);
    if (client == detail::kInvalidSocket) {
        const int err = detail::last_socket_error();
        if (is_would_block_error(err)) {
            return std::optional<std::pair<detail::SocketHandle, Address>>{};
        }
        return std::unexpected(Error{
            .code = ErrorCode::SocketReceiveFailed,
            .message = detail::socket_error_message(err)
        });
    }

    if (const auto non_blocking = detail::set_non_blocking(client); !non_blocking.has_value()) {
        const Error error = non_blocking.error();
        detail::close_socket(client);
        return std::unexpected(error);
    }

    detail::NativeAddressAccess::set_length(address, len);
    return std::optional<std::pair<detail::SocketHandle, Address>>(std::pair<detail::SocketHandle, Address>{client, address});
}

std::expected<std::optional<std::size_t>, Error> tcp_receive_some(
    detail::SocketHandle socket,
    std::span<std::byte> buffer) {
    const int received = ::recv(
        socket,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0);
    if (received > 0) {
        return std::optional<std::size_t>(static_cast<std::size_t>(received));
    }
    if (received == 0) {
        return std::optional<std::size_t>(0);
    }

    const int err = detail::last_socket_error();
    if (is_would_block_error(err)) {
        return std::optional<std::size_t>{};
    }
    return std::unexpected(Error{
        .code = ErrorCode::SocketReceiveFailed,
        .message = detail::socket_error_message(err)
    });
}

std::expected<std::optional<std::size_t>, Error> tcp_send_some(
    detail::SocketHandle socket,
    std::span<const std::byte> buffer) {
    const int sent = ::send(
        socket,
        reinterpret_cast<const char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0);
    if (sent > 0) {
        return std::optional<std::size_t>(static_cast<std::size_t>(sent));
    }
    if (sent == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketSendFailed,
            .message = "TCP connection closed"
        });
    }

    const int err = detail::last_socket_error();
    if (is_would_block_error(err)) {
        return std::optional<std::size_t>{};
    }
    return std::unexpected(Error{
        .code = ErrorCode::SocketSendFailed,
        .message = detail::socket_error_message(err)
    });
}

std::expected<bool, Error> tcp_connect_finished(detail::SocketHandle socket) {
    fd_set writes{};
    fd_set errors{};
    FD_ZERO(&writes);
    FD_ZERO(&errors);
    FD_SET(socket, &writes);
    FD_SET(socket, &errors);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    const int selected = ::select(
        static_cast<int>(socket + 1),
        nullptr,
        &writes,
        &errors,
        &timeout);
    if (selected < 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOpenFailed,
            .message = detail::socket_error_message(detail::last_socket_error())
        });
    }
    if (selected == 0) {
        return false;
    }

    int so_error = 0;
    detail::SockLen so_len = static_cast<detail::SockLen>(sizeof(so_error));
    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len) != 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOpenFailed,
            .message = detail::socket_error_message(detail::last_socket_error())
        });
    }
    if (so_error != 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOpenFailed,
            .message = detail::socket_error_message(so_error)
        });
    }
    return true;
}

}  // namespace

class Host::Impl {
public:
    explicit Impl(HostConfig config)
        : config_(std::move(config)),
          peers_(static_cast<std::size_t>(config_.max_peers) + 1) {}

    ~Impl() {
        clear_upnp_mapping();
        for (auto& peer : peers_) {
            if (peer.has_value() && peer->tcp_socket != detail::kInvalidSocket) {
                detail::close_socket(peer->tcp_socket);
                peer->tcp_socket = detail::kInvalidSocket;
            }
        }
        if (tcp_listen_socket_ != detail::kInvalidSocket) {
            detail::close_socket(tcp_listen_socket_);
            tcp_listen_socket_ = detail::kInvalidSocket;
        }
        socket_.close();
        if (socket_runtime_ready_) {
            detail::shutdown_sockets();
            socket_runtime_ready_ = false;
        }
    }

    enum class Mode {
        Stopped,
        Server,
        Client,
        Peer
    };

    struct ChannelState {
        std::uint32_t next_reliable_send{};
        std::uint32_t next_reliable_recv{};
        std::uint32_t next_unreliable_send{};
        bool has_last_unreliable_recv{};
        std::uint32_t last_unreliable_recv{};
        std::map<std::uint32_t, std::vector<std::byte>> pending_ordered{};
    };

    struct ReliableRef {
        std::uint8_t channel{};
        std::uint32_t sequence{};
        std::uint16_t fragment{};
    };

    struct SentPacket {
        TimePoint sent_at{};
        std::optional<ReliableRef> reliable_ref{};
    };

    struct ReliableMessage {
        std::uint8_t channel{};
        std::uint32_t sequence{};
        std::vector<std::vector<std::byte>> fragments{};
        std::vector<bool> acked{};
        std::vector<TimePoint> last_sent{};
    };

    struct Assembly {
        Delivery delivery{Delivery::Unreliable};
        std::uint8_t channel{};
        std::uint32_t sequence{};
        std::uint16_t fragment_count{};
        std::vector<std::vector<std::byte>> fragments{};
        std::vector<bool> received{};
        TimePoint updated_at{};
    };

    struct Peer {
        enum class State {
            Connecting,
            Connected
        };

        PeerId id{invalid_peer_id};
        PeerId wire_id{invalid_peer_id};
        Address address{};
        bool outgoing{};
        State state{State::Connecting};

        TimePoint created_at{};
        TimePoint last_recv{};
        TimePoint last_send{};
        TimePoint last_connect_attempt{};
        bool ack_dirty{};

        std::uint32_t next_packet_sequence{1};
        bool has_remote_sequence{};
        std::uint32_t remote_sequence{};
        std::uint32_t remote_ack_bits{};

        std::vector<ChannelState> channels{};
        std::unordered_map<std::uint32_t, SentPacket> sent_packets{};
        std::deque<std::uint32_t> sent_packet_order{};
        std::unordered_map<std::uint64_t, ReliableMessage> reliable_outgoing{};
        std::unordered_map<std::uint64_t, Assembly> assemblies{};
        detail::SocketHandle tcp_socket{detail::kInvalidSocket};
        bool tcp_connect_in_progress{false};
        std::vector<std::byte> tcp_recv_buffer{};
        std::vector<std::byte> tcp_send_buffer{};
        std::size_t tcp_send_offset{};
        PeerStats stats{};
    };

    struct TransferKey {
        PeerId peer{invalid_peer_id};
        std::uint64_t transfer_id{};

        bool operator==(const TransferKey& rhs) const noexcept {
            return peer == rhs.peer && transfer_id == rhs.transfer_id;
        }
    };

    struct TransferKeyHash {
        std::size_t operator()(const TransferKey& key) const noexcept {
            const std::size_t a = std::hash<std::uint64_t>{}(key.transfer_id);
            const std::size_t b = std::hash<std::uint16_t>{}(key.peer);
            return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2));
        }
    };

    struct UploadTransfer {
        PeerId peer{invalid_peer_id};
        std::uint64_t transfer_id{};
        std::filesystem::path local_path{};
        std::string remote_filename{};
        std::uint64_t total_bytes{};
        std::uint64_t bytes_sent{};
        bool accepted{};
        bool completion_notified{};
        std::ifstream stream{};
    };

    struct DownloadTransfer {
        PeerId peer{invalid_peer_id};
        std::uint64_t transfer_id{};
        std::string filename{};
        std::uint64_t total_bytes{};
        std::uint64_t bytes_received{};
        bool accepted{};
        std::filesystem::path destination_path{};
        std::ofstream stream{};
    };

    [[nodiscard]] std::expected<void, Error> validate_config() const;
    [[nodiscard]] std::expected<void, Error> open_listen_socket(std::uint16_t port, std::string_view bind_ip);
    void push_event(Event event);

    [[nodiscard]] std::expected<void, Error> ensure_socket_runtime();
    [[nodiscard]] std::expected<void, Error> start_server(std::uint16_t port, std::string_view bind_ip);
    [[nodiscard]] std::expected<void, Error> start_p2p(std::uint16_t port, std::string_view bind_ip);
    [[nodiscard]] std::expected<PeerId, Error> connect(const Address& remote);
    [[nodiscard]] std::expected<void, Error> send(PeerId id, std::span<const std::byte> bytes, SendOptions options);
    [[nodiscard]] std::expected<std::uint64_t, Error> send_file(PeerId peer, const std::filesystem::path& local_path, FileSendOptions options);
    [[nodiscard]] std::expected<void, Error> accept_file(PeerId peer, std::uint64_t transfer_id, const std::filesystem::path& destination_path);
    [[nodiscard]] std::expected<void, Error> reject_file(PeerId peer, std::uint64_t transfer_id, std::string_view reason);
    [[nodiscard]] std::expected<void, Error> disconnect(PeerId id, DisconnectReason reason);

    void service();
    [[nodiscard]] std::optional<Event> poll_event();

    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] bool is_server() const noexcept { return mode_ == Mode::Server; }
    [[nodiscard]] std::vector<PeerId> connected_peers() const;
    [[nodiscard]] std::optional<Address> peer_address(PeerId id) const;
    [[nodiscard]] std::optional<PeerStats> peer_stats(PeerId id) const;
    [[nodiscard]] std::optional<Address> upnp_external_address() const;
    [[nodiscard]] std::optional<Error> upnp_last_error() const;

private:
    [[nodiscard]] bool using_tcp() const noexcept {
        return config_.transport == Transport::Tcp;
    }

    [[nodiscard]] PeerId allocate_peer_id() const;
    [[nodiscard]] Peer* peer_by_id(PeerId id);
    [[nodiscard]] const Peer* peer_by_id(PeerId id) const;
    [[nodiscard]] Peer* peer_by_address(const Address& address);

    [[nodiscard]] std::expected<void, Error> send_packet(
        Peer& peer,
        PacketType type,
        std::uint8_t channel,
        Delivery delivery,
        std::uint32_t message_sequence,
        std::uint16_t fragment_index,
        std::uint16_t fragment_count,
        std::span<const std::byte> payload,
        std::optional<ReliableRef> reliable_ref);

    [[nodiscard]] std::expected<void, Error> send_connect_request(Peer& peer);
    [[nodiscard]] std::expected<void, Error> send_connect_accept(Peer& peer);
    [[nodiscard]] std::expected<void, Error> send_ping(Peer& peer);
    [[nodiscard]] std::expected<void, Error> send_pong(Peer& peer, std::span<const std::byte> payload);
    [[nodiscard]] std::expected<void, Error> send_data_fragment(
        Peer& peer,
        Delivery delivery,
        std::uint8_t channel,
        std::uint32_t sequence,
        std::uint16_t fragment_index,
        std::uint16_t fragment_count,
        std::span<const std::byte> payload,
        std::optional<ReliableRef> reliable_ref);

    void receive_loop();
    void receive_loop_udp();
    void receive_loop_tcp();
    void accept_tcp_peers();
    [[nodiscard]] std::expected<void, Error> flush_tcp_send(Peer& peer);
    void process_datagram(const Address& from, std::span<const std::byte> bytes);
    void handle_connect_request(const Address& from);
    void handle_connect_accept(Peer& peer, const WireHeader& header);
    void handle_disconnect(Peer& peer, std::span<const std::byte> payload);
    void handle_data(Peer& peer, const WireHeader& header, std::span<const std::byte> payload);
    void process_message(Peer& peer, std::uint8_t channel_index, Delivery delivery, std::uint32_t sequence, std::span<const std::byte> payload);
    void emit_message(PeerId id, std::uint8_t channel, Delivery delivery, std::span<const std::byte> payload);
    void register_remote_sequence(Peer& peer, std::uint32_t sequence);
    void apply_acks(Peer& peer, std::uint32_t ack, std::uint32_t ack_bits);
    void ack_packet(Peer& peer, std::uint32_t sequence);
    void resend_reliable(Peer& peer, TimePoint now);
    void cleanup_assemblies(Peer& peer, TimePoint now);
    void remove_peer(PeerId id, DisconnectReason reason);
    void pump_uploads();
    void remove_peer_transfers(PeerId peer);
    [[nodiscard]] std::uint8_t file_channel() const;
    [[nodiscard]] bool is_file_payload(std::span<const std::byte> payload) const;
    [[nodiscard]] bool handle_file_control(Peer& peer, std::span<const std::byte> payload);
    [[nodiscard]] std::expected<void, Error> send_file_control(PeerId peer, std::span<const std::byte> payload);
    [[nodiscard]] std::expected<void, Error> begin_download(PeerId peer, std::uint64_t transfer_id, const std::filesystem::path& destination_path);
    void emit_file_offer(PeerId peer, std::uint64_t transfer_id, std::string filename, std::uint64_t total_bytes, bool auto_accepted);
    void emit_file_progress(PeerId peer, std::uint64_t transfer_id, std::uint64_t transferred, std::uint64_t total, bool upload);
    void emit_file_complete(PeerId peer, std::uint64_t transfer_id, std::string filename, std::uint64_t total_bytes, std::string path, bool upload);
    void emit_file_rejected(PeerId peer, std::uint64_t transfer_id, std::string reason);
    [[nodiscard]] std::expected<void, Error> setup_upnp_mapping(std::uint16_t listen_port, std::string_view bind_ip);
    void clear_upnp_mapping() noexcept;

    HostConfig config_{};
    bool running_{false};
    bool socket_runtime_ready_{false};
    bool accepts_incoming_{false};
    Mode mode_{Mode::Stopped};
    detail::UdpSocket socket_{};
    detail::SocketHandle tcp_listen_socket_{detail::kInvalidSocket};
    std::vector<std::optional<Peer>> peers_{};
    std::deque<Event> events_{};
    std::unordered_map<std::uint64_t, UploadTransfer> uploads_{};
    std::unordered_map<TransferKey, DownloadTransfer, TransferKeyHash> downloads_{};
    std::uint64_t next_transfer_id_{1};
    std::optional<detail::UpnpMapping> upnp_mapping_{};
    std::optional<Error> upnp_last_error_{};
};

std::expected<void, Error> Host::Impl::ensure_socket_runtime() {
    if (socket_runtime_ready_) {
        return {};
    }

    auto init = detail::initialize_sockets();
    if (!init.has_value()) {
        return std::unexpected(init.error());
    }
    socket_runtime_ready_ = true;
    return {};
}

std::expected<void, Error> Host::Impl::validate_config() const {
    if (config_.transport != Transport::Udp &&
        config_.transport != Transport::Tcp &&
        config_.transport != Transport::Quic) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig.transport must be UDP, TCP, or QUIC"
        });
    }
    if (config_.max_peers == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig.max_peers must be >= 1"
        });
    }
    if (config_.channel_count == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig.channel_count must be >= 1"
        });
    }
    if (config_.mtu <= kWireHeaderSize) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig.mtu must be greater than protocol header size"
        });
    }
    if (config_.max_fragments_per_message == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig.max_fragments_per_message must be >= 1"
        });
    }
    if (config_.max_fragments_per_message > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig.max_fragments_per_message exceeds wire protocol limits"
        });
    }
    if (config_.max_pending_reliable_messages == 0 ||
        config_.max_pending_reassembly_messages == 0 ||
        config_.max_sent_packet_history == 0 ||
        config_.max_event_queue == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig queue/history limits must be >= 1"
        });
    }
    if (config_.heartbeat_interval <= std::chrono::milliseconds::zero() ||
        config_.ack_flush_interval <= std::chrono::milliseconds::zero() ||
        config_.disconnect_timeout <= std::chrono::milliseconds::zero() ||
        config_.reliable_resend_interval <= std::chrono::milliseconds::zero() ||
        config_.connect_retry_interval <= std::chrono::milliseconds::zero() ||
        config_.fragment_timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig timing intervals must be > 0"
        });
    }
    if (config_.file_chunk_size == 0 || config_.file_chunk_size > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "HostConfig.file_chunk_size must be in range [1, 65535]"
        });
    }
    if (config_.enable_upnp) {
        if (config_.upnp_discovery_timeout <= std::chrono::milliseconds::zero()) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "HostConfig.upnp_discovery_timeout must be > 0"
            });
        }
        if (config_.upnp_discovery_address.empty()) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "HostConfig.upnp_discovery_address must not be empty"
            });
        }
    }
    return {};
}

std::expected<void, Error> Host::Impl::open_listen_socket(std::uint16_t port, std::string_view bind_ip) {
    const bool explicit_ipv6 = bind_ip.find(':') != std::string_view::npos;
    const bool prefer_ipv6 = explicit_ipv6 || bind_ip == "::" || (bind_ip.empty() && config_.enable_ipv6);
    if (!using_tcp()) {
        if (const auto opened = socket_.open(bind_ip, port, prefer_ipv6); !opened.has_value()) {
            const bool can_fallback_to_ipv4 = (bind_ip == "::" || bind_ip.empty()) && prefer_ipv6;
            if (!can_fallback_to_ipv4) {
                return std::unexpected(opened.error());
            }
            if (const auto fallback = socket_.open("0.0.0.0", port, false); !fallback.has_value()) {
                return std::unexpected(fallback.error());
            }
        }
        return {};
    }

    auto open_tcp = [&](std::string_view candidate_bind, bool candidate_ipv6) -> std::expected<detail::SocketHandle, Error> {
        auto opened = open_tcp_socket(candidate_bind, port, candidate_ipv6);
        if (!opened.has_value()) {
            return std::unexpected(opened.error());
        }
        if (::listen(*opened, SOMAXCONN) != 0) {
            const Error error{
                .code = ErrorCode::SocketBindFailed,
                .message = detail::socket_error_message(detail::last_socket_error())
            };
            detail::close_socket(*opened);
            return std::unexpected(error);
        }
        return *opened;
    };

    auto opened = open_tcp(bind_ip, prefer_ipv6);
    if (!opened.has_value()) {
        const bool can_fallback_to_ipv4 = (bind_ip == "::" || bind_ip.empty()) && prefer_ipv6;
        if (!can_fallback_to_ipv4) {
            return std::unexpected(opened.error());
        }
        opened = open_tcp("0.0.0.0", false);
        if (!opened.has_value()) {
            return std::unexpected(opened.error());
        }
    }

    if (tcp_listen_socket_ != detail::kInvalidSocket) {
        detail::close_socket(tcp_listen_socket_);
        tcp_listen_socket_ = detail::kInvalidSocket;
    }
    tcp_listen_socket_ = *opened;
    return {};
}

std::expected<void, Error> Host::Impl::setup_upnp_mapping(std::uint16_t listen_port, std::string_view bind_ip) {
    if (!config_.enable_upnp) {
        return {};
    }
    if (listen_port == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpControlFailed,
            .message = "UPnP requires a non-zero listen port"
        });
    }

    detail::UpnpRequest request{};
    request.internal_port = listen_port;
    request.external_port = config_.upnp_external_port;
    request.protocol = using_tcp() ? "TCP" : "UDP";
    request.description = config_.upnp_description;
    request.lease_duration = config_.upnp_lease_duration;
    request.discovery_address = config_.upnp_discovery_address;
    request.discovery_port = config_.upnp_discovery_port;
    request.discovery_timeout = config_.upnp_discovery_timeout;

    if (!bind_ip.empty() &&
        bind_ip != "0.0.0.0" &&
        bind_ip != "::" &&
        bind_ip.find(':') == std::string_view::npos) {
        request.internal_client = std::string(bind_ip);
    }

    auto mapped = detail::upnp_add_port_mapping(request);
    if (!mapped.has_value()) {
        return std::unexpected(mapped.error());
    }

    upnp_mapping_ = *mapped;
    return {};
}

void Host::Impl::clear_upnp_mapping() noexcept {
    if (upnp_mapping_.has_value()) {
        (void)detail::upnp_remove_port_mapping(*upnp_mapping_, config_.upnp_discovery_timeout);
        upnp_mapping_.reset();
    }
}

void Host::Impl::push_event(Event event) {
    if (events_.size() >= config_.max_event_queue) {
        events_.pop_front();
    }
    events_.push_back(std::move(event));
}

std::expected<void, Error> Host::Impl::start_server(std::uint16_t port, std::string_view bind_ip) {
    if (running_) {
        return std::unexpected(Error{ErrorCode::InvalidState, "Host is already running"});
    }
    if (const auto valid = validate_config(); !valid.has_value()) {
        return std::unexpected(valid.error());
    }

    if (const auto runtime = ensure_socket_runtime(); !runtime.has_value()) {
        return std::unexpected(runtime.error());
    }
    if (const auto opened = open_listen_socket(port, bind_ip); !opened.has_value()) {
        return std::unexpected(opened.error());
    }

    clear_upnp_mapping();
    upnp_last_error_.reset();
    if (config_.enable_upnp) {
        auto upnp = setup_upnp_mapping(port, bind_ip);
        if (!upnp.has_value()) {
            upnp_last_error_ = upnp.error();
            if (config_.require_upnp) {
                if (using_tcp()) {
                    if (tcp_listen_socket_ != detail::kInvalidSocket) {
                        detail::close_socket(tcp_listen_socket_);
                        tcp_listen_socket_ = detail::kInvalidSocket;
                    }
                } else {
                    socket_.close();
                }
                return std::unexpected(upnp.error());
            }
        }
    }

    running_ = true;
    accepts_incoming_ = true;
    mode_ = Mode::Server;
    return {};
}

std::expected<void, Error> Host::Impl::start_p2p(std::uint16_t port, std::string_view bind_ip) {
    if (running_) {
        return std::unexpected(Error{ErrorCode::InvalidState, "Host is already running"});
    }
    if (const auto valid = validate_config(); !valid.has_value()) {
        return std::unexpected(valid.error());
    }

    if (const auto runtime = ensure_socket_runtime(); !runtime.has_value()) {
        return std::unexpected(runtime.error());
    }
    if (const auto opened = open_listen_socket(port, bind_ip); !opened.has_value()) {
        return std::unexpected(opened.error());
    }

    clear_upnp_mapping();
    upnp_last_error_.reset();
    if (config_.enable_upnp) {
        auto upnp = setup_upnp_mapping(port, bind_ip);
        if (!upnp.has_value()) {
            upnp_last_error_ = upnp.error();
            if (config_.require_upnp) {
                if (using_tcp()) {
                    if (tcp_listen_socket_ != detail::kInvalidSocket) {
                        detail::close_socket(tcp_listen_socket_);
                        tcp_listen_socket_ = detail::kInvalidSocket;
                    }
                } else {
                    socket_.close();
                }
                return std::unexpected(upnp.error());
            }
        }
    }

    running_ = true;
    accepts_incoming_ = true;
    mode_ = Mode::Peer;
    return {};
}

std::expected<PeerId, Error> Host::Impl::connect(const Address& remote) {
    if (!remote.is_valid()) {
        return std::unexpected(Error{ErrorCode::AddressResolutionFailed, "Remote address is invalid"});
    }
    if (const auto valid = validate_config(); !valid.has_value()) {
        return std::unexpected(valid.error());
    }

    if (Peer* existing = peer_by_address(remote); existing != nullptr) {
        return existing->id;
    }

    if (!running_) {
        if (const auto runtime = ensure_socket_runtime(); !runtime.has_value()) {
            return std::unexpected(runtime.error());
        }
        if (!using_tcp()) {
            const bool remote_is_ipv6 = remote.ip().find(':') != std::string::npos;
            const std::string_view bind_ip = remote_is_ipv6 ? "::" : "0.0.0.0";
            if (const auto opened = socket_.open(bind_ip, 0, remote_is_ipv6); !opened.has_value()) {
                return std::unexpected(opened.error());
            }
        }
        running_ = true;
        accepts_incoming_ = false;
        mode_ = Mode::Client;
    }

    const PeerId id = allocate_peer_id();
    if (id == invalid_peer_id) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, "No free peer slots available"});
    }

    const TimePoint now = Clock::now();
    Peer peer{};
    peer.id = id;
    peer.address = remote;
    peer.outgoing = true;
    peer.state = Peer::State::Connecting;
    peer.created_at = now;
    peer.last_recv = now;
    peer.last_send = now;
    peer.last_connect_attempt = TimePoint{};
    peer.channels.resize(config_.channel_count);
    if (using_tcp()) {
        const sockaddr* native = detail::NativeAddressAccess::sockaddr_ref(remote);
        detail::SocketHandle tcp_socket = ::socket(native->sa_family, SOCK_STREAM, IPPROTO_TCP);
        if (tcp_socket == detail::kInvalidSocket) {
            return std::unexpected(Error{
                .code = ErrorCode::SocketOpenFailed,
                .message = detail::socket_error_message(detail::last_socket_error())
            });
        }
        if (const auto non_blocking = detail::set_non_blocking(tcp_socket); !non_blocking.has_value()) {
            const Error error = non_blocking.error();
            detail::close_socket(tcp_socket);
            return std::unexpected(error);
        }

        const int connected = ::connect(
            tcp_socket,
            detail::NativeAddressAccess::sockaddr_ref(remote),
            detail::NativeAddressAccess::length(remote));
        if (connected != 0) {
            const int err = detail::last_socket_error();
            if (!is_connect_in_progress_error(err)) {
                detail::close_socket(tcp_socket);
                return std::unexpected(Error{
                    .code = ErrorCode::SocketOpenFailed,
                    .message = detail::socket_error_message(err)
                });
            }
            peer.tcp_connect_in_progress = true;
        }
        peer.tcp_socket = tcp_socket;
    }

    peers_[id] = std::move(peer);

    if (using_tcp()) {
        if (!peers_[id]->tcp_connect_in_progress) {
            (void)send_connect_request(*peers_[id]);
        }
    } else {
        (void)send_connect_request(*peers_[id]);
    }
    return id;
}

std::expected<void, Error> Host::Impl::send(PeerId id, std::span<const std::byte> bytes, SendOptions options) {
    Peer* peer = peer_by_id(id);
    if (peer == nullptr) {
        return std::unexpected(Error{ErrorCode::InvalidPeer, "Unknown peer id"});
    }
    if (peer->state != Peer::State::Connected) {
        return std::unexpected(Error{ErrorCode::InvalidState, "Peer is not connected"});
    }
    if (options.channel >= config_.channel_count) {
        return std::unexpected(Error{ErrorCode::InvalidChannel, "Invalid channel id"});
    }
    if (config_.mtu <= kWireHeaderSize) {
        return std::unexpected(Error{ErrorCode::InvalidState, "MTU is too small"});
    }

    const std::size_t fragment_payload = config_.mtu - kWireHeaderSize;
    const std::size_t fragment_count = bytes.empty()
        ? 1
        : (1 + ((bytes.size() - 1) / fragment_payload));
    if (fragment_count > config_.max_fragments_per_message) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "Message exceeds max_fragments_per_message"});
    }
    if (fragment_count > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "Message requires too many fragments"});
    }
    const std::uint16_t fragment_count_u16 = static_cast<std::uint16_t>(fragment_count);

    ChannelState& channel = peer->channels[options.channel];
    std::uint32_t sequence = 0;

    if (options.delivery == Delivery::ReliableOrdered) {
        if (peer->reliable_outgoing.size() >= config_.max_pending_reliable_messages) {
            return std::unexpected(Error{
                ErrorCode::CapacityExceeded,
                "Peer reached max pending reliable message limit"
            });
        }

        sequence = channel.next_reliable_send++;
        auto fragments = split_fragments(bytes, fragment_payload);

        ReliableMessage message{};
        message.channel = options.channel;
        message.sequence = sequence;
        message.fragments = std::move(fragments);
        message.acked.assign(fragment_count, false);
        message.last_sent.assign(fragment_count, TimePoint{});

        const std::uint64_t key = make_message_key(options.channel, sequence);
        auto [message_it, _] = peer->reliable_outgoing.emplace(key, std::move(message));
        ReliableMessage& stored = message_it->second;

        for (std::uint16_t i = 0; i < fragment_count_u16; ++i) {
            auto result = send_data_fragment(
                *peer,
                options.delivery,
                options.channel,
                sequence,
                i,
                fragment_count_u16,
                stored.fragments[i],
                ReliableRef{options.channel, sequence, i});
            if (!result.has_value()) {
                peer->reliable_outgoing.erase(message_it);
                return std::unexpected(result.error());
            }
            stored.last_sent[i] = Clock::now();
        }
        return {};
    }

    if (options.delivery == Delivery::UnreliableSequenced) {
        sequence = channel.next_unreliable_send++;
    }

    if (bytes.empty()) {
        auto result = send_data_fragment(
            *peer,
            options.delivery,
            options.channel,
            sequence,
            0,
            1,
            {},
            std::nullopt);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        return {};
    }

    std::size_t offset = 0;
    std::uint16_t fragment_index = 0;
    while (offset < bytes.size()) {
        const std::size_t chunk = std::min(fragment_payload, bytes.size() - offset);
        auto result = send_data_fragment(
            *peer,
            options.delivery,
            options.channel,
            sequence,
            fragment_index,
            fragment_count_u16,
            bytes.subspan(offset, chunk),
            std::nullopt);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        offset += chunk;
        fragment_index += 1;
    }

    return {};
}

std::expected<std::uint64_t, Error> Host::Impl::send_file(PeerId peer_id, const std::filesystem::path& local_path, FileSendOptions options) {
    Peer* peer = peer_by_id(peer_id);
    if (peer == nullptr) {
        return std::unexpected(Error{ErrorCode::InvalidPeer, "Unknown peer id"});
    }
    if (peer->state != Peer::State::Connected) {
        return std::unexpected(Error{ErrorCode::InvalidState, "Peer is not connected"});
    }
    if (uploads_.size() >= config_.max_pending_reliable_messages) {
        return std::unexpected(Error{
            ErrorCode::CapacityExceeded,
            "Exceeded maximum concurrent uploads"
        });
    }

    std::error_code ec{};
    if (!std::filesystem::exists(local_path, ec) || !std::filesystem::is_regular_file(local_path, ec)) {
        return std::unexpected(Error{ErrorCode::FileOpenFailed, "File does not exist or is not a regular file"});
    }
    const std::uintmax_t file_size = std::filesystem::file_size(local_path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::FileOpenFailed, "Failed to read file size"});
    }
    if (file_size > std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "File size exceeds protocol limits"});
    }
    const std::uint64_t total_bytes = static_cast<std::uint64_t>(file_size);

    UploadTransfer transfer{};
    transfer.peer = peer_id;
    transfer.transfer_id = next_transfer_id_++;
    transfer.local_path = local_path;
    transfer.total_bytes = total_bytes;
    transfer.remote_filename = options.remote_name.empty()
        ? local_path.filename().string()
        : options.remote_name;
    if (transfer.remote_filename.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "Remote filename is too long"});
    }

    transfer.stream.open(local_path, std::ios::binary);
    if (!transfer.stream.is_open()) {
        return std::unexpected(Error{ErrorCode::FileOpenFailed, "Failed to open file for reading"});
    }

    const std::vector<std::byte> offer = encode_file_offer(transfer.transfer_id, transfer.total_bytes, transfer.remote_filename);
    if (const auto result = send_file_control(peer_id, offer); !result.has_value()) {
        return std::unexpected(result.error());
    }

    const std::uint64_t transfer_id = transfer.transfer_id;
    uploads_[transfer_id] = std::move(transfer);
    return transfer_id;
}

std::expected<void, Error> Host::Impl::accept_file(PeerId peer, std::uint64_t transfer_id, const std::filesystem::path& destination_path) {
    return begin_download(peer, transfer_id, destination_path);
}

std::expected<void, Error> Host::Impl::reject_file(PeerId peer, std::uint64_t transfer_id, std::string_view reason) {
    if (reason.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "Reject reason is too long"});
    }
    const TransferKey key{peer, transfer_id};
    auto it = downloads_.find(key);
    if (it == downloads_.end()) {
        return std::unexpected(Error{ErrorCode::FileTransferNotFound, "Transfer was not found"});
    }
    if (it->second.accepted) {
        return std::unexpected(Error{ErrorCode::FileTransferStateInvalid, "Transfer is already accepted"});
    }

    const std::vector<std::byte> reject = encode_file_reject(transfer_id, reason);
    if (const auto result = send_file_control(peer, reject); !result.has_value()) {
        return std::unexpected(result.error());
    }
    downloads_.erase(it);
    return {};
}

std::expected<void, Error> Host::Impl::disconnect(PeerId id, DisconnectReason reason) {
    Peer* peer = peer_by_id(id);
    if (peer == nullptr) {
        return std::unexpected(Error{ErrorCode::InvalidPeer, "Unknown peer id"});
    }

    std::array<std::byte, 2> payload{};
    const std::uint16_t wire = static_cast<std::uint16_t>(reason);
    payload[0] = static_cast<std::byte>((wire >> 8) & 0xff);
    payload[1] = static_cast<std::byte>(wire & 0xff);
    (void)send_packet(*peer, PacketType::Disconnect, 0, Delivery::Unreliable, 0, 0, 1, payload, std::nullopt);

    remove_peer(id, DisconnectReason::Requested);
    return {};
}

void Host::Impl::service() {
    if (!running_) {
        return;
    }

    receive_loop();

    const TimePoint now = Clock::now();
    for (std::size_t index = 1; index < peers_.size(); ++index) {
        if (!peers_[index].has_value()) {
            continue;
        }
        Peer& peer = *peers_[index];

        if (using_tcp()) {
            if (peer.tcp_socket == detail::kInvalidSocket) {
                remove_peer(peer.id, DisconnectReason::RemoteClosed);
                continue;
            }

            bool tcp_ready = true;
            if (peer.tcp_connect_in_progress) {
                auto connected = tcp_connect_finished(peer.tcp_socket);
                if (!connected.has_value()) {
                    remove_peer(peer.id, DisconnectReason::RemoteClosed);
                    continue;
                }
                if (!*connected) {
                    tcp_ready = false;
                } else {
                    peer.tcp_connect_in_progress = false;
                }
            }

            if (tcp_ready) {
                if (const auto flushed = flush_tcp_send(peer); !flushed.has_value()) {
                    remove_peer(peer.id, DisconnectReason::RemoteClosed);
                    continue;
                }
            }
        }

        if (peer.state == Peer::State::Connecting && peer.outgoing && !peer.tcp_connect_in_progress &&
            (now - peer.last_connect_attempt) >= config_.connect_retry_interval) {
            (void)send_connect_request(peer);
        }

        if (peer.state == Peer::State::Connected) {
            if (peer.ack_dirty && (now - peer.last_send) >= config_.ack_flush_interval) {
                (void)send_packet(peer, PacketType::AckOnly, 0, Delivery::Unreliable, 0, 0, 1, {}, std::nullopt);
            }
            if ((now - peer.last_send) >= config_.heartbeat_interval) {
                (void)send_ping(peer);
            }
            resend_reliable(peer, now);
        }

        cleanup_assemblies(peer, now);

        if ((now - peer.last_recv) >= config_.disconnect_timeout) {
            remove_peer(peer.id, DisconnectReason::Timeout);
        }
    }

    pump_uploads();
}

std::optional<Event> Host::Impl::poll_event() {
    if (events_.empty()) {
        return std::nullopt;
    }
    Event event = std::move(events_.front());
    events_.pop_front();
    return event;
}

std::vector<PeerId> Host::Impl::connected_peers() const {
    std::vector<PeerId> out;
    for (std::size_t index = 1; index < peers_.size(); ++index) {
        if (peers_[index].has_value() && peers_[index]->state == Peer::State::Connected) {
            out.push_back(static_cast<PeerId>(index));
        }
    }
    return out;
}

std::optional<Address> Host::Impl::peer_address(PeerId id) const {
    if (id == invalid_peer_id || id >= peers_.size() || !peers_[id].has_value()) {
        return std::nullopt;
    }
    return peers_[id]->address;
}

std::optional<PeerStats> Host::Impl::peer_stats(PeerId id) const {
    if (id == invalid_peer_id || id >= peers_.size() || !peers_[id].has_value()) {
        return std::nullopt;
    }
    PeerStats stats = peers_[id]->stats;
    stats.connected_for = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - peers_[id]->created_at);
    return stats;
}

std::optional<Address> Host::Impl::upnp_external_address() const {
    if (!upnp_mapping_.has_value() || upnp_mapping_->external_ip.empty()) {
        return std::nullopt;
    }
    auto resolved = Address::from_ip(upnp_mapping_->external_ip, upnp_mapping_->external_port);
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    return *resolved;
}

std::optional<Error> Host::Impl::upnp_last_error() const {
    return upnp_last_error_;
}

PeerId Host::Impl::allocate_peer_id() const {
    for (std::size_t index = 1; index < peers_.size(); ++index) {
        if (!peers_[index].has_value()) {
            return static_cast<PeerId>(index);
        }
    }
    return invalid_peer_id;
}

Host::Impl::Peer* Host::Impl::peer_by_id(PeerId id) {
    if (id == invalid_peer_id || id >= peers_.size()) {
        return nullptr;
    }
    if (!peers_[id].has_value()) {
        return nullptr;
    }
    return &(*peers_[id]);
}

const Host::Impl::Peer* Host::Impl::peer_by_id(PeerId id) const {
    if (id == invalid_peer_id || id >= peers_.size()) {
        return nullptr;
    }
    if (!peers_[id].has_value()) {
        return nullptr;
    }
    return &(*peers_[id]);
}

Host::Impl::Peer* Host::Impl::peer_by_address(const Address& address) {
    for (std::size_t index = 1; index < peers_.size(); ++index) {
        if (peers_[index].has_value() && peers_[index]->address == address) {
            return &(*peers_[index]);
        }
    }
    return nullptr;
}

std::expected<void, Error> Host::Impl::send_packet(
    Peer& peer,
    PacketType type,
    std::uint8_t channel,
    Delivery delivery,
    std::uint32_t message_sequence,
    std::uint16_t fragment_index,
    std::uint16_t fragment_count,
    std::span<const std::byte> payload,
    std::optional<ReliableRef> reliable_ref) {

    if (fragment_count == 0 || fragment_index >= fragment_count) {
        return std::unexpected(Error{
            ErrorCode::InvalidPacket,
            "Invalid fragment metadata"
        });
    }
    if (payload.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "Payload exceeds protocol field size"});
    }

    WireHeader header{};
    header.protocol_id = config_.protocol_id;
    header.type = type;
    header.peer_id = peer.wire_id;
    header.channel = channel;
    header.delivery = delivery;
    header.packet_sequence = peer.next_packet_sequence++;
    header.ack = peer.has_remote_sequence ? peer.remote_sequence : 0;
    header.ack_bits = peer.has_remote_sequence ? peer.remote_ack_bits : 0;
    header.message_sequence = message_sequence;
    header.fragment_index = fragment_index;
    header.fragment_count = fragment_count;
    header.payload_size = static_cast<std::uint16_t>(payload.size());

    std::vector<std::byte> encoded = serialize_packet(header, payload);
    if (encoded.size() > config_.mtu) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "Datagram exceeds configured MTU"});
    }

    std::size_t wire_bytes = 0;
    if (using_tcp()) {
        if (peer.tcp_socket == detail::kInvalidSocket) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "TCP peer socket is not open"
            });
        }
        if (encoded.size() > std::numeric_limits<std::uint16_t>::max()) {
            return std::unexpected(Error{
                .code = ErrorCode::MessageTooLarge,
                .message = "TCP frame exceeds 16-bit length field"
            });
        }

        const std::uint16_t frame_size = static_cast<std::uint16_t>(encoded.size());
        peer.tcp_send_buffer.push_back(static_cast<std::byte>((frame_size >> 8) & 0xff));
        peer.tcp_send_buffer.push_back(static_cast<std::byte>(frame_size & 0xff));
        peer.tcp_send_buffer.insert(peer.tcp_send_buffer.end(), encoded.begin(), encoded.end());
        wire_bytes = kTcpFrameHeaderSize + encoded.size();

        auto flushed = flush_tcp_send(peer);
        if (!flushed.has_value()) {
            return std::unexpected(flushed.error());
        }
    } else {
        auto sent = socket_.send_to(encoded, peer.address);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());
        }
        wire_bytes = sent.value();
    }

    const TimePoint now = Clock::now();
    peer.last_send = now;
    peer.ack_dirty = false;
    peer.stats.packets_sent += 1;
    peer.stats.bytes_sent += wire_bytes;
    peer.sent_packets[header.packet_sequence] = SentPacket{now, reliable_ref};
    peer.sent_packet_order.push_back(header.packet_sequence);
    while (peer.sent_packet_order.size() > config_.max_sent_packet_history) {
        const std::uint32_t sequence = peer.sent_packet_order.front();
        peer.sent_packet_order.pop_front();
        peer.sent_packets.erase(sequence);
    }
    return {};
}

std::expected<void, Error> Host::Impl::send_connect_request(Peer& peer) {
    peer.last_connect_attempt = Clock::now();

    std::array<std::byte, 4> payload{};
    payload[0] = static_cast<std::byte>(config_.channel_count);
    payload[1] = static_cast<std::byte>(0);
    payload[2] = static_cast<std::byte>((config_.mtu >> 8) & 0xff);
    payload[3] = static_cast<std::byte>(config_.mtu & 0xff);
    return send_packet(peer, PacketType::ConnectRequest, 0, Delivery::Unreliable, 0, 0, 1, payload, std::nullopt);
}

std::expected<void, Error> Host::Impl::send_connect_accept(Peer& peer) {
    std::array<std::byte, 2> payload{};
    payload[0] = static_cast<std::byte>(config_.channel_count);
    payload[1] = static_cast<std::byte>(0);
    return send_packet(peer, PacketType::ConnectAccept, 0, Delivery::Unreliable, 0, 0, 1, payload, std::nullopt);
}

std::expected<void, Error> Host::Impl::send_ping(Peer& peer) {
    std::array<std::byte, 8> payload{};
    const std::uint64_t micros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    for (int i = 0; i < 8; ++i) {
        payload[i] = static_cast<std::byte>((micros >> ((7 - i) * 8)) & 0xff);
    }
    return send_packet(peer, PacketType::Ping, 0, Delivery::Unreliable, 0, 0, 1, payload, std::nullopt);
}

std::expected<void, Error> Host::Impl::send_pong(Peer& peer, std::span<const std::byte> payload) {
    return send_packet(peer, PacketType::Pong, 0, Delivery::Unreliable, 0, 0, 1, payload, std::nullopt);
}

std::expected<void, Error> Host::Impl::send_data_fragment(
    Peer& peer,
    Delivery delivery,
    std::uint8_t channel,
    std::uint32_t sequence,
    std::uint16_t fragment_index,
    std::uint16_t fragment_count,
    std::span<const std::byte> payload,
    std::optional<ReliableRef> reliable_ref) {
    return send_packet(peer, PacketType::Data, channel, delivery, sequence, fragment_index, fragment_count, payload, reliable_ref);
}

void Host::Impl::receive_loop() {
    if (using_tcp()) {
        receive_loop_tcp();
        return;
    }
    receive_loop_udp();
}

void Host::Impl::receive_loop_udp() {
    std::array<std::byte, kMaxUdpPacketSize> buffer{};
    while (true) {
        auto recv = socket_.receive(buffer);
        if (!recv.has_value()) {
            break;
        }
        if (!recv->has_value()) {
            break;
        }

        const detail::ReceiveResult packet = **recv;
        if (packet.size > buffer.size()) {
            continue;
        }
        std::span<const std::byte> bytes(buffer.data(), packet.size);
        process_datagram(packet.from, bytes);
    }
}

std::expected<void, Error> Host::Impl::flush_tcp_send(Peer& peer) {
    while (peer.tcp_send_offset < peer.tcp_send_buffer.size()) {
        std::span<const std::byte> pending(
            peer.tcp_send_buffer.data() + static_cast<std::ptrdiff_t>(peer.tcp_send_offset),
            peer.tcp_send_buffer.size() - peer.tcp_send_offset);
        auto sent = tcp_send_some(peer.tcp_socket, pending);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());
        }
        if (!sent->has_value()) {
            break;
        }
        peer.tcp_send_offset += **sent;
    }

    if (peer.tcp_send_offset == peer.tcp_send_buffer.size()) {
        peer.tcp_send_buffer.clear();
        peer.tcp_send_offset = 0;
    } else if (peer.tcp_send_offset > 0 && peer.tcp_send_offset > peer.tcp_send_buffer.size() / 2) {
        peer.tcp_send_buffer.erase(
            peer.tcp_send_buffer.begin(),
            peer.tcp_send_buffer.begin() + static_cast<std::ptrdiff_t>(peer.tcp_send_offset));
        peer.tcp_send_offset = 0;
    }

    return {};
}

void Host::Impl::accept_tcp_peers() {
    if (!accepts_incoming_ || tcp_listen_socket_ == detail::kInvalidSocket) {
        return;
    }

    while (true) {
        auto accepted = tcp_accept(tcp_listen_socket_);
        if (!accepted.has_value()) {
            break;
        }
        if (!accepted->has_value()) {
            break;
        }

        auto [tcp_socket, from] = std::move(**accepted);
        if (Peer* existing = peer_by_address(from); existing != nullptr) {
            if (existing->tcp_socket != detail::kInvalidSocket) {
                detail::close_socket(existing->tcp_socket);
            }
            existing->tcp_socket = tcp_socket;
            existing->tcp_connect_in_progress = false;
            continue;
        }

        const PeerId id = allocate_peer_id();
        if (id == invalid_peer_id) {
            detail::close_socket(tcp_socket);
            continue;
        }

        const TimePoint now = Clock::now();
        Peer peer{};
        peer.id = id;
        peer.wire_id = id;
        peer.address = from;
        peer.outgoing = false;
        peer.state = Peer::State::Connected;
        peer.created_at = now;
        peer.last_recv = now;
        peer.last_send = now;
        peer.channels.resize(config_.channel_count);
        peer.tcp_socket = tcp_socket;
        peers_[id] = std::move(peer);

        (void)send_connect_accept(*peers_[id]);

        Event event{};
        event.type = Event::Type::Connect;
        event.connect.peer = id;
        event.connect.address = from;
        push_event(std::move(event));
    }
}

void Host::Impl::receive_loop_tcp() {
    accept_tcp_peers();

    std::array<std::byte, 4096> recv_buffer{};
    for (std::size_t index = 1; index < peers_.size(); ++index) {
        if (!peers_[index].has_value()) {
            continue;
        }
        PeerId id = peers_[index]->id;
        if (peers_[index]->tcp_socket == detail::kInvalidSocket) {
            remove_peer(id, DisconnectReason::RemoteClosed);
            continue;
        }

        while (peers_[index].has_value()) {
            Peer& peer = *peers_[index];
            auto received = tcp_receive_some(peer.tcp_socket, recv_buffer);
            if (!received.has_value()) {
                remove_peer(peer.id, DisconnectReason::RemoteClosed);
                break;
            }
            if (!received->has_value()) {
                break;
            }
            if (**received == 0) {
                remove_peer(peer.id, DisconnectReason::RemoteClosed);
                break;
            }

            const std::size_t byte_count = **received;
            peer.tcp_recv_buffer.insert(
                peer.tcp_recv_buffer.end(),
                recv_buffer.begin(),
                recv_buffer.begin() + static_cast<std::ptrdiff_t>(byte_count));
        }

        if (!peers_[index].has_value()) {
            continue;
        }

        while (peers_[index].has_value()) {
            Peer& peer = *peers_[index];
            if (peer.tcp_recv_buffer.size() < kTcpFrameHeaderSize) {
                break;
            }

            std::span<const std::byte> frame_header(peer.tcp_recv_buffer.data(), peer.tcp_recv_buffer.size());
            const std::uint16_t frame_size = read_u16(frame_header, 0);
            if (frame_size == 0 || frame_size > config_.mtu) {
                remove_peer(peer.id, DisconnectReason::ProtocolError);
                break;
            }
            const std::size_t total_frame = kTcpFrameHeaderSize + static_cast<std::size_t>(frame_size);
            if (peer.tcp_recv_buffer.size() < total_frame) {
                break;
            }

            std::vector<std::byte> packet(frame_size);
            std::memcpy(
                packet.data(),
                peer.tcp_recv_buffer.data() + static_cast<std::ptrdiff_t>(kTcpFrameHeaderSize),
                frame_size);
            peer.tcp_recv_buffer.erase(
                peer.tcp_recv_buffer.begin(),
                peer.tcp_recv_buffer.begin() + static_cast<std::ptrdiff_t>(total_frame));

            process_datagram(peer.address, packet);
        }
    }
}

void Host::Impl::process_datagram(const Address& from, std::span<const std::byte> bytes) {
    auto decoded = decode_packet(bytes);
    if (!decoded.has_value()) {
        return;
    }
    if (decoded->header.protocol_id != config_.protocol_id) {
        return;
    }

    Peer* peer = peer_by_address(from);
    if (peer == nullptr) {
        if (accepts_incoming_ && decoded->header.type == PacketType::ConnectRequest) {
            handle_connect_request(from);
        }
        return;
    }

    if (peer->wire_id != invalid_peer_id &&
        decoded->header.peer_id != peer->wire_id &&
        decoded->header.type != PacketType::ConnectRequest) {
        return;
    }

    peer->last_recv = Clock::now();
    peer->stats.packets_received += 1;
    peer->stats.bytes_received += bytes.size();
    register_remote_sequence(*peer, decoded->header.packet_sequence);
    apply_acks(*peer, decoded->header.ack, decoded->header.ack_bits);

    switch (decoded->header.type) {
    case PacketType::ConnectRequest:
        if (accepts_incoming_) {
            (void)send_connect_accept(*peer);
        }
        break;
    case PacketType::ConnectAccept:
        handle_connect_accept(*peer, decoded->header);
        break;
    case PacketType::Disconnect:
        handle_disconnect(*peer, decoded->payload);
        break;
    case PacketType::Ping:
        (void)send_pong(*peer, decoded->payload);
        break;
    case PacketType::Pong:
        break;
    case PacketType::Data:
        handle_data(*peer, decoded->header, decoded->payload);
        break;
    case PacketType::AckOnly:
        break;
    }
}

void Host::Impl::handle_connect_request(const Address& from) {
    if (!accepts_incoming_) {
        return;
    }

    if (Peer* existing = peer_by_address(from); existing != nullptr) {
        (void)send_connect_accept(*existing);
        return;
    }

    const PeerId id = allocate_peer_id();
    if (id == invalid_peer_id) {
        return;
    }

    const TimePoint now = Clock::now();
    Peer peer{};
    peer.id = id;
    peer.wire_id = id;
    peer.address = from;
    peer.outgoing = false;
    peer.state = Peer::State::Connected;
    peer.created_at = now;
    peer.last_recv = now;
    peer.last_send = now;
    peer.channels.resize(config_.channel_count);
    peers_[id] = std::move(peer);

    (void)send_connect_accept(*peers_[id]);

    Event event{};
    event.type = Event::Type::Connect;
    event.connect.peer = id;
    event.connect.address = from;
    push_event(std::move(event));
}

void Host::Impl::handle_connect_accept(Peer& peer, const WireHeader& header) {
    if (!peer.outgoing || peer.state != Peer::State::Connecting) {
        return;
    }
    if (header.peer_id == invalid_peer_id) {
        return;
    }

    peer.wire_id = header.peer_id;
    peer.state = Peer::State::Connected;

    Event event{};
    event.type = Event::Type::Connect;
    event.connect.peer = peer.id;
    event.connect.address = peer.address;
    push_event(std::move(event));
}

void Host::Impl::handle_disconnect(Peer& peer, std::span<const std::byte> payload) {
    DisconnectReason reason = DisconnectReason::RemoteClosed;
    if (payload.size() >= 2) {
        const std::uint16_t wire = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(payload[0]) << 8) |
            static_cast<std::uint16_t>(payload[1]));
        reason = static_cast<DisconnectReason>(wire);
    }
    remove_peer(peer.id, reason);
}

void Host::Impl::handle_data(Peer& peer, const WireHeader& header, std::span<const std::byte> payload) {
    auto delivery = delivery_from_byte(static_cast<std::uint8_t>(header.delivery));
    if (!delivery.has_value()) {
        return;
    }
    if (header.channel >= config_.channel_count) {
        return;
    }
    if (header.fragment_count == 0 || header.fragment_index >= header.fragment_count) {
        return;
    }
    if (header.fragment_count > config_.max_fragments_per_message) {
        return;
    }

    if (header.fragment_count == 1) {
        process_message(peer, header.channel, *delivery, header.message_sequence, payload);
        return;
    }

    const std::uint64_t key = make_assembly_key(header.channel, *delivery, header.message_sequence);
    if (!peer.assemblies.contains(key) &&
        peer.assemblies.size() >= config_.max_pending_reassembly_messages) {
        return;
    }
    Assembly& assembly = peer.assemblies[key];
    if (assembly.fragment_count == 0) {
        assembly.delivery = *delivery;
        assembly.channel = header.channel;
        assembly.sequence = header.message_sequence;
        assembly.fragment_count = header.fragment_count;
        assembly.fragments.resize(header.fragment_count);
        assembly.received.assign(header.fragment_count, false);
    }

    if (assembly.fragment_count != header.fragment_count || assembly.delivery != *delivery || assembly.channel != header.channel) {
        return;
    }

    assembly.updated_at = Clock::now();
    if (!assembly.received[header.fragment_index]) {
        assembly.received[header.fragment_index] = true;
        assembly.fragments[header.fragment_index].assign(payload.begin(), payload.end());
    }

    if (!all_true(assembly.received)) {
        return;
    }

    std::vector<std::byte> joined;
    std::size_t total = 0;
    for (const auto& fragment : assembly.fragments) {
        total += fragment.size();
    }
    joined.reserve(total);
    for (const auto& fragment : assembly.fragments) {
        joined.insert(joined.end(), fragment.begin(), fragment.end());
    }

    peer.assemblies.erase(key);
    process_message(peer, header.channel, *delivery, header.message_sequence, joined);
}

void Host::Impl::process_message(
    Peer& peer,
    std::uint8_t channel_index,
    Delivery delivery,
    std::uint32_t sequence,
    std::span<const std::byte> payload) {
    ChannelState& channel = peer.channels[channel_index];
    const bool is_file_channel = channel_index == file_channel();

    if (delivery == Delivery::ReliableOrdered) {
        if (sequence < channel.next_reliable_recv) {
            return;
        }

        if (sequence > channel.next_reliable_recv) {
            if (channel.pending_ordered.size() >= config_.max_pending_reassembly_messages) {
                return;
            }
            if (!channel.pending_ordered.contains(sequence)) {
                channel.pending_ordered.emplace(sequence, std::vector<std::byte>(payload.begin(), payload.end()));
            }
            return;
        }

        if (!is_file_channel || !handle_file_control(peer, payload)) {
            emit_message(peer.id, channel_index, delivery, payload);
        }
        channel.next_reliable_recv += 1;

        auto pending_it = channel.pending_ordered.find(channel.next_reliable_recv);
        while (pending_it != channel.pending_ordered.end()) {
            std::vector<std::byte> buffered = std::move(pending_it->second);
            channel.pending_ordered.erase(pending_it);
            if (!is_file_channel || !handle_file_control(peer, buffered)) {
                emit_message(peer.id, channel_index, delivery, buffered);
            }
            channel.next_reliable_recv += 1;
            pending_it = channel.pending_ordered.find(channel.next_reliable_recv);
        }
        return;
    }

    if (delivery == Delivery::UnreliableSequenced) {
        if (channel.has_last_unreliable_recv && sequence <= channel.last_unreliable_recv) {
            return;
        }
        channel.has_last_unreliable_recv = true;
        channel.last_unreliable_recv = sequence;
    }

    if (!is_file_channel || !handle_file_control(peer, payload)) {
        emit_message(peer.id, channel_index, delivery, payload);
    }
}

void Host::Impl::emit_message(PeerId id, std::uint8_t channel, Delivery delivery, std::span<const std::byte> payload) {
    Event event{};
    event.type = Event::Type::Message;
    event.message.peer = id;
    event.message.channel = channel;
    event.message.delivery = delivery;
    event.message.payload.assign(payload.begin(), payload.end());
    push_event(std::move(event));
}

void Host::Impl::register_remote_sequence(Peer& peer, std::uint32_t sequence) {
    peer.ack_dirty = true;
    if (!peer.has_remote_sequence) {
        peer.has_remote_sequence = true;
        peer.remote_sequence = sequence;
        peer.remote_ack_bits = 1;
        return;
    }

    if (sequence > peer.remote_sequence) {
        const std::uint32_t delta = sequence - peer.remote_sequence;
        if (delta >= 32) {
            peer.remote_ack_bits = 1;
        } else {
            peer.remote_ack_bits = (peer.remote_ack_bits << delta) | 1u;
        }
        peer.remote_sequence = sequence;
        return;
    }

    const std::uint32_t diff = peer.remote_sequence - sequence;
    if (diff < 32) {
        peer.remote_ack_bits |= (1u << diff);
    }
}

void Host::Impl::apply_acks(Peer& peer, std::uint32_t ack, std::uint32_t ack_bits) {
    if (ack > 0) {
        ack_packet(peer, ack);
    }
    for (std::uint32_t i = 1; i < 32; ++i) {
        if ((ack_bits & (1u << i)) == 0) {
            continue;
        }
        if (ack < i) {
            continue;
        }
        ack_packet(peer, ack - i);
    }
}

void Host::Impl::ack_packet(Peer& peer, std::uint32_t sequence) {
    auto it = peer.sent_packets.find(sequence);
    if (it == peer.sent_packets.end()) {
        return;
    }

    const TimePoint now = Clock::now();
    const double rtt_ms = std::chrono::duration<double, std::milli>(now - it->second.sent_at).count();
    if (peer.stats.smoothed_rtt_ms <= 0.0) {
        peer.stats.smoothed_rtt_ms = rtt_ms;
    } else {
        peer.stats.smoothed_rtt_ms = peer.stats.smoothed_rtt_ms * 0.9 + rtt_ms * 0.1;
    }

    if (it->second.reliable_ref.has_value()) {
        const ReliableRef ref = *it->second.reliable_ref;
        const std::uint64_t key = make_message_key(ref.channel, ref.sequence);
        auto message_it = peer.reliable_outgoing.find(key);
        if (message_it != peer.reliable_outgoing.end() && ref.fragment < message_it->second.acked.size()) {
            message_it->second.acked[ref.fragment] = true;
            if (all_true(message_it->second.acked)) {
                peer.reliable_outgoing.erase(message_it);
            }
        }
    }

    peer.sent_packets.erase(it);
}

void Host::Impl::resend_reliable(Peer& peer, TimePoint now) {
    for (auto& [_, message] : peer.reliable_outgoing) {
        for (std::uint16_t i = 0; i < message.fragments.size(); ++i) {
            if (message.acked[i]) {
                continue;
            }
            if ((now - message.last_sent[i]) < config_.reliable_resend_interval) {
                continue;
            }

            auto result = send_data_fragment(
                peer,
                Delivery::ReliableOrdered,
                message.channel,
                message.sequence,
                i,
                static_cast<std::uint16_t>(message.fragments.size()),
                message.fragments[i],
                ReliableRef{message.channel, message.sequence, i});
            if (result.has_value()) {
                message.last_sent[i] = now;
                peer.stats.reliable_retransmissions += 1;
            }
        }
    }
}

void Host::Impl::cleanup_assemblies(Peer& peer, TimePoint now) {
    std::vector<std::uint64_t> remove;
    remove.reserve(peer.assemblies.size());
    for (const auto& [key, assembly] : peer.assemblies) {
        if ((now - assembly.updated_at) >= config_.fragment_timeout) {
            remove.push_back(key);
        }
    }
    for (std::uint64_t key : remove) {
        peer.assemblies.erase(key);
    }
}

void Host::Impl::pump_uploads() {
    if (uploads_.empty()) {
        return;
    }

    const std::size_t configured_chunk = std::clamp<std::size_t>(config_.file_chunk_size, 1, std::numeric_limits<std::uint16_t>::max());
    std::vector<std::uint64_t> completed;
    completed.reserve(uploads_.size());

    for (auto& [transfer_id, transfer] : uploads_) {
        if (!transfer.accepted) {
            continue;
        }

        if (transfer.bytes_sent < transfer.total_bytes) {
            const std::size_t remaining = static_cast<std::size_t>(transfer.total_bytes - transfer.bytes_sent);
            const std::size_t chunk_size = std::min(configured_chunk, remaining);
            std::vector<std::byte> chunk(chunk_size);
            transfer.stream.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk_size));
            const std::streamsize bytes_read = transfer.stream.gcount();
            if (bytes_read <= 0) {
                emit_file_rejected(transfer.peer, transfer_id, "file read failed");
                completed.push_back(transfer_id);
                continue;
            }
            chunk.resize(static_cast<std::size_t>(bytes_read));

            const auto payload = encode_file_chunk(transfer_id, transfer.bytes_sent, chunk);
            if (const auto send_result = send_file_control(transfer.peer, payload); !send_result.has_value()) {
                continue;
            }

            transfer.bytes_sent += static_cast<std::uint64_t>(bytes_read);
            emit_file_progress(transfer.peer, transfer_id, transfer.bytes_sent, transfer.total_bytes, true);
            continue;
        }

        const auto payload = encode_file_complete(transfer_id);
        if (const auto send_result = send_file_control(transfer.peer, payload); send_result.has_value()) {
            emit_file_complete(
                transfer.peer,
                transfer_id,
                transfer.remote_filename,
                transfer.total_bytes,
                transfer.local_path.string(),
                true);
            completed.push_back(transfer_id);
        }
    }

    for (const std::uint64_t id : completed) {
        uploads_.erase(id);
    }
}

void Host::Impl::remove_peer_transfers(PeerId peer) {
    std::vector<std::uint64_t> upload_remove;
    upload_remove.reserve(uploads_.size());
    for (const auto& [transfer_id, transfer] : uploads_) {
        if (transfer.peer == peer) {
            upload_remove.push_back(transfer_id);
        }
    }
    for (const std::uint64_t id : upload_remove) {
        uploads_.erase(id);
    }

    std::vector<TransferKey> download_remove;
    download_remove.reserve(downloads_.size());
    for (const auto& [key, _] : downloads_) {
        if (key.peer == peer) {
            download_remove.push_back(key);
        }
    }
    for (const auto& key : download_remove) {
        downloads_.erase(key);
    }
}

std::uint8_t Host::Impl::file_channel() const {
    if (config_.channel_count == 0) {
        return 0;
    }
    if (config_.file_transfer_channel >= config_.channel_count) {
        return static_cast<std::uint8_t>(config_.channel_count - 1);
    }
    return config_.file_transfer_channel;
}

bool Host::Impl::is_file_payload(std::span<const std::byte> payload) const {
    if (payload.size() < 5) {
        return false;
    }
    if (read_u32(payload, 0) != kFileMagic) {
        return false;
    }
    return file_control_type(static_cast<std::uint8_t>(payload[4])).has_value();
}

std::expected<void, Error> Host::Impl::send_file_control(PeerId peer, std::span<const std::byte> payload) {
    SendOptions options{};
    options.channel = file_channel();
    options.delivery = Delivery::ReliableOrdered;
    return send(peer, payload, options);
}

std::expected<void, Error> Host::Impl::begin_download(
    PeerId peer,
    std::uint64_t transfer_id,
    const std::filesystem::path& destination_path) {
    const TransferKey key{peer, transfer_id};
    auto it = downloads_.find(key);
    if (it == downloads_.end()) {
        return std::unexpected(Error{ErrorCode::FileTransferNotFound, "Transfer was not found"});
    }
    if (it->second.accepted) {
        return std::unexpected(Error{ErrorCode::FileTransferStateInvalid, "Transfer is already accepted"});
    }

    DownloadTransfer& transfer = it->second;
    if (destination_path.empty()) {
        return std::unexpected(Error{
            ErrorCode::FileOpenFailed,
            "Destination path must not be empty"
        });
    }
    transfer.destination_path = destination_path;

    std::error_code ec{};
    const std::filesystem::path parent = destination_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::FileOpenFailed, "Failed to create destination directory"});
        }
    }

    transfer.stream.open(destination_path, std::ios::binary | std::ios::trunc);
    if (!transfer.stream.is_open()) {
        return std::unexpected(Error{ErrorCode::FileOpenFailed, "Failed to open destination file"});
    }
    transfer.accepted = true;

    const auto payload = encode_file_accept(transfer_id);
    if (const auto send_result = send_file_control(peer, payload); !send_result.has_value()) {
        transfer.stream.close();
        transfer.accepted = false;
        return std::unexpected(send_result.error());
    }

    return {};
}

bool Host::Impl::handle_file_control(Peer& peer, std::span<const std::byte> payload) {
    if (!is_file_payload(payload)) {
        return false;
    }

    const auto type = file_control_type(static_cast<std::uint8_t>(payload[4]));
    if (!type.has_value()) {
        return true;
    }

    switch (*type) {
    case FileControlType::Offer: {
        if (payload.size() < 23) {
            return true;
        }
        const std::uint64_t transfer_id = read_u64(payload, 5);
        const std::uint64_t total_bytes = read_u64(payload, 13);
        const std::uint16_t name_size = read_u16(payload, 21);
        if (payload.size() != 23 + name_size) {
            return true;
        }

        std::string filename = decode_text(payload, 23, name_size);
        if (filename.empty()) {
            filename = "download.bin";
        }
        const TransferKey key{peer.id, transfer_id};
        if (downloads_.contains(key)) {
            return true;
        }
        if (downloads_.size() >= config_.max_pending_reassembly_messages) {
            return true;
        }

        DownloadTransfer transfer{};
        transfer.peer = peer.id;
        transfer.transfer_id = transfer_id;
        transfer.filename = filename;
        transfer.total_bytes = total_bytes;
        downloads_[key] = std::move(transfer);

        bool auto_accept_applied = false;
        if (config_.auto_accept_file_transfers) {
            const std::filesystem::path destination =
                std::filesystem::path(config_.download_directory) / sanitize_filename(filename);
            auto_accept_applied = begin_download(peer.id, transfer_id, destination).has_value();
        }
        emit_file_offer(peer.id, transfer_id, filename, total_bytes, auto_accept_applied);
        return true;
    }
    case FileControlType::Accept: {
        if (payload.size() != 13) {
            return true;
        }
        const std::uint64_t transfer_id = read_u64(payload, 5);
        auto it = uploads_.find(transfer_id);
        if (it == uploads_.end() || it->second.peer != peer.id) {
            return true;
        }
        it->second.accepted = true;
        emit_file_progress(peer.id, transfer_id, 0, it->second.total_bytes, true);
        return true;
    }
    case FileControlType::Reject: {
        if (payload.size() < 15) {
            return true;
        }
        const std::uint64_t transfer_id = read_u64(payload, 5);
        const std::uint16_t reason_size = read_u16(payload, 13);
        if (payload.size() != 15 + reason_size) {
            return true;
        }
        const std::string reason = decode_text(payload, 15, reason_size);

        auto upload_it = uploads_.find(transfer_id);
        if (upload_it != uploads_.end() && upload_it->second.peer == peer.id) {
            uploads_.erase(upload_it);
        }

        const TransferKey download_key{peer.id, transfer_id};
        auto download_it = downloads_.find(download_key);
        if (download_it != downloads_.end()) {
            downloads_.erase(download_it);
        }

        emit_file_rejected(peer.id, transfer_id, reason.empty() ? "rejected" : reason);
        return true;
    }
    case FileControlType::Chunk: {
        if (payload.size() < 23) {
            return true;
        }
        const std::uint64_t transfer_id = read_u64(payload, 5);
        const std::uint64_t offset = read_u64(payload, 13);
        const std::uint16_t chunk_size = read_u16(payload, 21);
        if (payload.size() != 23 + chunk_size) {
            return true;
        }

        const TransferKey key{peer.id, transfer_id};
        auto it = downloads_.find(key);
        if (it == downloads_.end() || !it->second.accepted) {
            return true;
        }
        DownloadTransfer& transfer = it->second;

        if (offset != transfer.bytes_received) {
            const auto reject = encode_file_reject(transfer_id, "offset mismatch");
            (void)send_file_control(peer.id, reject);
            downloads_.erase(it);
            emit_file_rejected(peer.id, transfer_id, "offset mismatch");
            return true;
        }
        if (chunk_size == 0 && transfer.bytes_received < transfer.total_bytes) {
            const auto reject = encode_file_reject(transfer_id, "empty chunk");
            (void)send_file_control(peer.id, reject);
            downloads_.erase(it);
            emit_file_rejected(peer.id, transfer_id, "empty chunk");
            return true;
        }
        if (transfer.bytes_received > transfer.total_bytes) {
            const auto reject = encode_file_reject(transfer_id, "received bytes exceed declared size");
            (void)send_file_control(peer.id, reject);
            downloads_.erase(it);
            emit_file_rejected(peer.id, transfer_id, "received bytes exceed declared size");
            return true;
        }
        const std::uint64_t remaining = transfer.total_bytes - transfer.bytes_received;
        if (static_cast<std::uint64_t>(chunk_size) > remaining) {
            const auto reject = encode_file_reject(transfer_id, "chunk exceeds declared size");
            (void)send_file_control(peer.id, reject);
            downloads_.erase(it);
            emit_file_rejected(peer.id, transfer_id, "chunk exceeds declared size");
            return true;
        }

        transfer.stream.write(reinterpret_cast<const char*>(payload.data() + 23), static_cast<std::streamsize>(chunk_size));
        if (!transfer.stream.good()) {
            const auto reject = encode_file_reject(transfer_id, "file write failed");
            (void)send_file_control(peer.id, reject);
            downloads_.erase(it);
            emit_file_rejected(peer.id, transfer_id, "file write failed");
            return true;
        }

        transfer.bytes_received += chunk_size;
        emit_file_progress(peer.id, transfer_id, transfer.bytes_received, transfer.total_bytes, false);
        return true;
    }
    case FileControlType::Complete: {
        if (payload.size() != 13) {
            return true;
        }
        const std::uint64_t transfer_id = read_u64(payload, 5);
        const TransferKey key{peer.id, transfer_id};
        auto it = downloads_.find(key);
        if (it == downloads_.end()) {
            return true;
        }

        DownloadTransfer transfer = std::move(it->second);
        downloads_.erase(it);

        if (transfer.stream.is_open()) {
            transfer.stream.close();
        }

        if (transfer.bytes_received == transfer.total_bytes) {
            emit_file_complete(
                peer.id,
                transfer.transfer_id,
                transfer.filename,
                transfer.total_bytes,
                transfer.destination_path.string(),
                false);
        } else {
            if (!transfer.destination_path.empty()) {
                std::error_code ec{};
                (void)std::filesystem::remove(transfer.destination_path, ec);
            }
            emit_file_rejected(peer.id, transfer.transfer_id, "incomplete transfer");
        }
        return true;
    }
    }

    return true;
}

void Host::Impl::emit_file_offer(PeerId peer, std::uint64_t transfer_id, std::string filename, std::uint64_t total_bytes, bool auto_accepted) {
    Event event{};
    event.type = Event::Type::FileOffer;
    event.file_offer.peer = peer;
    event.file_offer.transfer_id = transfer_id;
    event.file_offer.filename = std::move(filename);
    event.file_offer.total_bytes = total_bytes;
    event.file_offer.auto_accepted = auto_accepted;
    push_event(std::move(event));
}

void Host::Impl::emit_file_progress(PeerId peer, std::uint64_t transfer_id, std::uint64_t transferred, std::uint64_t total, bool upload) {
    Event event{};
    event.type = Event::Type::FileProgress;
    event.file_progress.peer = peer;
    event.file_progress.transfer_id = transfer_id;
    event.file_progress.bytes_transferred = transferred;
    event.file_progress.total_bytes = total;
    event.file_progress.upload = upload;
    push_event(std::move(event));
}

void Host::Impl::emit_file_complete(
    PeerId peer,
    std::uint64_t transfer_id,
    std::string filename,
    std::uint64_t total_bytes,
    std::string path,
    bool upload) {
    Event event{};
    event.type = Event::Type::FileComplete;
    event.file_complete.peer = peer;
    event.file_complete.transfer_id = transfer_id;
    event.file_complete.filename = std::move(filename);
    event.file_complete.total_bytes = total_bytes;
    event.file_complete.path = std::move(path);
    event.file_complete.upload = upload;
    push_event(std::move(event));
}

void Host::Impl::emit_file_rejected(PeerId peer, std::uint64_t transfer_id, std::string reason) {
    Event event{};
    event.type = Event::Type::FileRejected;
    event.file_rejected.peer = peer;
    event.file_rejected.transfer_id = transfer_id;
    event.file_rejected.reason = std::move(reason);
    push_event(std::move(event));
}

void Host::Impl::remove_peer(PeerId id, DisconnectReason reason) {
    if (id == invalid_peer_id || id >= peers_.size() || !peers_[id].has_value()) {
        return;
    }

    if (peers_[id]->tcp_socket != detail::kInvalidSocket) {
        detail::close_socket(peers_[id]->tcp_socket);
        peers_[id]->tcp_socket = detail::kInvalidSocket;
    }

    remove_peer_transfers(id);

    Event event{};
    event.type = Event::Type::Disconnect;
    event.disconnect.peer = id;
    event.disconnect.reason = reason;
    push_event(std::move(event));

    peers_[id].reset();
}

Host::Host(HostConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Host::~Host() = default;
Host::Host(Host&&) noexcept = default;
Host& Host::operator=(Host&&) noexcept = default;

std::expected<void, Error> Host::start_server(std::uint16_t port, std::string_view bind_ip) {
    return impl_->start_server(port, bind_ip);
}

std::expected<void, Error> Host::start_p2p(std::uint16_t port, std::string_view bind_ip) {
    return impl_->start_p2p(port, bind_ip);
}

std::expected<PeerId, Error> Host::connect(const Address& remote) {
    return impl_->connect(remote);
}

std::expected<void, Error> Host::send(PeerId peer, std::span<const std::byte> bytes, SendOptions options) {
    return impl_->send(peer, bytes, options);
}

std::expected<std::uint64_t, Error> Host::send_file(PeerId peer, const std::filesystem::path& local_path, FileSendOptions options) {
    return impl_->send_file(peer, local_path, std::move(options));
}

std::expected<void, Error> Host::accept_file(PeerId peer, std::uint64_t transfer_id, const std::filesystem::path& destination_path) {
    return impl_->accept_file(peer, transfer_id, destination_path);
}

std::expected<void, Error> Host::reject_file(PeerId peer, std::uint64_t transfer_id, std::string_view reason) {
    return impl_->reject_file(peer, transfer_id, reason);
}

std::expected<void, Error> Host::disconnect(PeerId peer, DisconnectReason reason) {
    return impl_->disconnect(peer, reason);
}

void Host::service() {
    impl_->service();
}

std::optional<Event> Host::poll_event() {
    return impl_->poll_event();
}

bool Host::is_running() const noexcept {
    return impl_->is_running();
}

bool Host::is_server() const noexcept {
    return impl_->is_server();
}

std::vector<PeerId> Host::connected_peers() const {
    return impl_->connected_peers();
}

std::optional<Address> Host::peer_address(PeerId peer) const {
    return impl_->peer_address(peer);
}

std::optional<PeerStats> Host::peer_stats(PeerId peer) const {
    return impl_->peer_stats(peer);
}

std::optional<Address> Host::upnp_external_address() const {
    return impl_->upnp_external_address();
}

std::optional<Error> Host::upnp_last_error() const {
    return impl_->upnp_last_error();
}

std::vector<std::byte> to_bytes(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

std::string to_string(std::span<const std::byte> bytes) {
    std::string text(bytes.size(), '\0');
    if (!text.empty()) {
        std::memcpy(text.data(), bytes.data(), bytes.size());
    }
    return text;
}

}  // namespace unet
