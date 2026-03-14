#include "detail/quic_transport.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(UNET_HAS_MSQUIC)
#include <msquic.h>
#endif

namespace unet::detail {

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr std::byte kMessageMarker = static_cast<std::byte>(0x01);
constexpr std::byte kAppStreamMarker = static_cast<std::byte>(0xA0);

struct SendBuffer {
    std::vector<std::byte> bytes{};
};

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

bool parse_thumbprint(std::string_view thumbprint, std::array<std::uint8_t, 20>& out) {
    std::string digits;
    digits.reserve(thumbprint.size());
    for (char c : thumbprint) {
        if (std::isxdigit(static_cast<unsigned char>(c)) != 0) {
            digits.push_back(c);
        }
    }

    if (digits.size() != out.size() * 2) {
        return false;
    }

    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hex_value(digits[i * 2]);
        const int lo = hex_value(digits[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

#if defined(UNET_HAS_MSQUIC)

class QuicTransport::Impl {
public:
    explicit Impl(HostConfig config)
        : config_(std::move(config)),
          peers_(static_cast<std::size_t>(config_.max_peers) + 1) {}

    ~Impl() {
        shutdown();
    }

    [[nodiscard]] static bool available() noexcept {
        return true;
    }

    [[nodiscard]] std::expected<void, Error> start_server(std::uint16_t port, std::string_view bind_ip) {
        std::scoped_lock lock(mutex_);
        if (running_) {
            return std::unexpected(Error{ErrorCode::InvalidState, "QUIC transport already running"});
        }
        if (auto ready = ensure_runtime_locked(); !ready.has_value()) {
            return std::unexpected(ready.error());
        }
        if (auto opened = open_listener_locked(port, bind_ip); !opened.has_value()) {
            return std::unexpected(opened.error());
        }
        accepts_incoming_ = true;
        running_ = true;
        return {};
    }

    [[nodiscard]] std::expected<void, Error> start_p2p(std::uint16_t port, std::string_view bind_ip) {
        return start_server(port, bind_ip);
    }

    [[nodiscard]] std::expected<PeerId, Error> connect(const Address& remote) {
        if (!remote.is_valid()) {
            return std::unexpected(Error{ErrorCode::AddressResolutionFailed, "Remote address is invalid"});
        }

        std::scoped_lock lock(mutex_);
        if (auto ready = ensure_runtime_locked(); !ready.has_value()) {
            return std::unexpected(ready.error());
        }

        if (PeerId existing = peer_by_address_locked(remote); existing != invalid_peer_id) {
            return existing;
        }

        const PeerId id = allocate_peer_id_locked();
        if (id == invalid_peer_id) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, "No free peer slots available"});
        }

        HQUIC connection = nullptr;
        const QUIC_STATUS opened = api_->ConnectionOpen(
            registration_,
            quic_connection_callback,
            this,
            &connection);
        if (QUIC_FAILED(opened)) {
            return std::unexpected(make_quic_error(ErrorCode::SocketOpenFailed, "ConnectionOpen", opened));
        }

        PeerState peer{};
        peer.id = id;
        peer.connection = connection;
        peer.address = remote;
        peer.connected = false;
        peer.outgoing = true;
        peer.created_at = Clock::now();
        peer.last_recv = peer.created_at;
        peer.last_send = peer.created_at;
        peers_[id] = std::move(peer);
        connection_to_peer_[connection] = id;
        pending_remote_address_[connection] = remote;

        const QUIC_ADDRESS_FAMILY family = remote.ip().find(':') != std::string::npos
            ? QUIC_ADDRESS_FAMILY_INET6
            : QUIC_ADDRESS_FAMILY_INET;
        const QUIC_STATUS started = api_->ConnectionStart(
            connection,
            client_configuration_,
            family,
            remote.ip().c_str(),
            remote.port());
        if (QUIC_FAILED(started)) {
            cleanup_peer_locked(id, true);
            return std::unexpected(make_quic_error(ErrorCode::SocketOpenFailed, "ConnectionStart", started));
        }

        running_ = true;
        return id;
    }

    [[nodiscard]] std::expected<void, Error> send(PeerId peer_id, std::span<const std::byte> bytes, SendOptions options) {
        std::scoped_lock lock(mutex_);
        PeerState* peer = peer_by_id_locked(peer_id);
        if (peer == nullptr) {
            return std::unexpected(Error{ErrorCode::InvalidPeer, "Unknown peer id"});
        }
        if (!peer->connected) {
            return std::unexpected(Error{ErrorCode::InvalidState, "Peer is not connected"});
        }
        if (options.channel >= config_.channel_count) {
            return std::unexpected(Error{ErrorCode::InvalidChannel, "Invalid channel id"});
        }

        HQUIC stream = nullptr;
        const QUIC_STATUS stream_open = api_->StreamOpen(
            peer->connection,
            QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            quic_stream_callback,
            this,
            &stream);
        if (QUIC_FAILED(stream_open)) {
            return std::unexpected(make_quic_error(ErrorCode::SocketOpenFailed, "StreamOpen(send)", stream_open));
        }

        StreamState state{};
        state.handle = stream;
        state.peer = peer_id;
        state.token = invalid_stream_id;
        state.remote_initiated = false;
        state.app_stream = false;
        state.marker_known = true;
        streams_[stream] = std::move(state);

        const QUIC_STATUS start = api_->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE);
        if (QUIC_FAILED(start)) {
            cleanup_stream_locked(stream);
            return std::unexpected(make_quic_error(ErrorCode::SocketOpenFailed, "StreamStart(send)", start));
        }

        std::vector<std::byte> payload;
        payload.reserve(3 + bytes.size());
        payload.push_back(kMessageMarker);
        payload.push_back(static_cast<std::byte>(options.channel));
        payload.push_back(static_cast<std::byte>(options.delivery));
        payload.insert(payload.end(), bytes.begin(), bytes.end());
        if (auto sent = stream_send_locked(stream, std::move(payload), true); !sent.has_value()) {
            cleanup_stream_locked(stream);
            return std::unexpected(sent.error());
        }

        peer->stats.packets_sent += 1;
        peer->stats.bytes_sent += bytes.size();
        peer->last_send = Clock::now();
        return {};
    }

    [[nodiscard]] std::expected<void, Error> disconnect(PeerId peer_id, DisconnectReason reason) {
        std::scoped_lock lock(mutex_);
        PeerState* peer = peer_by_id_locked(peer_id);
        if (peer == nullptr) {
            return std::unexpected(Error{ErrorCode::InvalidPeer, "Unknown peer id"});
        }
        api_->ConnectionShutdown(peer->connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, static_cast<QUIC_UINT62>(reason));
        return {};
    }

    [[nodiscard]] std::expected<StreamId, Error> open_stream(PeerId peer_id, StreamOpenOptions options) {
        std::scoped_lock lock(mutex_);
        PeerState* peer = peer_by_id_locked(peer_id);
        if (peer == nullptr) {
            return std::unexpected(Error{ErrorCode::InvalidPeer, "Unknown peer id"});
        }
        if (!peer->connected) {
            return std::unexpected(Error{ErrorCode::InvalidState, "Peer is not connected"});
        }
        if (options.channel >= config_.channel_count) {
            return std::unexpected(Error{ErrorCode::InvalidChannel, "Invalid channel id"});
        }

        QUIC_STREAM_OPEN_FLAGS flags = QUIC_STREAM_OPEN_FLAG_NONE;
        if (!options.bidirectional) {
            flags = static_cast<QUIC_STREAM_OPEN_FLAGS>(flags | QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL);
        }

        HQUIC stream = nullptr;
        const QUIC_STATUS opened = api_->StreamOpen(peer->connection, flags, quic_stream_callback, this, &stream);
        if (QUIC_FAILED(opened)) {
            return std::unexpected(make_quic_error(ErrorCode::SocketOpenFailed, "StreamOpen(app)", opened));
        }

        StreamState state{};
        state.handle = stream;
        state.peer = peer_id;
        state.token = allocate_stream_token_locked();
        state.remote_initiated = false;
        state.channel = options.channel;
        state.app_stream = true;
        state.marker_known = true;
        streams_[stream] = std::move(state);
        stream_by_token_[streams_[stream].token] = stream;

        const QUIC_STATUS started = api_->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE);
        if (QUIC_FAILED(started)) {
            StreamId token = streams_[stream].token;
            stream_by_token_.erase(token);
            cleanup_stream_locked(stream);
            return std::unexpected(make_quic_error(ErrorCode::SocketOpenFailed, "StreamStart(app)", started));
        }

        std::vector<std::byte> marker{
            kAppStreamMarker,
            static_cast<std::byte>(options.channel)
        };
        if (auto sent = stream_send_locked(stream, std::move(marker), false); !sent.has_value()) {
            StreamId token = streams_[stream].token;
            stream_by_token_.erase(token);
            cleanup_stream_locked(stream);
            return std::unexpected(sent.error());
        }

        return streams_[stream].token;
    }

    [[nodiscard]] std::expected<void, Error> send_stream(
        PeerId peer_id,
        StreamId stream_id,
        std::span<const std::byte> bytes,
        StreamSendOptions options) {
        std::scoped_lock lock(mutex_);
        HQUIC* stream = stream_for_token_locked(stream_id);
        if (stream == nullptr) {
            return std::unexpected(Error{ErrorCode::InvalidState, "Unknown stream id"});
        }
        StreamState* state = stream_by_handle_locked(*stream);
        if (state == nullptr || state->peer != peer_id || !state->app_stream) {
            return std::unexpected(Error{ErrorCode::InvalidState, "Stream does not belong to peer"});
        }

        std::vector<std::byte> payload(bytes.begin(), bytes.end());
        if (auto sent = stream_send_locked(*stream, std::move(payload), options.fin); !sent.has_value()) {
            return std::unexpected(sent.error());
        }
        return {};
    }

    [[nodiscard]] std::expected<void, Error> close_stream(PeerId peer_id, StreamId stream_id, std::uint64_t error_code) {
        std::scoped_lock lock(mutex_);
        HQUIC* stream = stream_for_token_locked(stream_id);
        if (stream == nullptr) {
            return std::unexpected(Error{ErrorCode::InvalidState, "Unknown stream id"});
        }
        StreamState* state = stream_by_handle_locked(*stream);
        if (state == nullptr || state->peer != peer_id || !state->app_stream) {
            return std::unexpected(Error{ErrorCode::InvalidState, "Stream does not belong to peer"});
        }
        api_->StreamShutdown(
            *stream,
            static_cast<QUIC_STREAM_SHUTDOWN_FLAGS>(QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE),
            error_code);
        return {};
    }

    void service() {}

    [[nodiscard]] std::optional<Event> poll_event() {
        std::scoped_lock lock(mutex_);
        if (events_.empty()) {
            return std::nullopt;
        }
        Event event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

    [[nodiscard]] std::vector<PeerId> connected_peers() const {
        std::scoped_lock lock(mutex_);
        std::vector<PeerId> ids{};
        for (std::size_t i = 1; i < peers_.size(); ++i) {
            if (peers_[i].has_value() && peers_[i]->connected) {
                ids.push_back(static_cast<PeerId>(i));
            }
        }
        return ids;
    }

    [[nodiscard]] std::optional<Address> peer_address(PeerId peer) const {
        std::scoped_lock lock(mutex_);
        if (peer == invalid_peer_id || peer >= peers_.size() || !peers_[peer].has_value()) {
            return std::nullopt;
        }
        return peers_[peer]->address;
    }

    [[nodiscard]] std::optional<PeerStats> peer_stats(PeerId peer) const {
        std::scoped_lock lock(mutex_);
        if (peer == invalid_peer_id || peer >= peers_.size() || !peers_[peer].has_value()) {
            return std::nullopt;
        }
        PeerStats stats = peers_[peer]->stats;
        stats.connected_for = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - peers_[peer]->created_at);
        return stats;
    }

    [[nodiscard]] bool is_peer_connected(PeerId peer) const {
        std::scoped_lock lock(mutex_);
        return peer != invalid_peer_id &&
               peer < peers_.size() &&
               peers_[peer].has_value() &&
               peers_[peer]->connected;
    }

private:
    struct PeerState {
        PeerId id{invalid_peer_id};
        HQUIC connection{};
        Address address{};
        bool connected{};
        bool outgoing{};
        TimePoint created_at{};
        TimePoint last_recv{};
        TimePoint last_send{};
        PeerStats stats{};
    };

    struct StreamState {
        HQUIC handle{};
        PeerId peer{invalid_peer_id};
        StreamId token{invalid_stream_id};
        std::uint8_t channel{};
        bool remote_initiated{};
        bool app_stream{};
        bool marker_known{};
        std::vector<std::byte> recv_buffer{};
    };

    [[nodiscard]] std::expected<void, Error> ensure_runtime_locked();
    [[nodiscard]] std::expected<void, Error> open_listener_locked(std::uint16_t port, std::string_view bind_ip);
    [[nodiscard]] std::expected<void, Error> load_server_credentials_locked();
    [[nodiscard]] std::expected<void, Error> load_client_credentials_locked();
    [[nodiscard]] std::expected<void, Error> stream_send_locked(HQUIC stream, std::vector<std::byte> bytes, bool fin);

    static QUIC_STATUS QUIC_API quic_listener_callback(HQUIC listener, void* context, QUIC_LISTENER_EVENT* event);
    static QUIC_STATUS QUIC_API quic_connection_callback(HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event);
    static QUIC_STATUS QUIC_API quic_stream_callback(HQUIC stream, void* context, QUIC_STREAM_EVENT* event);
    QUIC_STATUS on_listener_event(QUIC_LISTENER_EVENT* event);
    QUIC_STATUS on_connection_event(HQUIC connection, QUIC_CONNECTION_EVENT* event);
    QUIC_STATUS on_stream_event(HQUIC stream, QUIC_STREAM_EVENT* event);

    void push_event_locked(Event event);
    void cleanup_stream_locked(HQUIC stream);
    void cleanup_peer_locked(PeerId id, bool close_connection);
    void shutdown();

    [[nodiscard]] PeerId allocate_peer_id_locked() const;
    [[nodiscard]] PeerId peer_by_address_locked(const Address& address) const;
    [[nodiscard]] PeerState* peer_by_id_locked(PeerId id);
    [[nodiscard]] StreamState* stream_by_handle_locked(HQUIC stream);
    [[nodiscard]] HQUIC* stream_for_token_locked(StreamId stream_id);
    [[nodiscard]] StreamId allocate_stream_token_locked();
    [[nodiscard]] Address address_from_quic_locked(const QUIC_ADDR& address) const;
    [[nodiscard]] Error make_quic_error(ErrorCode code, std::string_view operation, QUIC_STATUS status) const;

private:
    HostConfig config_{};
    mutable std::recursive_mutex mutex_{};

    const QUIC_API_TABLE* api_{nullptr};
    HQUIC registration_{nullptr};
    HQUIC server_configuration_{nullptr};
    HQUIC client_configuration_{nullptr};
    HQUIC listener_{nullptr};
    QUIC_BUFFER alpn_{};
    std::string alpn_storage_{};
    std::vector<std::byte> server_pkcs12_blob_{};

    bool running_{false};
    bool accepts_incoming_{false};
    std::vector<std::optional<PeerState>> peers_{};
    std::unordered_map<HQUIC, PeerId> connection_to_peer_{};
    std::unordered_map<HQUIC, Address> pending_remote_address_{};
    std::unordered_map<HQUIC, StreamState> streams_{};
    std::unordered_map<StreamId, HQUIC> stream_by_token_{};
    StreamId next_stream_id_{1};
    std::unordered_map<void*, std::unique_ptr<SendBuffer>> sends_{};
    std::deque<Event> events_{};
};

std::expected<void, Error> QuicTransport::Impl::ensure_runtime_locked() {
    if (api_ != nullptr) {
        return {};
    }

    const QUIC_STATUS opened = MsQuicOpen2(&api_);
    if (QUIC_FAILED(opened) || api_ == nullptr) {
        return std::unexpected(make_quic_error(ErrorCode::SocketInitializationFailed, "MsQuicOpen2", opened));
    }

    const std::string alpn = config_.quic_alpn.empty() ? "unet" : config_.quic_alpn;
    alpn_storage_ = alpn;
    alpn_.Length = static_cast<std::uint32_t>(alpn_storage_.size());
    alpn_.Buffer = reinterpret_cast<std::uint8_t*>(alpn_storage_.data());

    QUIC_REGISTRATION_CONFIG registration_config{
        "unet",
        QUIC_EXECUTION_PROFILE_LOW_LATENCY
    };
    const QUIC_STATUS registration_status = api_->RegistrationOpen(&registration_config, &registration_);
    if (QUIC_FAILED(registration_status)) {
        return std::unexpected(make_quic_error(ErrorCode::SocketInitializationFailed, "RegistrationOpen", registration_status));
    }

    QUIC_SETTINGS settings{};
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.IdleTimeoutMs = static_cast<std::uint64_t>(std::max<std::int64_t>(1000, config_.disconnect_timeout.count()));
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    settings.KeepAliveIntervalMs = static_cast<std::uint32_t>(std::max<std::int64_t>(100, config_.heartbeat_interval.count()));
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.PeerBidiStreamCount = 128;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.PeerUnidiStreamCount = 128;

    QUIC_STATUS server_config = api_->ConfigurationOpen(
        registration_,
        &alpn_,
        1,
        &settings,
        static_cast<std::uint32_t>(sizeof(settings)),
        nullptr,
        &server_configuration_);
    if (QUIC_FAILED(server_config)) {
        return std::unexpected(make_quic_error(ErrorCode::SocketInitializationFailed, "ConfigurationOpen(server)", server_config));
    }

    QUIC_STATUS client_config = api_->ConfigurationOpen(
        registration_,
        &alpn_,
        1,
        &settings,
        static_cast<std::uint32_t>(sizeof(settings)),
        nullptr,
        &client_configuration_);
    if (QUIC_FAILED(client_config)) {
        return std::unexpected(make_quic_error(ErrorCode::SocketInitializationFailed, "ConfigurationOpen(client)", client_config));
    }

    if (auto server_credentials = load_server_credentials_locked(); !server_credentials.has_value()) {
        return std::unexpected(server_credentials.error());
    }
    if (auto client_credentials = load_client_credentials_locked(); !client_credentials.has_value()) {
        return std::unexpected(client_credentials.error());
    }

    return {};
}

std::expected<void, Error> QuicTransport::Impl::load_server_credentials_locked() {
    QUIC_CREDENTIAL_CONFIG credentials{};
    credentials.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    QUIC_CERTIFICATE_HASH_STORE cert_hash_store{};
    QUIC_CERTIFICATE_FILE cert_file{};
    QUIC_CERTIFICATE_PKCS12 pkcs12{};

    if (!config_.quic_certificate_thumbprint.empty()) {
        std::array<std::uint8_t, 20> hash{};
        if (!parse_thumbprint(config_.quic_certificate_thumbprint, hash)) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "HostConfig.quic_certificate_thumbprint must contain a 40-hex-digit SHA1 thumbprint"
            });
        }

        cert_hash_store.Flags = config_.quic_certificate_store_machine
            ? QUIC_CERTIFICATE_HASH_STORE_FLAG_MACHINE_STORE
            : QUIC_CERTIFICATE_HASH_STORE_FLAG_NONE;
        std::memcpy(cert_hash_store.ShaHash, hash.data(), hash.size());

        const std::string store_name = config_.quic_certificate_store_name.empty()
            ? "MY"
            : config_.quic_certificate_store_name;
        if (store_name.size() >= sizeof(cert_hash_store.StoreName)) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "HostConfig.quic_certificate_store_name is too long"
            });
        }
        std::memset(cert_hash_store.StoreName, 0, sizeof(cert_hash_store.StoreName));
        std::memcpy(cert_hash_store.StoreName, store_name.data(), store_name.size());

        credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE;
        credentials.CertificateHashStore = &cert_hash_store;
    } else if (!config_.quic_pkcs12_file.empty()) {
        std::ifstream file(config_.quic_pkcs12_file, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected(Error{
                .code = ErrorCode::FileOpenFailed,
                .message = "Failed to open HostConfig.quic_pkcs12_file"
            });
        }

        file.seekg(0, std::ios::end);
        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size <= 0) {
            return std::unexpected(Error{
                .code = ErrorCode::FileReadFailed,
                .message = "HostConfig.quic_pkcs12_file is empty"
            });
        }

        server_pkcs12_blob_.resize(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char*>(server_pkcs12_blob_.data()), size);
        if (!file.good()) {
            return std::unexpected(Error{
                .code = ErrorCode::FileReadFailed,
                .message = "Failed to read HostConfig.quic_pkcs12_file"
            });
        }

        pkcs12.Asn1Blob = reinterpret_cast<const std::uint8_t*>(server_pkcs12_blob_.data());
        pkcs12.Asn1BlobLength = static_cast<std::uint32_t>(server_pkcs12_blob_.size());
        pkcs12.PrivateKeyPassword = config_.quic_pkcs12_password.empty()
            ? nullptr
            : config_.quic_pkcs12_password.c_str();

        credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_PKCS12;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
        credentials.CertificatePkcs12 = &pkcs12;
    } else if (!config_.quic_certificate_file.empty() && !config_.quic_private_key_file.empty()) {
        cert_file.CertificateFile = config_.quic_certificate_file.c_str();
        cert_file.PrivateKeyFile = config_.quic_private_key_file.c_str();
        credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
        credentials.CertificateFile = &cert_file;
    } else {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "QUIC server requires quic_certificate_thumbprint, quic_pkcs12_file, or quic_certificate_file + quic_private_key_file"
        });
    }

    const QUIC_STATUS loaded = api_->ConfigurationLoadCredential(server_configuration_, &credentials);
    if (QUIC_FAILED(loaded)) {
        return std::unexpected(make_quic_error(ErrorCode::SocketInitializationFailed, "ConfigurationLoadCredential(server)", loaded));
    }
    return {};
}

std::expected<void, Error> QuicTransport::Impl::load_client_credentials_locked() {
    QUIC_CREDENTIAL_CONFIG credentials{};
    credentials.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credentials.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (config_.quic_insecure_skip_verify) {
        credentials.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
            credentials.Flags | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
    }

    const QUIC_STATUS loaded = api_->ConfigurationLoadCredential(client_configuration_, &credentials);
    if (QUIC_FAILED(loaded)) {
        return std::unexpected(make_quic_error(ErrorCode::SocketInitializationFailed, "ConfigurationLoadCredential(client)", loaded));
    }
    return {};
}

std::expected<void, Error> QuicTransport::Impl::open_listener_locked(std::uint16_t port, std::string_view bind_ip) {
    if (listener_ != nullptr) {
        api_->ListenerStop(listener_);
        api_->ListenerClose(listener_);
        listener_ = nullptr;
    }

    const QUIC_STATUS opened = api_->ListenerOpen(registration_, quic_listener_callback, this, &listener_);
    if (QUIC_FAILED(opened)) {
        return std::unexpected(make_quic_error(ErrorCode::SocketBindFailed, "ListenerOpen", opened));
    }

    QUIC_ADDR local{};
    std::string ip = bind_ip.empty()
        ? (config_.enable_ipv6 ? "::" : "0.0.0.0")
        : std::string(bind_ip);

    if (!QuicAddrFromString(ip.c_str(), port, &local)) {
        if (ip == "::") {
            ip = "0.0.0.0";
        }
        if (!QuicAddrFromString(ip.c_str(), port, &local)) {
            api_->ListenerClose(listener_);
            listener_ = nullptr;
            return std::unexpected(Error{
                .code = ErrorCode::SocketBindFailed,
                .message = "Failed to parse QUIC bind address"
            });
        }
    }

    const QUIC_STATUS started = api_->ListenerStart(listener_, &alpn_, 1, &local);
    if (QUIC_FAILED(started)) {
        api_->ListenerClose(listener_);
        listener_ = nullptr;
        return std::unexpected(make_quic_error(ErrorCode::SocketBindFailed, "ListenerStart", started));
    }
    return {};
}

std::expected<void, Error> QuicTransport::Impl::stream_send_locked(HQUIC stream, std::vector<std::byte> bytes, bool fin) {
    if (bytes.empty() && !fin) {
        return {};
    }
    if (bytes.empty() && fin) {
        const QUIC_STATUS shut = api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        if (QUIC_FAILED(shut)) {
            return std::unexpected(make_quic_error(ErrorCode::SocketSendFailed, "StreamShutdown(graceful)", shut));
        }
        return {};
    }

    auto send = std::make_unique<SendBuffer>();
    send->bytes = std::move(bytes);
    void* context = send.get();
    QUIC_BUFFER buffer{
        .Length = static_cast<std::uint32_t>(send->bytes.size()),
        .Buffer = reinterpret_cast<std::uint8_t*>(send->bytes.data())
    };
    sends_[context] = std::move(send);

    QUIC_SEND_FLAGS flags = QUIC_SEND_FLAG_NONE;
    if (fin) {
        flags = static_cast<QUIC_SEND_FLAGS>(flags | QUIC_SEND_FLAG_FIN);
    }
    const QUIC_STATUS sent = api_->StreamSend(stream, &buffer, 1, flags, context);
    if (QUIC_FAILED(sent)) {
        sends_.erase(context);
        return std::unexpected(make_quic_error(ErrorCode::SocketSendFailed, "StreamSend", sent));
    }
    return {};
}

QUIC_STATUS QUIC_API QuicTransport::Impl::quic_listener_callback(HQUIC, void* context, QUIC_LISTENER_EVENT* event) {
    auto* self = static_cast<Impl*>(context);
    return self->on_listener_event(event);
}

QUIC_STATUS QUIC_API QuicTransport::Impl::quic_connection_callback(HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
    auto* self = static_cast<Impl*>(context);
    return self->on_connection_event(connection, event);
}

QUIC_STATUS QUIC_API QuicTransport::Impl::quic_stream_callback(HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* self = static_cast<Impl*>(context);
    return self->on_stream_event(stream, event);
}

QUIC_STATUS QuicTransport::Impl::on_listener_event(QUIC_LISTENER_EVENT* event) {
    std::scoped_lock lock(mutex_);

    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        return QUIC_STATUS_SUCCESS;
    }

    if (!accepts_incoming_) {
        return QUIC_STATUS_CONNECTION_REFUSED;
    }

    const PeerId id = allocate_peer_id_locked();
    if (id == invalid_peer_id) {
        return QUIC_STATUS_CONNECTION_REFUSED;
    }

    Address remote{};
    if (event->NEW_CONNECTION.Info != nullptr && event->NEW_CONNECTION.Info->RemoteAddress != nullptr) {
        remote = address_from_quic_locked(*event->NEW_CONNECTION.Info->RemoteAddress);
    }

    PeerState peer{};
    peer.id = id;
    peer.connection = event->NEW_CONNECTION.Connection;
    peer.address = remote;
    peer.connected = false;
    peer.outgoing = false;
    peer.created_at = Clock::now();
    peer.last_recv = peer.created_at;
    peer.last_send = peer.created_at;
    peers_[id] = std::move(peer);
    connection_to_peer_[event->NEW_CONNECTION.Connection] = id;
    pending_remote_address_[event->NEW_CONNECTION.Connection] = remote;

    api_->SetCallbackHandler(event->NEW_CONNECTION.Connection, (void*)quic_connection_callback, this);
    const QUIC_STATUS configured = api_->ConnectionSetConfiguration(event->NEW_CONNECTION.Connection, server_configuration_);
    if (QUIC_FAILED(configured)) {
        cleanup_peer_locked(id, true);
        return configured;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicTransport::Impl::on_connection_event(HQUIC connection, QUIC_CONNECTION_EVENT* event) {
    std::scoped_lock lock(mutex_);

    auto it = connection_to_peer_.find(connection);
    if (it == connection_to_peer_.end()) {
        return QUIC_STATUS_SUCCESS;
    }

    PeerState* peer = peer_by_id_locked(it->second);
    if (peer == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        peer->connected = true;
        peer->created_at = Clock::now();
        peer->last_recv = peer->created_at;
        peer->last_send = peer->created_at;
        if (auto pending = pending_remote_address_.find(connection); pending != pending_remote_address_.end()) {
            peer->address = pending->second;
        }

        Event connect{};
        connect.type = Event::Type::Connect;
        connect.connect.peer = peer->id;
        connect.connect.address = peer->address;
        push_event_locked(std::move(connect));
        break;
    }
    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        if (event->PEER_ADDRESS_CHANGED.Address != nullptr) {
            peer->address = address_from_quic_locked(*event->PEER_ADDRESS_CHANGED.Address);
        }
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = event->PEER_STREAM_STARTED.Stream;
        api_->SetCallbackHandler(stream, (void*)quic_stream_callback, this);
        (void)api_->StreamReceiveSetEnabled(stream, TRUE);

        StreamState state{};
        state.handle = stream;
        state.peer = peer->id;
        state.token = allocate_stream_token_locked();
        state.remote_initiated = true;
        state.app_stream = false;
        state.marker_known = false;
        streams_[stream] = std::move(state);
        stream_by_token_[streams_[stream].token] = stream;
        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        Event disconnect{};
        disconnect.type = Event::Type::Disconnect;
        disconnect.disconnect.peer = peer->id;
        disconnect.disconnect.reason = DisconnectReason::RemoteClosed;
        push_event_locked(std::move(disconnect));
        cleanup_peer_locked(peer->id, true);
        break;
    }
    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicTransport::Impl::on_stream_event(HQUIC stream, QUIC_STREAM_EVENT* event) {
    std::scoped_lock lock(mutex_);

    StreamState* state = stream_by_handle_locked(stream);
    if (state == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        if (QUIC_FAILED(event->START_COMPLETE.Status)) {
            cleanup_stream_locked(stream);
        }
        break;
    case QUIC_STREAM_EVENT_RECEIVE: {
        for (std::uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER& part = event->RECEIVE.Buffers[i];
            state->recv_buffer.insert(
                state->recv_buffer.end(),
                reinterpret_cast<std::byte*>(part.Buffer),
                reinterpret_cast<std::byte*>(part.Buffer) + part.Length);
        }

        if (!state->marker_known && state->recv_buffer.size() >= 2) {
            const std::byte marker = state->recv_buffer[0];
            if (marker == kMessageMarker && state->recv_buffer.size() >= 3) {
                state->app_stream = false;
                state->channel = static_cast<std::uint8_t>(state->recv_buffer[1]);
                state->marker_known = true;
            } else if (marker == kAppStreamMarker) {
                state->app_stream = true;
                state->channel = static_cast<std::uint8_t>(state->recv_buffer[1]);
                state->marker_known = true;
                state->recv_buffer.erase(state->recv_buffer.begin(), state->recv_buffer.begin() + 2);

                Event opened{};
                opened.type = Event::Type::StreamOpen;
                opened.stream_open.peer = state->peer;
                opened.stream_open.stream = state->token;
                opened.stream_open.channel = state->channel;
                opened.stream_open.remote_initiated = true;
                opened.stream_open.bidirectional = true;
                push_event_locked(std::move(opened));
            }
        }

        if (state->marker_known && !state->app_stream && (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
            if (state->recv_buffer.size() >= 3) {
                Event message{};
                message.type = Event::Type::Message;
                message.message.peer = state->peer;
                message.message.channel = static_cast<std::uint8_t>(state->recv_buffer[1]);
                message.message.delivery = static_cast<Delivery>(state->recv_buffer[2]);
                message.message.payload.assign(state->recv_buffer.begin() + 3, state->recv_buffer.end());
                push_event_locked(std::move(message));
            }
            state->recv_buffer.clear();
        }

        if (state->marker_known && state->app_stream) {
            if (!state->recv_buffer.empty()) {
                Event data{};
                data.type = Event::Type::StreamData;
                data.stream_data.peer = state->peer;
                data.stream_data.stream = state->token;
                data.stream_data.payload = std::move(state->recv_buffer);
                data.stream_data.fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
                push_event_locked(std::move(data));
                state->recv_buffer.clear();
            } else if ((event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
                Event data{};
                data.type = Event::Type::StreamData;
                data.stream_data.peer = state->peer;
                data.stream_data.stream = state->token;
                data.stream_data.fin = true;
                push_event_locked(std::move(data));
            }
        }
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != nullptr) {
            sends_.erase(event->SEND_COMPLETE.ClientContext);
        }
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        if (state->app_stream && state->token != invalid_stream_id) {
            Event close{};
            close.type = Event::Type::StreamClose;
            close.stream_close.peer = state->peer;
            close.stream_close.stream = state->token;
            close.stream_close.error_code = static_cast<std::uint64_t>(event->SHUTDOWN_COMPLETE.ConnectionErrorCode);
            close.stream_close.remote = event->SHUTDOWN_COMPLETE.ConnectionClosedRemotely != FALSE;
            push_event_locked(std::move(close));
        }
        cleanup_stream_locked(stream);
        break;
    }
    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

void QuicTransport::Impl::push_event_locked(Event event) {
    if (events_.size() >= config_.max_event_queue) {
        events_.pop_front();
    }
    events_.push_back(std::move(event));
}

void QuicTransport::Impl::cleanup_stream_locked(HQUIC stream) {
    auto it = streams_.find(stream);
    if (it == streams_.end()) {
        return;
    }
    if (it->second.token != invalid_stream_id) {
        stream_by_token_.erase(it->second.token);
    }
    streams_.erase(it);
    api_->StreamClose(stream);
}

void QuicTransport::Impl::cleanup_peer_locked(PeerId id, bool close_connection) {
    if (id == invalid_peer_id || id >= peers_.size() || !peers_[id].has_value()) {
        return;
    }

    PeerState peer = std::move(*peers_[id]);
    peers_[id].reset();

    std::vector<HQUIC> peer_streams{};
    for (const auto& [handle, stream] : streams_) {
        if (stream.peer == id) {
            peer_streams.push_back(handle);
        }
    }
    for (HQUIC stream : peer_streams) {
        cleanup_stream_locked(stream);
    }

    pending_remote_address_.erase(peer.connection);
    connection_to_peer_.erase(peer.connection);

    if (close_connection && peer.connection != nullptr) {
        api_->ConnectionClose(peer.connection);
    }
}

void QuicTransport::Impl::shutdown() {
    std::scoped_lock lock(mutex_);

    if (api_ != nullptr) {
        for (auto& [handle, _] : streams_) {
            api_->SetCallbackHandler(handle, nullptr, nullptr);
        }
        for (auto& [connection, _] : connection_to_peer_) {
            api_->SetCallbackHandler(connection, nullptr, nullptr);
        }
        streams_.clear();
        stream_by_token_.clear();
        connection_to_peer_.clear();
        pending_remote_address_.clear();
        for (std::size_t i = 1; i < peers_.size(); ++i) {
            peers_[i].reset();
        }

        if (listener_ != nullptr) {
            api_->SetCallbackHandler(listener_, nullptr, nullptr);
        }
        listener_ = nullptr;
        server_configuration_ = nullptr;
        client_configuration_ = nullptr;
        registration_ = nullptr;
        api_ = nullptr;
    }

    sends_.clear();
    events_.clear();
    running_ = false;
    accepts_incoming_ = false;
}

PeerId QuicTransport::Impl::allocate_peer_id_locked() const {
    for (std::size_t i = 1; i < peers_.size(); ++i) {
        if (!peers_[i].has_value()) {
            return static_cast<PeerId>(i);
        }
    }
    return invalid_peer_id;
}

PeerId QuicTransport::Impl::peer_by_address_locked(const Address& address) const {
    for (std::size_t i = 1; i < peers_.size(); ++i) {
        if (peers_[i].has_value() && peers_[i]->address == address) {
            return static_cast<PeerId>(i);
        }
    }
    return invalid_peer_id;
}

QuicTransport::Impl::PeerState* QuicTransport::Impl::peer_by_id_locked(PeerId id) {
    if (id == invalid_peer_id || id >= peers_.size() || !peers_[id].has_value()) {
        return nullptr;
    }
    return &*peers_[id];
}

QuicTransport::Impl::StreamState* QuicTransport::Impl::stream_by_handle_locked(HQUIC stream) {
    auto it = streams_.find(stream);
    if (it == streams_.end()) {
        return nullptr;
    }
    return &it->second;
}

HQUIC* QuicTransport::Impl::stream_for_token_locked(StreamId stream_id) {
    auto it = stream_by_token_.find(stream_id);
    if (it == stream_by_token_.end()) {
        return nullptr;
    }
    return &it->second;
}

StreamId QuicTransport::Impl::allocate_stream_token_locked() {
    while (next_stream_id_ == invalid_stream_id || stream_by_token_.contains(next_stream_id_)) {
        next_stream_id_ += 1;
    }
    return next_stream_id_++;
}

Address QuicTransport::Impl::address_from_quic_locked(const QUIC_ADDR& address) const {
    char ip_buffer[INET6_ADDRSTRLEN]{};
    std::uint16_t port = 0;
    if (QuicAddrGetFamily(&address) == QUIC_ADDRESS_FAMILY_INET) {
        inet_ntop(AF_INET, &address.Ipv4.sin_addr, ip_buffer, static_cast<socklen_t>(sizeof(ip_buffer)));
        port = ntohs(address.Ipv4.sin_port);
    } else {
        inet_ntop(AF_INET6, &address.Ipv6.sin6_addr, ip_buffer, static_cast<socklen_t>(sizeof(ip_buffer)));
        port = ntohs(address.Ipv6.sin6_port);
    }
    auto resolved = Address::from_ip(ip_buffer, port);
    if (!resolved.has_value()) {
        return {};
    }
    return *resolved;
}

Error QuicTransport::Impl::make_quic_error(ErrorCode code, std::string_view operation, QUIC_STATUS status) const {
    std::string message(operation);
    message.append(" failed with status 0x");
    constexpr char hex[] = "0123456789ABCDEF";
    const std::uint32_t value = static_cast<std::uint32_t>(status);
    for (int shift = 28; shift >= 0; shift -= 4) {
        message.push_back(hex[(value >> shift) & 0xF]);
    }
    return Error{code, std::move(message)};
}

QuicTransport::QuicTransport(HostConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

QuicTransport::~QuicTransport() = default;

bool QuicTransport::available() noexcept {
    return Impl::available();
}

std::expected<void, Error> QuicTransport::start_server(std::uint16_t port, std::string_view bind_ip) {
    return impl_->start_server(port, bind_ip);
}

std::expected<void, Error> QuicTransport::start_p2p(std::uint16_t port, std::string_view bind_ip) {
    return impl_->start_p2p(port, bind_ip);
}

std::expected<PeerId, Error> QuicTransport::connect(const Address& remote) {
    return impl_->connect(remote);
}

std::expected<void, Error> QuicTransport::send(PeerId peer, std::span<const std::byte> bytes, SendOptions options) {
    return impl_->send(peer, bytes, options);
}

std::expected<void, Error> QuicTransport::disconnect(PeerId peer, DisconnectReason reason) {
    return impl_->disconnect(peer, reason);
}

std::expected<StreamId, Error> QuicTransport::open_stream(PeerId peer, StreamOpenOptions options) {
    return impl_->open_stream(peer, options);
}

std::expected<void, Error> QuicTransport::send_stream(PeerId peer, StreamId stream, std::span<const std::byte> bytes, StreamSendOptions options) {
    return impl_->send_stream(peer, stream, bytes, options);
}

std::expected<void, Error> QuicTransport::close_stream(PeerId peer, StreamId stream, std::uint64_t error_code) {
    return impl_->close_stream(peer, stream, error_code);
}

void QuicTransport::service() {
    impl_->service();
}

std::optional<Event> QuicTransport::poll_event() {
    return impl_->poll_event();
}

std::vector<PeerId> QuicTransport::connected_peers() const {
    return impl_->connected_peers();
}

std::optional<Address> QuicTransport::peer_address(PeerId peer) const {
    return impl_->peer_address(peer);
}

std::optional<PeerStats> QuicTransport::peer_stats(PeerId peer) const {
    return impl_->peer_stats(peer);
}

bool QuicTransport::is_peer_connected(PeerId peer) const {
    return impl_->is_peer_connected(peer);
}

#else

class QuicTransport::Impl {
public:
    explicit Impl(HostConfig) {}
};

QuicTransport::QuicTransport(HostConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

QuicTransport::~QuicTransport() = default;

bool QuicTransport::available() noexcept {
    return false;
}

std::expected<void, Error> QuicTransport::start_server(std::uint16_t, std::string_view) {
    return std::unexpected(Error{ErrorCode::InvalidState, "This build does not include QUIC support"});
}

std::expected<void, Error> QuicTransport::start_p2p(std::uint16_t, std::string_view) {
    return std::unexpected(Error{ErrorCode::InvalidState, "This build does not include QUIC support"});
}

std::expected<PeerId, Error> QuicTransport::connect(const Address&) {
    return std::unexpected(Error{ErrorCode::InvalidState, "This build does not include QUIC support"});
}

std::expected<void, Error> QuicTransport::send(PeerId, std::span<const std::byte>, SendOptions) {
    return std::unexpected(Error{ErrorCode::InvalidState, "This build does not include QUIC support"});
}

std::expected<void, Error> QuicTransport::disconnect(PeerId, DisconnectReason) {
    return std::unexpected(Error{ErrorCode::InvalidState, "This build does not include QUIC support"});
}

std::expected<StreamId, Error> QuicTransport::open_stream(PeerId, StreamOpenOptions) {
    return std::unexpected(Error{ErrorCode::InvalidState, "This build does not include QUIC support"});
}

std::expected<void, Error> QuicTransport::send_stream(PeerId, StreamId, std::span<const std::byte>, StreamSendOptions) {
    return std::unexpected(Error{ErrorCode::InvalidState, "This build does not include QUIC support"});
}

std::expected<void, Error> QuicTransport::close_stream(PeerId, StreamId, std::uint64_t) {
    return std::unexpected(Error{ErrorCode::InvalidState, "This build does not include QUIC support"});
}

void QuicTransport::service() {}

std::optional<Event> QuicTransport::poll_event() {
    return std::nullopt;
}

std::vector<PeerId> QuicTransport::connected_peers() const {
    return {};
}

std::optional<Address> QuicTransport::peer_address(PeerId) const {
    return std::nullopt;
}

std::optional<PeerStats> QuicTransport::peer_stats(PeerId) const {
    return std::nullopt;
}

bool QuicTransport::is_peer_connected(PeerId) const {
    return false;
}

#endif

}  // namespace unet::detail
