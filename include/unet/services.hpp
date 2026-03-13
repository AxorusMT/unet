#pragma once

#include "unet/address.hpp"
#include "unet/error.hpp"
#include "unet/event.hpp"
#include "unet/host.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace unet {

struct DirectoryConfig {
    HostConfig host{};
    std::uint8_t channel{0};
    std::chrono::milliseconds request_timeout{2500};
    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds server_ttl{10000};
    std::int32_t base_mmr_tolerance{150};
    std::int32_t mmr_tolerance_per_second{30};
    std::uint8_t match_size{2};
    std::size_t max_servers{4096};
    std::size_t max_comments_per_server{1024};
    std::size_t max_total_comments{131072};
    std::size_t max_match_tickets{8192};
    std::size_t max_nat_registrations{8192};
    std::size_t max_pending_responses{4096};
    std::size_t max_queued_events{4096};
};

struct ServerRegistration {
    std::string name{};
    std::string map{};
    std::string mode{};
    std::string region{};
    std::uint16_t max_players{};
    std::uint16_t current_players{};
    std::uint16_t connect_port{};
};

struct ServerInfo {
    std::uint64_t server_id{};
    std::string name{};
    std::string map{};
    std::string mode{};
    std::string region{};
    std::uint16_t max_players{};
    std::uint16_t current_players{};
    Address address{};
    std::uint32_t comment_count{};
};

struct ServerComment {
    std::uint64_t comment_id{};
    std::uint64_t server_id{};
    std::uint64_t parent_id{};
    std::string author{};
    std::string text{};
    std::uint32_t likes{};
};

struct MatchmakingProfile {
    std::string player_id{};
    std::string region{};
    std::string playlist{};
    std::int32_t mmr{};
    std::uint8_t party_size{1};
};

struct MatchFoundEvent {
    std::uint64_t ticket_id{};
    std::uint64_t match_id{};
    std::string region{};
    std::string playlist{};
    std::int32_t average_mmr{};
    std::vector<std::string> players{};
};

struct NatPunchEvent {
    std::string requester_identity{};
    Address requester_address{};
};

class DirectoryServer final {
public:
    explicit DirectoryServer(DirectoryConfig config = {});
    ~DirectoryServer();

    DirectoryServer(const DirectoryServer&) = delete;
    DirectoryServer& operator=(const DirectoryServer&) = delete;
    DirectoryServer(DirectoryServer&&) noexcept;
    DirectoryServer& operator=(DirectoryServer&&) noexcept;

    [[nodiscard]] std::expected<void, Error> start(std::uint16_t port, std::string_view bind_ip = "::");
    void service();
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::optional<Error> last_error() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class DirectoryClient final {
public:
    explicit DirectoryClient(DirectoryConfig config = {});
    ~DirectoryClient();

    DirectoryClient(const DirectoryClient&) = delete;
    DirectoryClient& operator=(const DirectoryClient&) = delete;
    DirectoryClient(DirectoryClient&&) noexcept;
    DirectoryClient& operator=(DirectoryClient&&) noexcept;

    [[nodiscard]] std::expected<void, Error> start_node(std::uint16_t listen_port, std::string_view bind_ip = "::");
    [[nodiscard]] std::expected<void, Error> connect(const Address& directory);
    void service();
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] std::optional<Error> last_error() const;

    [[nodiscard]] std::expected<std::uint64_t, Error> register_server(const ServerRegistration& registration);
    [[nodiscard]] std::expected<void, Error> heartbeat_server(std::uint64_t server_id, std::uint16_t current_players, std::uint16_t max_players, std::uint16_t connect_port = 0);
    [[nodiscard]] std::expected<std::vector<ServerInfo>, Error> list_servers(std::string_view region_filter = {}, std::string_view mode_filter = {});

    [[nodiscard]] std::expected<std::uint64_t, Error> post_comment(std::uint64_t server_id, std::string_view author, std::string_view text, std::uint64_t parent_id = 0);
    [[nodiscard]] std::expected<void, Error> like_comment(std::uint64_t server_id, std::uint64_t comment_id, std::string_view user_id);
    [[nodiscard]] std::expected<std::vector<ServerComment>, Error> list_comments(std::uint64_t server_id);

    [[nodiscard]] std::expected<std::uint64_t, Error> enqueue_matchmaking(const MatchmakingProfile& profile);
    [[nodiscard]] std::expected<void, Error> cancel_matchmaking(std::uint64_t ticket_id);
    [[nodiscard]] std::optional<MatchFoundEvent> poll_match_found();

    [[nodiscard]] std::expected<void, Error> register_nat_identity(std::string_view identity);
    [[nodiscard]] std::expected<Address, Error> request_nat_punch(std::string_view target_identity);
    [[nodiscard]] std::optional<NatPunchEvent> poll_nat_punch();

    [[nodiscard]] std::optional<Event> poll_network_event();
    [[nodiscard]] Host& transport() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace unet
