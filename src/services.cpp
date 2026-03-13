#include "unet/services.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace unet {

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr std::uint32_t kServiceMagic = 0x55535643u;  // "USVC"
constexpr std::uint8_t kServiceVersion = 1;

enum class EnvelopeKind : std::uint8_t {
    Request = 1,
    Response = 2,
    Event = 3
};

enum class OpCode : std::uint16_t {
    RegisterServer = 1,
    HeartbeatServer = 2,
    ListServers = 3,
    PostComment = 4,
    LikeComment = 5,
    ListComments = 6,
    EnqueueMatchmaking = 7,
    CancelMatchmaking = 8,
    RegisterNatIdentity = 9,
    RequestNatPunch = 10,

    MatchFoundEvent = 100,
    NatPunchEvent = 101
};

struct Envelope {
    EnvelopeKind kind{EnvelopeKind::Request};
    OpCode op{OpCode::RegisterServer};
    std::uint32_t request_id{};
    std::vector<std::byte> payload{};
};

class ByteWriter final {
public:
    void u8(std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }

    void u16(std::uint16_t value) {
        data_.push_back(static_cast<std::byte>((value >> 8) & 0xff));
        data_.push_back(static_cast<std::byte>(value & 0xff));
    }

    void u32(std::uint32_t value) {
        data_.push_back(static_cast<std::byte>((value >> 24) & 0xff));
        data_.push_back(static_cast<std::byte>((value >> 16) & 0xff));
        data_.push_back(static_cast<std::byte>((value >> 8) & 0xff));
        data_.push_back(static_cast<std::byte>(value & 0xff));
    }

    void i32(std::int32_t value) {
        u32(static_cast<std::uint32_t>(value));
    }

    void u64(std::uint64_t value) {
        for (int i = 7; i >= 0; --i) {
            data_.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
        }
    }

    [[nodiscard]] bool str(std::string_view text) {
        if (text.size() > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        u16(static_cast<std::uint16_t>(text.size()));
        for (char c : text) {
            data_.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
        return true;
    }

    void bytes(std::span<const std::byte> input) {
        data_.insert(data_.end(), input.begin(), input.end());
    }

    [[nodiscard]] const std::vector<std::byte>& data() const noexcept {
        return data_;
    }

    [[nodiscard]] std::vector<std::byte> take() && {
        return std::move(data_);
    }

private:
    std::vector<std::byte> data_{};
};

class ByteReader final {
public:
    explicit ByteReader(std::span<const std::byte> data)
        : data_(data) {}

    [[nodiscard]] bool u8(std::uint8_t& value) {
        if (pos_ + 1 > data_.size()) {
            return false;
        }
        value = static_cast<std::uint8_t>(data_[pos_]);
        pos_ += 1;
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& value) {
        if (pos_ + 2 > data_.size()) {
            return false;
        }
        value = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data_[pos_]) << 8) |
            static_cast<std::uint16_t>(data_[pos_ + 1]));
        pos_ += 2;
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) {
        if (pos_ + 4 > data_.size()) {
            return false;
        }
        value = (static_cast<std::uint32_t>(data_[pos_]) << 24) |
                (static_cast<std::uint32_t>(data_[pos_ + 1]) << 16) |
                (static_cast<std::uint32_t>(data_[pos_ + 2]) << 8) |
                static_cast<std::uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return true;
    }

    [[nodiscard]] bool i32(std::int32_t& value) {
        std::uint32_t raw = 0;
        if (!u32(raw)) {
            return false;
        }
        value = static_cast<std::int32_t>(raw);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) {
        if (pos_ + 8 > data_.size()) {
            return false;
        }
        value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value = (value << 8) | static_cast<std::uint64_t>(data_[pos_ + i]);
        }
        pos_ += 8;
        return true;
    }

    [[nodiscard]] bool str(std::string& value) {
        std::uint16_t length = 0;
        if (!u16(length)) {
            return false;
        }
        if (pos_ + length > data_.size()) {
            return false;
        }
        value.resize(length);
        if (length > 0) {
            std::memcpy(value.data(), data_.data() + pos_, length);
        }
        pos_ += length;
        return true;
    }

    [[nodiscard]] bool done() const noexcept {
        return pos_ == data_.size();
    }

private:
    std::span<const std::byte> data_{};
    std::size_t pos_{0};
};

std::vector<std::byte> encode_envelope(const Envelope& envelope) {
    ByteWriter writer;
    writer.u32(kServiceMagic);
    writer.u8(kServiceVersion);
    writer.u8(static_cast<std::uint8_t>(envelope.kind));
    writer.u16(static_cast<std::uint16_t>(envelope.op));
    writer.u32(envelope.request_id);
    writer.bytes(envelope.payload);
    return std::move(writer).take();
}

std::optional<Envelope> decode_envelope(std::span<const std::byte> bytes) {
    ByteReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint8_t version = 0;
    std::uint8_t kind = 0;
    std::uint16_t op = 0;
    std::uint32_t request_id = 0;

    if (!reader.u32(magic) || !reader.u8(version) || !reader.u8(kind) || !reader.u16(op) || !reader.u32(request_id)) {
        return std::nullopt;
    }
    if (magic != kServiceMagic || version != kServiceVersion) {
        return std::nullopt;
    }
    if (kind < static_cast<std::uint8_t>(EnvelopeKind::Request) || kind > static_cast<std::uint8_t>(EnvelopeKind::Event)) {
        return std::nullopt;
    }

    Envelope envelope{};
    envelope.kind = static_cast<EnvelopeKind>(kind);
    envelope.op = static_cast<OpCode>(op);
    envelope.request_id = request_id;

    const std::size_t header_size = 4 + 1 + 1 + 2 + 4;
    envelope.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(header_size), bytes.end());
    return envelope;
}

std::vector<std::byte> make_ok_payload() {
    ByteWriter writer;
    writer.u8(1);
    return std::move(writer).take();
}

std::vector<std::byte> make_error_payload(std::string_view message) {
    ByteWriter writer;
    writer.u8(0);
    if (!writer.str(message)) {
        writer = ByteWriter{};
        writer.u8(0);
        (void)writer.str("error");
    }
    return std::move(writer).take();
}

std::expected<std::vector<std::byte>, Error> open_success_payload(std::span<const std::byte> payload) {
    ByteReader reader(payload);
    std::uint8_t ok = 0;
    if (!reader.u8(ok)) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed response payload"
        });
    }

    if (ok == 1) {
        std::vector<std::byte> rest(payload.begin() + 1, payload.end());
        return rest;
    }

    std::string message{};
    if (!reader.str(message)) {
        message = "request failed";
    }
    return std::unexpected(Error{
        .code = ErrorCode::InvalidState,
        .message = std::move(message)
    });
}

std::expected<void, Error> write_address(ByteWriter& writer, const Address& address) {
    const std::string ip = address.ip();
    if (ip.empty()) {
        return std::unexpected(Error{
            .code = ErrorCode::AddressResolutionFailed,
            .message = "Invalid address"
        });
    }
    if (!writer.str(ip)) {
        return std::unexpected(Error{
            .code = ErrorCode::MessageTooLarge,
            .message = "Address string too large"
        });
    }
    writer.u16(address.port());
    return {};
}

std::expected<Address, Error> read_address(ByteReader& reader) {
    std::string ip{};
    std::uint16_t port = 0;
    if (!reader.str(ip) || !reader.u16(port)) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed address payload"
        });
    }

    auto resolved = Address::from_ip(ip, port);
    if (!resolved.has_value()) {
        return std::unexpected(resolved.error());
    }
    return *resolved;
}

std::string default_identity_for_peer(PeerId peer) {
    return "peer-" + std::to_string(peer);
}

}  // namespace

class DirectoryServer::Impl {
public:
    explicit Impl(DirectoryConfig config)
        : config_(std::move(config)),
          host_(config_.host) {}

    struct Session {
        std::unordered_set<std::uint64_t> owned_servers{};
        std::unordered_set<std::uint64_t> tickets{};
        std::optional<std::string> nat_identity{};
    };

    struct RegisteredServerState {
        ServerInfo info{};
        PeerId owner{invalid_peer_id};
        std::uint16_t connect_port{};
        TimePoint last_heartbeat{};
    };

    struct CommentState {
        ServerComment comment{};
        std::unordered_set<std::string> liked_by{};
    };

    struct MatchTicket {
        std::uint64_t ticket_id{};
        PeerId owner{invalid_peer_id};
        MatchmakingProfile profile{};
        TimePoint queued_at{};
    };

    struct NatRegistration {
        std::string identity{};
        PeerId owner{invalid_peer_id};
        Address public_address{};
        TimePoint updated_at{};
    };

    [[nodiscard]] std::expected<void, Error> start(std::uint16_t port, std::string_view bind_ip) {
        if (running_) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "DirectoryServer already started"
            });
        }
        if (const auto valid = validate_config(); !valid.has_value()) {
            return std::unexpected(valid.error());
        }

        auto started = host_.start_server(port, bind_ip);
        if (!started.has_value()) {
            last_error_ = started.error();
            return std::unexpected(started.error());
        }
        running_ = true;
        return {};
    }

    void service();

    [[nodiscard]] bool is_running() const noexcept {
        return running_;
    }

    [[nodiscard]] std::optional<Error> last_error() const {
        return last_error_;
    }

private:
    [[nodiscard]] std::expected<void, Error> validate_config() const;
    void on_disconnect(PeerId peer);
    void prune_stale_servers(TimePoint now);
    void run_matchmaking(TimePoint now);

    void handle_request(PeerId peer, const Envelope& envelope);
    void handle_register_server(PeerId peer, const Envelope& envelope);
    void handle_heartbeat_server(PeerId peer, const Envelope& envelope);
    void handle_list_servers(PeerId peer, const Envelope& envelope);
    void handle_post_comment(PeerId peer, const Envelope& envelope);
    void handle_like_comment(PeerId peer, const Envelope& envelope);
    void handle_list_comments(PeerId peer, const Envelope& envelope);
    void handle_enqueue_matchmaking(PeerId peer, const Envelope& envelope);
    void handle_cancel_matchmaking(PeerId peer, const Envelope& envelope);
    void handle_register_nat_identity(PeerId peer, const Envelope& envelope);
    void handle_request_nat_punch(PeerId peer, const Envelope& envelope);

    [[nodiscard]] bool send_payload(PeerId peer, EnvelopeKind kind, OpCode op, std::uint32_t request_id, std::vector<std::byte> payload);
    void send_error(PeerId peer, OpCode op, std::uint32_t request_id, std::string_view message);
    void send_match_found_event(const MatchTicket& ticket, std::uint64_t match_id, std::int32_t average_mmr, const std::vector<std::string>& players);
    void send_nat_punch_event(PeerId target_peer, std::string_view requester_identity, const Address& requester_address);

    void remove_server(std::uint64_t server_id);

    DirectoryConfig config_{};
    Host host_{};
    bool running_{false};
    std::optional<Error> last_error_{};

    std::unordered_map<PeerId, Session> sessions_{};
    std::unordered_map<std::uint64_t, RegisteredServerState> servers_{};
    std::unordered_map<std::uint64_t, CommentState> comments_{};
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> comments_by_server_{};
    std::unordered_map<std::uint64_t, MatchTicket> tickets_{};
    std::unordered_map<std::string, NatRegistration> nat_by_identity_{};

    std::uint64_t next_server_id_{1};
    std::uint64_t next_comment_id_{1};
    std::uint64_t next_ticket_id_{1};
    std::uint64_t next_match_id_{1};
};

std::expected<void, Error> DirectoryServer::Impl::validate_config() const {
    if (config_.host.channel_count == 0 || config_.channel >= config_.host.channel_count) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidChannel,
            .message = "Directory channel is outside host channel_count"
        });
    }
    if (config_.request_timeout <= std::chrono::milliseconds::zero() ||
        config_.connect_timeout <= std::chrono::milliseconds::zero() ||
        config_.server_ttl <= std::chrono::milliseconds::zero()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "Directory timing values must be > 0"
        });
    }
    if (config_.match_size < 2) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "DirectoryConfig.match_size must be >= 2"
        });
    }
    if (config_.max_servers == 0 ||
        config_.max_comments_per_server == 0 ||
        config_.max_total_comments == 0 ||
        config_.max_match_tickets == 0 ||
        config_.max_nat_registrations == 0 ||
        config_.max_pending_responses == 0 ||
        config_.max_queued_events == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "Directory capacity limits must be >= 1"
        });
    }
    return {};
}

void DirectoryServer::Impl::service() {
    if (!running_) {
        return;
    }

    host_.service();

    while (auto event = host_.poll_event()) {
        switch (event->type) {
        case Event::Type::Connect:
            sessions_.try_emplace(event->connect.peer);
            break;
        case Event::Type::Disconnect:
            on_disconnect(event->disconnect.peer);
            break;
        case Event::Type::Message: {
            if (event->message.channel != config_.channel) {
                break;
            }
            auto envelope = decode_envelope(event->message.payload);
            if (!envelope.has_value()) {
                break;
            }
            if (envelope->kind != EnvelopeKind::Request) {
                break;
            }
            handle_request(event->message.peer, *envelope);
            break;
        }
        case Event::Type::FileOffer:
        case Event::Type::FileProgress:
        case Event::Type::FileComplete:
        case Event::Type::FileRejected:
            break;
        }
    }

    const TimePoint now = Clock::now();
    prune_stale_servers(now);
    run_matchmaking(now);
}

void DirectoryServer::Impl::on_disconnect(PeerId peer) {
    auto session_it = sessions_.find(peer);
    if (session_it != sessions_.end()) {
        std::vector<std::uint64_t> servers_to_remove(session_it->second.owned_servers.begin(), session_it->second.owned_servers.end());
        for (const std::uint64_t server_id : servers_to_remove) {
            remove_server(server_id);
        }

        for (const std::uint64_t ticket_id : session_it->second.tickets) {
            tickets_.erase(ticket_id);
        }

        if (session_it->second.nat_identity.has_value()) {
            auto nat_it = nat_by_identity_.find(*session_it->second.nat_identity);
            if (nat_it != nat_by_identity_.end() && nat_it->second.owner == peer) {
                nat_by_identity_.erase(nat_it);
            }
        }

        sessions_.erase(session_it);
    }
}

void DirectoryServer::Impl::prune_stale_servers(TimePoint now) {
    std::vector<std::uint64_t> stale;
    stale.reserve(servers_.size());
    for (const auto& [server_id, state] : servers_) {
        if ((now - state.last_heartbeat) > config_.server_ttl) {
            stale.push_back(server_id);
        }
    }
    for (const std::uint64_t server_id : stale) {
        remove_server(server_id);
    }
}

void DirectoryServer::Impl::run_matchmaking(TimePoint now) {
    if (tickets_.size() < 2 || config_.match_size < 2) {
        return;
    }

    std::vector<MatchTicket*> ordered;
    ordered.reserve(tickets_.size());
    for (auto& [_, ticket] : tickets_) {
        ordered.push_back(&ticket);
    }
    std::sort(ordered.begin(), ordered.end(), [](const MatchTicket* a, const MatchTicket* b) {
        return a->queued_at < b->queued_at;
    });

    std::unordered_set<std::uint64_t> matched_ids;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        MatchTicket* first = ordered[i];
        if (matched_ids.contains(first->ticket_id)) {
            continue;
        }

        for (std::size_t j = i + 1; j < ordered.size(); ++j) {
            MatchTicket* second = ordered[j];
            if (matched_ids.contains(second->ticket_id)) {
                continue;
            }

            if (first->profile.region != second->profile.region ||
                first->profile.playlist != second->profile.playlist) {
                continue;
            }

            const std::uint16_t total_party_size =
                static_cast<std::uint16_t>(first->profile.party_size) +
                static_cast<std::uint16_t>(second->profile.party_size);
            if (total_party_size != config_.match_size) {
                continue;
            }

            const auto wait_first =
                std::chrono::duration_cast<std::chrono::seconds>(now - first->queued_at).count();
            const auto wait_second =
                std::chrono::duration_cast<std::chrono::seconds>(now - second->queued_at).count();

            const auto to_tolerance = [&](std::int64_t waited_seconds) -> std::int64_t {
                const std::int64_t base = static_cast<std::int64_t>(config_.base_mmr_tolerance);
                const std::int64_t growth =
                    waited_seconds * static_cast<std::int64_t>(config_.mmr_tolerance_per_second);
                return std::max<std::int64_t>(0, base + growth);
            };

            const std::int64_t tolerance = std::max(
                to_tolerance(static_cast<std::int64_t>(wait_first)),
                to_tolerance(static_cast<std::int64_t>(wait_second)));

            const std::int64_t mmr_first = static_cast<std::int64_t>(first->profile.mmr);
            const std::int64_t mmr_second = static_cast<std::int64_t>(second->profile.mmr);
            const std::int64_t mmr_diff =
                (mmr_first >= mmr_second) ? (mmr_first - mmr_second) : (mmr_second - mmr_first);
            if (mmr_diff > tolerance) {
                continue;
            }

            const std::uint64_t match_id = next_match_id_++;
            const std::int32_t average_mmr = static_cast<std::int32_t>((mmr_first + mmr_second) / 2);
            std::vector<std::string> players;
            players.push_back(first->profile.player_id.empty() ? default_identity_for_peer(first->owner) : first->profile.player_id);
            players.push_back(second->profile.player_id.empty() ? default_identity_for_peer(second->owner) : second->profile.player_id);

            send_match_found_event(*first, match_id, average_mmr, players);
            send_match_found_event(*second, match_id, average_mmr, players);

            matched_ids.insert(first->ticket_id);
            matched_ids.insert(second->ticket_id);
            break;
        }
    }

    for (const std::uint64_t ticket_id : matched_ids) {
        auto it = tickets_.find(ticket_id);
        if (it == tickets_.end()) {
            continue;
        }
        const PeerId owner = it->second.owner;
        auto session_it = sessions_.find(owner);
        if (session_it != sessions_.end()) {
            session_it->second.tickets.erase(ticket_id);
        }
        tickets_.erase(it);
    }
}

void DirectoryServer::Impl::handle_request(PeerId peer, const Envelope& envelope) {
    switch (envelope.op) {
    case OpCode::RegisterServer:
        handle_register_server(peer, envelope);
        break;
    case OpCode::HeartbeatServer:
        handle_heartbeat_server(peer, envelope);
        break;
    case OpCode::ListServers:
        handle_list_servers(peer, envelope);
        break;
    case OpCode::PostComment:
        handle_post_comment(peer, envelope);
        break;
    case OpCode::LikeComment:
        handle_like_comment(peer, envelope);
        break;
    case OpCode::ListComments:
        handle_list_comments(peer, envelope);
        break;
    case OpCode::EnqueueMatchmaking:
        handle_enqueue_matchmaking(peer, envelope);
        break;
    case OpCode::CancelMatchmaking:
        handle_cancel_matchmaking(peer, envelope);
        break;
    case OpCode::RegisterNatIdentity:
        handle_register_nat_identity(peer, envelope);
        break;
    case OpCode::RequestNatPunch:
        handle_request_nat_punch(peer, envelope);
        break;
    case OpCode::MatchFoundEvent:
    case OpCode::NatPunchEvent:
        break;
    default:
        send_error(peer, envelope.op, envelope.request_id, "Unsupported operation");
        break;
    }
}

void DirectoryServer::Impl::handle_register_server(PeerId peer, const Envelope& envelope) {
    ByteReader reader(envelope.payload);
    ServerRegistration registration{};
    if (!reader.str(registration.name) || !reader.str(registration.map) ||
        !reader.str(registration.mode) || !reader.str(registration.region) ||
        !reader.u16(registration.max_players) || !reader.u16(registration.current_players) ||
        !reader.u16(registration.connect_port) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed register_server payload");
        return;
    }
    if (registration.max_players == 0 || registration.current_players > registration.max_players) {
        send_error(peer, envelope.op, envelope.request_id, "Invalid player counts in register_server");
        return;
    }
    if (servers_.size() >= config_.max_servers) {
        send_error(peer, envelope.op, envelope.request_id, "Directory server capacity reached");
        return;
    }

    const std::uint64_t server_id = next_server_id_++;
    const auto address = host_.peer_address(peer);
    if (!address.has_value()) {
        send_error(peer, envelope.op, envelope.request_id, "Failed to resolve peer address");
        return;
    }

    RegisteredServerState state{};
    state.info.server_id = server_id;
    state.info.name = registration.name;
    state.info.map = registration.map;
    state.info.mode = registration.mode;
    state.info.region = registration.region;
    state.info.max_players = registration.max_players;
    state.info.current_players = registration.current_players;
    state.info.address = *address;
    state.owner = peer;
    state.connect_port = registration.connect_port;
    state.last_heartbeat = Clock::now();
    servers_[server_id] = std::move(state);
    sessions_[peer].owned_servers.insert(server_id);

    ByteWriter writer;
    writer.u8(1);
    writer.u64(server_id);
    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, std::move(writer).take());
}

void DirectoryServer::Impl::handle_heartbeat_server(PeerId peer, const Envelope& envelope) {
    ByteReader reader(envelope.payload);
    std::uint64_t server_id = 0;
    std::uint16_t current_players = 0;
    std::uint16_t max_players = 0;
    std::uint16_t connect_port = 0;
    if (!reader.u64(server_id) || !reader.u16(current_players) || !reader.u16(max_players) || !reader.u16(connect_port) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed heartbeat payload");
        return;
    }
    if (max_players == 0 || current_players > max_players) {
        send_error(peer, envelope.op, envelope.request_id, "Invalid player counts in heartbeat");
        return;
    }

    auto it = servers_.find(server_id);
    if (it == servers_.end() || it->second.owner != peer) {
        send_error(peer, envelope.op, envelope.request_id, "Unknown server_id");
        return;
    }

    it->second.info.current_players = current_players;
    it->second.info.max_players = max_players;
    if (connect_port != 0) {
        it->second.connect_port = connect_port;
    }
    it->second.last_heartbeat = Clock::now();

    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, make_ok_payload());
}

void DirectoryServer::Impl::handle_list_servers(PeerId peer, const Envelope& envelope) {
    ByteReader reader(envelope.payload);
    std::string region_filter{};
    std::string mode_filter{};
    if (!reader.str(region_filter) || !reader.str(mode_filter) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed list_servers payload");
        return;
    }

    ByteWriter writer;
    writer.u8(1);

    struct EncodedServerCandidate {
        const RegisteredServerState* state{nullptr};
        Address address{};
        std::uint32_t comment_count{};
    };

    std::vector<EncodedServerCandidate> candidates;
    candidates.reserve(servers_.size());
    for (const auto& [server_id, state] : servers_) {
        if (!region_filter.empty() && state.info.region != region_filter) {
            continue;
        }
        if (!mode_filter.empty() && state.info.mode != mode_filter) {
            continue;
        }

        const auto owner_address = host_.peer_address(state.owner);
        if (!owner_address.has_value()) {
            continue;
        }

        const std::uint16_t connect_port = (state.connect_port == 0) ? owner_address->port() : state.connect_port;
        auto connect_address = Address::from_ip(owner_address->ip(), connect_port);
        if (!connect_address.has_value()) {
            continue;
        }

        std::uint32_t comment_count = 0;
        const auto comment_it = comments_by_server_.find(server_id);
        if (comment_it != comments_by_server_.end()) {
            comment_count = static_cast<std::uint32_t>(std::min<std::size_t>(
                comment_it->second.size(),
                std::numeric_limits<std::uint32_t>::max()));
        }
        candidates.push_back(EncodedServerCandidate{
            .state = &state,
            .address = *connect_address,
            .comment_count = comment_count
        });
    }

    if (candidates.size() > std::numeric_limits<std::uint16_t>::max()) {
        send_error(peer, envelope.op, envelope.request_id, "Too many servers");
        return;
    }

    writer.u16(static_cast<std::uint16_t>(candidates.size()));
    for (const auto& candidate : candidates) {
        const ServerInfo& info = candidate.state->info;
        writer.u64(info.server_id);
        if (!writer.str(info.name) || !writer.str(info.map) ||
            !writer.str(info.mode) || !writer.str(info.region)) {
            send_error(peer, envelope.op, envelope.request_id, "Server fields exceed limits");
            return;
        }
        writer.u16(info.max_players);
        writer.u16(info.current_players);
        if (const auto addr_result = write_address(writer, candidate.address); !addr_result.has_value()) {
            send_error(peer, envelope.op, envelope.request_id, addr_result.error().message);
            return;
        }
        writer.u32(candidate.comment_count);
    }

    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, std::move(writer).take());
}

void DirectoryServer::Impl::handle_post_comment(PeerId peer, const Envelope& envelope) {
    (void)peer;
    ByteReader reader(envelope.payload);
    std::uint64_t server_id = 0;
    std::uint64_t parent_id = 0;
    std::string author{};
    std::string text{};
    if (!reader.u64(server_id) || !reader.u64(parent_id) || !reader.str(author) || !reader.str(text) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed post_comment payload");
        return;
    }

    if (!servers_.contains(server_id)) {
        send_error(peer, envelope.op, envelope.request_id, "Unknown server_id");
        return;
    }
    if (author.empty() || text.empty()) {
        send_error(peer, envelope.op, envelope.request_id, "Comment author/text must not be empty");
        return;
    }
    if (comments_.size() >= config_.max_total_comments) {
        send_error(peer, envelope.op, envelope.request_id, "Comment capacity reached");
        return;
    }
    const auto comments_for_server = comments_by_server_.find(server_id);
    if (comments_for_server != comments_by_server_.end() &&
        comments_for_server->second.size() >= config_.max_comments_per_server) {
        send_error(peer, envelope.op, envelope.request_id, "Server comment capacity reached");
        return;
    }
    if (parent_id != 0) {
        auto parent_it = comments_.find(parent_id);
        if (parent_it == comments_.end() || parent_it->second.comment.server_id != server_id) {
            send_error(peer, envelope.op, envelope.request_id, "Invalid parent_id");
            return;
        }
    }

    ServerComment comment{};
    comment.comment_id = next_comment_id_++;
    comment.server_id = server_id;
    comment.parent_id = parent_id;
    comment.author = std::move(author);
    comment.text = std::move(text);
    comment.likes = 0;

    CommentState state{};
    state.comment = comment;
    comments_[comment.comment_id] = std::move(state);
    comments_by_server_[server_id].push_back(comment.comment_id);

    ByteWriter writer;
    writer.u8(1);
    writer.u64(comment.comment_id);
    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, std::move(writer).take());
}

void DirectoryServer::Impl::handle_like_comment(PeerId peer, const Envelope& envelope) {
    (void)peer;
    ByteReader reader(envelope.payload);
    std::uint64_t server_id = 0;
    std::uint64_t comment_id = 0;
    std::string user_id{};
    if (!reader.u64(server_id) || !reader.u64(comment_id) || !reader.str(user_id) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed like_comment payload");
        return;
    }

    auto comment_it = comments_.find(comment_id);
    if (comment_it == comments_.end() || comment_it->second.comment.server_id != server_id) {
        send_error(peer, envelope.op, envelope.request_id, "Unknown comment_id");
        return;
    }
    if (user_id.empty()) {
        send_error(peer, envelope.op, envelope.request_id, "user_id must not be empty");
        return;
    }

    comment_it->second.liked_by.insert(std::move(user_id));
    comment_it->second.comment.likes = static_cast<std::uint32_t>(comment_it->second.liked_by.size());

    ByteWriter writer;
    writer.u8(1);
    writer.u32(comment_it->second.comment.likes);
    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, std::move(writer).take());
}

void DirectoryServer::Impl::handle_list_comments(PeerId peer, const Envelope& envelope) {
    (void)peer;
    ByteReader reader(envelope.payload);
    std::uint64_t server_id = 0;
    if (!reader.u64(server_id) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed list_comments payload");
        return;
    }

    ByteWriter writer;
    writer.u8(1);

    const auto it = comments_by_server_.find(server_id);
    std::vector<const ServerComment*> serializable;
    if (it != comments_by_server_.end()) {
        serializable.reserve(it->second.size());
        for (const std::uint64_t comment_id : it->second) {
            const auto comment_it = comments_.find(comment_id);
            if (comment_it != comments_.end()) {
                serializable.push_back(&comment_it->second.comment);
            }
        }
    }

    const std::size_t count = serializable.size();
    if (count > std::numeric_limits<std::uint16_t>::max()) {
        send_error(peer, envelope.op, envelope.request_id, "Too many comments");
        return;
    }

    writer.u16(static_cast<std::uint16_t>(count));
    for (const ServerComment* comment : serializable) {
        writer.u64(comment->comment_id);
        writer.u64(comment->server_id);
        writer.u64(comment->parent_id);
        if (!writer.str(comment->author) || !writer.str(comment->text)) {
            send_error(peer, envelope.op, envelope.request_id, "Comment fields exceed limits");
            return;
        }
        writer.u32(comment->likes);
    }

    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, std::move(writer).take());
}

void DirectoryServer::Impl::handle_enqueue_matchmaking(PeerId peer, const Envelope& envelope) {
    ByteReader reader(envelope.payload);
    MatchmakingProfile profile{};
    if (!reader.str(profile.player_id) || !reader.str(profile.region) || !reader.str(profile.playlist) ||
        !reader.i32(profile.mmr) || !reader.u8(profile.party_size) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed enqueue_matchmaking payload");
        return;
    }
    if (profile.party_size == 0) {
        send_error(peer, envelope.op, envelope.request_id, "party_size must be >= 1");
        return;
    }
    if (profile.party_size > config_.match_size) {
        send_error(peer, envelope.op, envelope.request_id, "party_size exceeds match_size");
        return;
    }
    if (profile.region.empty() || profile.playlist.empty()) {
        send_error(peer, envelope.op, envelope.request_id, "region/playlist must not be empty");
        return;
    }
    if (tickets_.size() >= config_.max_match_tickets) {
        send_error(peer, envelope.op, envelope.request_id, "Matchmaking capacity reached");
        return;
    }

    MatchTicket ticket{};
    ticket.ticket_id = next_ticket_id_++;
    ticket.owner = peer;
    ticket.profile = std::move(profile);
    ticket.queued_at = Clock::now();
    tickets_[ticket.ticket_id] = ticket;
    sessions_[peer].tickets.insert(ticket.ticket_id);

    ByteWriter writer;
    writer.u8(1);
    writer.u64(ticket.ticket_id);
    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, std::move(writer).take());
}

void DirectoryServer::Impl::handle_cancel_matchmaking(PeerId peer, const Envelope& envelope) {
    ByteReader reader(envelope.payload);
    std::uint64_t ticket_id = 0;
    if (!reader.u64(ticket_id) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed cancel_matchmaking payload");
        return;
    }

    auto it = tickets_.find(ticket_id);
    if (it == tickets_.end() || it->second.owner != peer) {
        send_error(peer, envelope.op, envelope.request_id, "Unknown ticket_id");
        return;
    }

    tickets_.erase(it);
    sessions_[peer].tickets.erase(ticket_id);
    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, make_ok_payload());
}

void DirectoryServer::Impl::handle_register_nat_identity(PeerId peer, const Envelope& envelope) {
    ByteReader reader(envelope.payload);
    std::string identity{};
    if (!reader.str(identity) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed register_nat_identity payload");
        return;
    }
    if (identity.empty()) {
        send_error(peer, envelope.op, envelope.request_id, "Identity must not be empty");
        return;
    }
    const auto existing = nat_by_identity_.find(identity);
    if (existing != nat_by_identity_.end() && existing->second.owner != peer) {
        send_error(peer, envelope.op, envelope.request_id, "Identity already in use");
        return;
    }
    if (!nat_by_identity_.contains(identity) && nat_by_identity_.size() >= config_.max_nat_registrations) {
        send_error(peer, envelope.op, envelope.request_id, "NAT registration capacity reached");
        return;
    }

    auto address = host_.peer_address(peer);
    if (!address.has_value()) {
        send_error(peer, envelope.op, envelope.request_id, "Failed to resolve peer public endpoint");
        return;
    }

    auto& session = sessions_[peer];
    if (session.nat_identity.has_value() && *session.nat_identity != identity) {
        auto old_it = nat_by_identity_.find(*session.nat_identity);
        if (old_it != nat_by_identity_.end() && old_it->second.owner == peer) {
            nat_by_identity_.erase(old_it);
        }
    }

    NatRegistration registration{};
    registration.identity = identity;
    registration.owner = peer;
    registration.public_address = *address;
    registration.updated_at = Clock::now();
    nat_by_identity_[identity] = std::move(registration);
    session.nat_identity = identity;

    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, make_ok_payload());
}

void DirectoryServer::Impl::handle_request_nat_punch(PeerId peer, const Envelope& envelope) {
    ByteReader reader(envelope.payload);
    std::string target_identity{};
    if (!reader.str(target_identity) || !reader.done()) {
        send_error(peer, envelope.op, envelope.request_id, "Malformed request_nat_punch payload");
        return;
    }

    const auto requester_address = host_.peer_address(peer);
    if (!requester_address.has_value()) {
        send_error(peer, envelope.op, envelope.request_id, "Failed to resolve requester endpoint");
        return;
    }

    auto target_it = nat_by_identity_.find(target_identity);
    if (target_it == nat_by_identity_.end()) {
        send_error(peer, envelope.op, envelope.request_id, "Unknown NAT identity");
        return;
    }
    if (target_it->second.owner == peer) {
        send_error(peer, envelope.op, envelope.request_id, "Cannot punch to self");
        return;
    }

    ByteWriter response;
    response.u8(1);
    if (!response.str(target_identity)) {
        send_error(peer, envelope.op, envelope.request_id, "Identity too long");
        return;
    }
    if (const auto address_result = write_address(response, target_it->second.public_address); !address_result.has_value()) {
        send_error(peer, envelope.op, envelope.request_id, address_result.error().message);
        return;
    }
    (void)send_payload(peer, EnvelopeKind::Response, envelope.op, envelope.request_id, std::move(response).take());

    const auto session_it = sessions_.find(peer);
    const std::string requester_identity =
        (session_it != sessions_.end() && session_it->second.nat_identity.has_value())
            ? *session_it->second.nat_identity
            : default_identity_for_peer(peer);
    send_nat_punch_event(target_it->second.owner, requester_identity, *requester_address);
}

bool DirectoryServer::Impl::send_payload(PeerId peer, EnvelopeKind kind, OpCode op, std::uint32_t request_id, std::vector<std::byte> payload) {
    Envelope envelope{};
    envelope.kind = kind;
    envelope.op = op;
    envelope.request_id = request_id;
    envelope.payload = std::move(payload);

    SendOptions options{};
    options.channel = config_.channel;
    options.delivery = Delivery::ReliableOrdered;
    auto sent = host_.send(peer, encode_envelope(envelope), options);
    if (!sent.has_value()) {
        last_error_ = sent.error();
        return false;
    }
    return true;
}

void DirectoryServer::Impl::send_error(PeerId peer, OpCode op, std::uint32_t request_id, std::string_view message) {
    (void)send_payload(peer, EnvelopeKind::Response, op, request_id, make_error_payload(message));
}

void DirectoryServer::Impl::send_match_found_event(
    const MatchTicket& ticket,
    std::uint64_t match_id,
    std::int32_t average_mmr,
    const std::vector<std::string>& players) {
    ByteWriter payload;
    payload.u64(ticket.ticket_id);
    payload.u64(match_id);
    if (!payload.str(ticket.profile.region) || !payload.str(ticket.profile.playlist)) {
        return;
    }
    payload.i32(average_mmr);
    if (players.size() > std::numeric_limits<std::uint16_t>::max()) {
        return;
    }
    payload.u16(static_cast<std::uint16_t>(players.size()));
    for (const std::string& player : players) {
        if (!payload.str(player)) {
            return;
        }
    }

    (void)send_payload(ticket.owner, EnvelopeKind::Event, OpCode::MatchFoundEvent, 0, std::move(payload).take());
}

void DirectoryServer::Impl::send_nat_punch_event(
    PeerId target_peer,
    std::string_view requester_identity,
    const Address& requester_address) {
    ByteWriter payload;
    if (!payload.str(requester_identity)) {
        return;
    }
    if (const auto result = write_address(payload, requester_address); !result.has_value()) {
        return;
    }
    (void)send_payload(target_peer, EnvelopeKind::Event, OpCode::NatPunchEvent, 0, std::move(payload).take());
}

void DirectoryServer::Impl::remove_server(std::uint64_t server_id) {
    auto server_it = servers_.find(server_id);
    if (server_it == servers_.end()) {
        return;
    }

    const PeerId owner = server_it->second.owner;
    auto session_it = sessions_.find(owner);
    if (session_it != sessions_.end()) {
        session_it->second.owned_servers.erase(server_id);
    }

    auto comments_it = comments_by_server_.find(server_id);
    if (comments_it != comments_by_server_.end()) {
        for (const std::uint64_t comment_id : comments_it->second) {
            comments_.erase(comment_id);
        }
        comments_by_server_.erase(comments_it);
    }

    servers_.erase(server_it);
}

class DirectoryClient::Impl {
public:
    explicit Impl(DirectoryConfig config)
        : config_(std::move(config)),
          host_(config_.host) {}

    struct StoredResponse {
        OpCode op{OpCode::RegisterServer};
        std::vector<std::byte> payload{};
    };

    [[nodiscard]] std::expected<void, Error> validate_config() const {
        if (config_.host.channel_count == 0 || config_.channel >= config_.host.channel_count) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidChannel,
                .message = "Directory channel is outside host channel_count"
            });
        }
        if (config_.request_timeout <= std::chrono::milliseconds::zero() ||
            config_.connect_timeout <= std::chrono::milliseconds::zero()) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "Directory client timeouts must be > 0"
            });
        }
        if (config_.max_pending_responses == 0 || config_.max_queued_events == 0) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "Directory client limits must be >= 1"
            });
        }
        return {};
    }

    [[nodiscard]] std::expected<void, Error> start_node(std::uint16_t listen_port, std::string_view bind_ip) {
        if (const auto valid = validate_config(); !valid.has_value()) {
            return std::unexpected(valid.error());
        }
        auto result = host_.start_p2p(listen_port, bind_ip);
        if (!result.has_value()) {
            last_error_ = result.error();
            return std::unexpected(result.error());
        }
        return {};
    }

    [[nodiscard]] std::expected<void, Error> connect(const Address& directory) {
        if (const auto valid = validate_config(); !valid.has_value()) {
            return std::unexpected(valid.error());
        }
        if (!directory.is_valid()) {
            return std::unexpected(Error{
                .code = ErrorCode::AddressResolutionFailed,
                .message = "Invalid directory address"
            });
        }

        auto connect_result = host_.connect(directory);
        if (!connect_result.has_value()) {
            last_error_ = connect_result.error();
            return std::unexpected(connect_result.error());
        }
        directory_peer_ = *connect_result;

        const TimePoint deadline = Clock::now() + config_.connect_timeout;
        while (Clock::now() < deadline) {
            service();
            if (connected_) {
                return {};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "Timed out connecting to directory"
        });
    }

    void service() {
        host_.service();

        while (auto event = host_.poll_event()) {
            if (event->type == Event::Type::Connect) {
                if (event->connect.peer == directory_peer_) {
                    connected_ = true;
                } else {
                    push_network_event(std::move(*event));
                }
                continue;
            }

            if (event->type == Event::Type::Disconnect) {
                if (event->disconnect.peer == directory_peer_) {
                    connected_ = false;
                    last_error_ = Error{
                        .code = ErrorCode::InvalidState,
                        .message = "Disconnected from directory"
                    };
                } else {
                    push_network_event(std::move(*event));
                }
                continue;
            }

            if (event->type != Event::Type::Message) {
                push_network_event(std::move(*event));
                continue;
            }

            if (event->message.peer != directory_peer_ || event->message.channel != config_.channel) {
                push_network_event(std::move(*event));
                continue;
            }

            auto envelope = decode_envelope(event->message.payload);
            if (!envelope.has_value()) {
                continue;
            }

            if (envelope->kind == EnvelopeKind::Response) {
                const bool is_new = !responses_.contains(envelope->request_id);
                responses_[envelope->request_id] = StoredResponse{
                    .op = envelope->op,
                    .payload = std::move(envelope->payload)
                };
                if (is_new) {
                    response_order_.push_back(envelope->request_id);
                }
                while (response_order_.size() > config_.max_pending_responses) {
                    const std::uint32_t evicted = response_order_.front();
                    response_order_.pop_front();
                    responses_.erase(evicted);
                }
                continue;
            }

            if (envelope->kind == EnvelopeKind::Event) {
                handle_event(*envelope);
                continue;
            }
        }
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return connected_;
    }

    [[nodiscard]] std::optional<Error> last_error() const {
        return last_error_;
    }

    [[nodiscard]] std::optional<MatchFoundEvent> poll_match_found() {
        if (match_events_.empty()) {
            return std::nullopt;
        }
        MatchFoundEvent event = std::move(match_events_.front());
        match_events_.pop_front();
        return event;
    }

    [[nodiscard]] std::optional<NatPunchEvent> poll_nat_punch() {
        if (nat_events_.empty()) {
            return std::nullopt;
        }
        NatPunchEvent event = std::move(nat_events_.front());
        nat_events_.pop_front();
        return event;
    }

    [[nodiscard]] std::optional<Event> poll_network_event() {
        if (network_events_.empty()) {
            return std::nullopt;
        }
        Event event = std::move(network_events_.front());
        network_events_.pop_front();
        return event;
    }

    [[nodiscard]] Host& transport() noexcept {
        return host_;
    }

    [[nodiscard]] std::expected<StoredResponse, Error> send_request_wait(OpCode op, std::vector<std::byte> payload) {
        if (!connected_ || directory_peer_ == invalid_peer_id) {
            return std::unexpected(Error{
                .code = ErrorCode::InvalidState,
                .message = "Directory client is not connected"
            });
        }

        std::uint32_t request_id = 0;
        bool allocated = false;
        const std::size_t max_attempts = responses_.size() + 1;
        for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
            const std::uint32_t candidate = next_request_id_++;
            if (candidate == 0 || responses_.contains(candidate)) {
                continue;
            }
            request_id = candidate;
            allocated = true;
            break;
        }
        if (!allocated) {
            return std::unexpected(Error{
                .code = ErrorCode::CapacityExceeded,
                .message = "No free directory request id slots"
            });
        }

        Envelope envelope{};
        envelope.kind = EnvelopeKind::Request;
        envelope.op = op;
        envelope.request_id = request_id;
        envelope.payload = std::move(payload);

        SendOptions options{};
        options.channel = config_.channel;
        options.delivery = Delivery::ReliableOrdered;
        auto sent = host_.send(directory_peer_, encode_envelope(envelope), options);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());
        }

        const TimePoint deadline = Clock::now() + config_.request_timeout;
        while (Clock::now() < deadline) {
            service();

            auto it = responses_.find(request_id);
            if (it != responses_.end()) {
                StoredResponse response = std::move(it->second);
                responses_.erase(it);
                auto order_it = std::find(response_order_.begin(), response_order_.end(), request_id);
                if (order_it != response_order_.end()) {
                    response_order_.erase(order_it);
                }
                if (response.op != op) {
                    return std::unexpected(Error{
                        .code = ErrorCode::ProtocolMismatch,
                        .message = "Response opcode mismatch"
                    });
                }
                return response;
            }

            if (!connected_) {
                return std::unexpected(Error{
                    .code = ErrorCode::InvalidState,
                    .message = "Directory disconnected while waiting for response"
                });
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "Timed out waiting for directory response"
        });
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, Error> send_request_success_payload(OpCode op, std::vector<std::byte> payload) {
        auto response = send_request_wait(op, std::move(payload));
        if (!response.has_value()) {
            last_error_ = response.error();
            return std::unexpected(response.error());
        }

        auto success_payload = open_success_payload(response->payload);
        if (!success_payload.has_value()) {
            last_error_ = success_payload.error();
            return std::unexpected(success_payload.error());
        }
        return success_payload;
    }

private:
    void push_network_event(Event event) {
        if (network_events_.size() >= config_.max_queued_events) {
            network_events_.pop_front();
        }
        network_events_.push_back(std::move(event));
    }

    void push_match_event(MatchFoundEvent event) {
        if (match_events_.size() >= config_.max_queued_events) {
            match_events_.pop_front();
        }
        match_events_.push_back(std::move(event));
    }

    void push_nat_event(NatPunchEvent event) {
        if (nat_events_.size() >= config_.max_queued_events) {
            nat_events_.pop_front();
        }
        nat_events_.push_back(std::move(event));
    }

    void handle_event(const Envelope& envelope) {
        if (envelope.op == OpCode::MatchFoundEvent) {
            ByteReader reader(envelope.payload);
            MatchFoundEvent event{};
            std::uint16_t count = 0;
            if (!reader.u64(event.ticket_id) || !reader.u64(event.match_id) ||
                !reader.str(event.region) || !reader.str(event.playlist) ||
                !reader.i32(event.average_mmr) || !reader.u16(count)) {
                return;
            }
            event.players.reserve(count);
            for (std::uint16_t i = 0; i < count; ++i) {
                std::string id{};
                if (!reader.str(id)) {
                    return;
                }
                event.players.push_back(std::move(id));
            }
            if (!reader.done()) {
                return;
            }
            push_match_event(std::move(event));
            return;
        }

        if (envelope.op == OpCode::NatPunchEvent) {
            ByteReader reader(envelope.payload);
            NatPunchEvent event{};
            if (!reader.str(event.requester_identity)) {
                return;
            }
            auto address = read_address(reader);
            if (!address.has_value() || !reader.done()) {
                return;
            }
            event.requester_address = *address;
            push_nat_event(std::move(event));
        }
    }

public:
    DirectoryConfig config_{};
    Host host_{};
    bool connected_{false};
    PeerId directory_peer_{invalid_peer_id};
    std::optional<Error> last_error_{};
    std::uint32_t next_request_id_{1};
    std::unordered_map<std::uint32_t, StoredResponse> responses_{};
    std::deque<std::uint32_t> response_order_{};
    std::deque<MatchFoundEvent> match_events_{};
    std::deque<NatPunchEvent> nat_events_{};
    std::deque<Event> network_events_{};
};

DirectoryServer::DirectoryServer(DirectoryConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

DirectoryServer::~DirectoryServer() = default;
DirectoryServer::DirectoryServer(DirectoryServer&&) noexcept = default;
DirectoryServer& DirectoryServer::operator=(DirectoryServer&&) noexcept = default;

std::expected<void, Error> DirectoryServer::start(std::uint16_t port, std::string_view bind_ip) {
    return impl_->start(port, bind_ip);
}

void DirectoryServer::service() {
    impl_->service();
}

bool DirectoryServer::is_running() const noexcept {
    return impl_->is_running();
}

std::optional<Error> DirectoryServer::last_error() const {
    return impl_->last_error();
}

DirectoryClient::DirectoryClient(DirectoryConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

DirectoryClient::~DirectoryClient() = default;
DirectoryClient::DirectoryClient(DirectoryClient&&) noexcept = default;
DirectoryClient& DirectoryClient::operator=(DirectoryClient&&) noexcept = default;

std::expected<void, Error> DirectoryClient::start_node(std::uint16_t listen_port, std::string_view bind_ip) {
    return impl_->start_node(listen_port, bind_ip);
}

std::expected<void, Error> DirectoryClient::connect(const Address& directory) {
    return impl_->connect(directory);
}

void DirectoryClient::service() {
    impl_->service();
}

bool DirectoryClient::is_connected() const noexcept {
    return impl_->is_connected();
}

std::optional<Error> DirectoryClient::last_error() const {
    return impl_->last_error();
}

std::expected<std::uint64_t, Error> DirectoryClient::register_server(const ServerRegistration& registration) {
    if (registration.max_players == 0 || registration.current_players > registration.max_players) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "register_server requires max_players >= 1 and current_players <= max_players"
        });
    }

    ByteWriter payload;
    if (!payload.str(registration.name) || !payload.str(registration.map) ||
        !payload.str(registration.mode) || !payload.str(registration.region)) {
        return std::unexpected(Error{
            .code = ErrorCode::MessageTooLarge,
            .message = "Server registration text fields exceed limits"
        });
    }
    payload.u16(registration.max_players);
    payload.u16(registration.current_players);
    payload.u16(registration.connect_port);

    auto raw_payload = impl_->send_request_success_payload(OpCode::RegisterServer, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);

    std::uint64_t server_id = 0;
    if (!reader.u64(server_id) || !reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed register_server response"
        });
    }
    return server_id;
}

std::expected<void, Error> DirectoryClient::heartbeat_server(
    std::uint64_t server_id,
    std::uint16_t current_players,
    std::uint16_t max_players,
    std::uint16_t connect_port) {
    if (max_players == 0 || current_players > max_players) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "heartbeat_server requires max_players >= 1 and current_players <= max_players"
        });
    }

    ByteWriter payload;
    payload.u64(server_id);
    payload.u16(current_players);
    payload.u16(max_players);
    payload.u16(connect_port);

    auto raw_payload = impl_->send_request_success_payload(OpCode::HeartbeatServer, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);
    if (!reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed heartbeat_server response"
        });
    }
    return {};
}

std::expected<std::vector<ServerInfo>, Error> DirectoryClient::list_servers(std::string_view region_filter, std::string_view mode_filter) {
    ByteWriter payload;
    if (!payload.str(region_filter) || !payload.str(mode_filter)) {
        return std::unexpected(Error{
            .code = ErrorCode::MessageTooLarge,
            .message = "Filter fields exceed limits"
        });
    }

    auto raw_payload = impl_->send_request_success_payload(OpCode::ListServers, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);

    std::uint16_t count = 0;
    if (!reader.u16(count)) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed list_servers response"
        });
    }

    std::vector<ServerInfo> servers;
    servers.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        ServerInfo info{};
        std::string ip{};
        std::uint16_t port = 0;
        if (!reader.u64(info.server_id) || !reader.str(info.name) || !reader.str(info.map) ||
            !reader.str(info.mode) || !reader.str(info.region) || !reader.u16(info.max_players) ||
            !reader.u16(info.current_players) || !reader.str(ip) || !reader.u16(port) ||
            !reader.u32(info.comment_count)) {
            return std::unexpected(Error{
                .code = ErrorCode::ProtocolMismatch,
                .message = "Malformed server entry"
            });
        }

        auto address = Address::from_ip(ip, port);
        if (!address.has_value()) {
            return std::unexpected(address.error());
        }
        info.address = *address;
        servers.push_back(std::move(info));
    }

    if (!reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Trailing bytes in list_servers response"
        });
    }

    return servers;
}

std::expected<std::uint64_t, Error> DirectoryClient::post_comment(
    std::uint64_t server_id,
    std::string_view author,
    std::string_view text,
    std::uint64_t parent_id) {
    if (author.empty() || text.empty()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "post_comment requires non-empty author and text"
        });
    }

    ByteWriter payload;
    payload.u64(server_id);
    payload.u64(parent_id);
    if (!payload.str(author) || !payload.str(text)) {
        return std::unexpected(Error{
            .code = ErrorCode::MessageTooLarge,
            .message = "Comment fields exceed limits"
        });
    }

    auto raw_payload = impl_->send_request_success_payload(OpCode::PostComment, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);

    std::uint64_t comment_id = 0;
    if (!reader.u64(comment_id) || !reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed post_comment response"
        });
    }
    return comment_id;
}

std::expected<void, Error> DirectoryClient::like_comment(
    std::uint64_t server_id,
    std::uint64_t comment_id,
    std::string_view user_id) {
    if (user_id.empty()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "like_comment requires a non-empty user_id"
        });
    }

    ByteWriter payload;
    payload.u64(server_id);
    payload.u64(comment_id);
    if (!payload.str(user_id)) {
        return std::unexpected(Error{
            .code = ErrorCode::MessageTooLarge,
            .message = "user_id exceeds limits"
        });
    }

    auto raw_payload = impl_->send_request_success_payload(OpCode::LikeComment, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);

    std::uint32_t likes = 0;
    if (!reader.u32(likes) || !reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed like_comment response"
        });
    }
    (void)likes;
    return {};
}

std::expected<std::vector<ServerComment>, Error> DirectoryClient::list_comments(std::uint64_t server_id) {
    ByteWriter payload;
    payload.u64(server_id);

    auto raw_payload = impl_->send_request_success_payload(OpCode::ListComments, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);

    std::uint16_t count = 0;
    if (!reader.u16(count)) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed list_comments response"
        });
    }

    std::vector<ServerComment> comments;
    comments.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        ServerComment comment{};
        if (!reader.u64(comment.comment_id) || !reader.u64(comment.server_id) ||
            !reader.u64(comment.parent_id) || !reader.str(comment.author) ||
            !reader.str(comment.text) || !reader.u32(comment.likes)) {
            return std::unexpected(Error{
                .code = ErrorCode::ProtocolMismatch,
                .message = "Malformed comment entry"
            });
        }
        comments.push_back(std::move(comment));
    }

    if (!reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Trailing bytes in list_comments response"
        });
    }
    return comments;
}

std::expected<std::uint64_t, Error> DirectoryClient::enqueue_matchmaking(const MatchmakingProfile& profile) {
    ByteWriter payload;
    if (!payload.str(profile.player_id) || !payload.str(profile.region) || !payload.str(profile.playlist)) {
        return std::unexpected(Error{
            .code = ErrorCode::MessageTooLarge,
            .message = "Matchmaking fields exceed limits"
        });
    }
    payload.i32(profile.mmr);
    payload.u8(profile.party_size);

    auto raw_payload = impl_->send_request_success_payload(OpCode::EnqueueMatchmaking, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);

    std::uint64_t ticket_id = 0;
    if (!reader.u64(ticket_id) || !reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed enqueue_matchmaking response"
        });
    }
    return ticket_id;
}

std::expected<void, Error> DirectoryClient::cancel_matchmaking(std::uint64_t ticket_id) {
    ByteWriter payload;
    payload.u64(ticket_id);

    auto raw_payload = impl_->send_request_success_payload(OpCode::CancelMatchmaking, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);
    if (!reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed cancel_matchmaking response"
        });
    }
    return {};
}

std::optional<MatchFoundEvent> DirectoryClient::poll_match_found() {
    return impl_->poll_match_found();
}

std::expected<void, Error> DirectoryClient::register_nat_identity(std::string_view identity) {
    ByteWriter payload;
    if (!payload.str(identity)) {
        return std::unexpected(Error{
            .code = ErrorCode::MessageTooLarge,
            .message = "Identity exceeds limits"
        });
    }

    auto raw_payload = impl_->send_request_success_payload(OpCode::RegisterNatIdentity, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);
    if (!reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed register_nat_identity response"
        });
    }
    return {};
}

std::expected<Address, Error> DirectoryClient::request_nat_punch(std::string_view target_identity) {
    ByteWriter payload;
    if (!payload.str(target_identity)) {
        return std::unexpected(Error{
            .code = ErrorCode::MessageTooLarge,
            .message = "Target identity exceeds limits"
        });
    }

    auto raw_payload = impl_->send_request_success_payload(OpCode::RequestNatPunch, std::move(payload).take());
    if (!raw_payload.has_value()) {
        return std::unexpected(raw_payload.error());
    }
    ByteReader reader(*raw_payload);

    std::string echoed_target{};
    if (!reader.str(echoed_target)) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed request_nat_punch response"
        });
    }
    if (echoed_target != target_identity) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "NAT punch target mismatch"
        });
    }
    auto address = read_address(reader);
    if (!address.has_value() || !reader.done()) {
        return std::unexpected(Error{
            .code = ErrorCode::ProtocolMismatch,
            .message = "Malformed NAT punch address response"
        });
    }
    (void)echoed_target;
    return *address;
}

std::optional<NatPunchEvent> DirectoryClient::poll_nat_punch() {
    return impl_->poll_nat_punch();
}

std::optional<Event> DirectoryClient::poll_network_event() {
    return impl_->poll_network_event();
}

Host& DirectoryClient::transport() noexcept {
    return impl_->transport();
}

}  // namespace unet
