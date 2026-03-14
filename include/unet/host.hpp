#pragma once

#include "unet/address.hpp"
#include "unet/error.hpp"
#include "unet/event.hpp"
#include "unet/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace unet {

struct HostConfig {
    std::uint32_t protocol_id{0x554E4554u};  // "UNET"
    Transport transport{Transport::Udp};
    std::uint16_t max_peers{128};
    std::uint8_t channel_count{8};
    std::size_t mtu{1200};
    std::size_t max_fragments_per_message{2048};
    std::size_t max_pending_reliable_messages{2048};
    std::size_t max_pending_reassembly_messages{2048};
    std::size_t max_sent_packet_history{8192};
    std::size_t max_event_queue{8192};
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds ack_flush_interval{16};
    std::chrono::milliseconds disconnect_timeout{10000};
    std::chrono::milliseconds reliable_resend_interval{250};
    std::chrono::milliseconds connect_retry_interval{500};
    std::chrono::milliseconds fragment_timeout{5000};
    bool enable_ipv6{true};
    std::uint8_t file_transfer_channel{7};
    std::size_t file_chunk_size{32 * 1024};
    bool auto_accept_file_transfers{true};
    std::string download_directory{"downloads"};
    bool enable_upnp{false};
    bool require_upnp{false};
    std::uint16_t upnp_external_port{0};
    std::chrono::milliseconds upnp_discovery_timeout{1500};
    std::chrono::seconds upnp_lease_duration{0};
    std::string upnp_description{"unet"};
    std::string upnp_discovery_address{"239.255.255.250"};
    std::uint16_t upnp_discovery_port{1900};
    std::string quic_alpn{"unet"};
    bool quic_insecure_skip_verify{true};
    std::string quic_certificate_thumbprint{};
    std::string quic_certificate_store_name{"MY"};
    bool quic_certificate_store_machine{false};
    std::string quic_certificate_file{};
    std::string quic_private_key_file{};
    std::string quic_private_key_password{};
    std::string quic_pkcs12_file{};
    std::string quic_pkcs12_password{};
};

struct FileSendOptions {
    std::string remote_name{};
};

struct StreamOpenOptions {
    std::uint8_t channel{0};
    bool bidirectional{true};
};

struct StreamSendOptions {
    bool fin{false};
};

class Host final {
public:
    explicit Host(HostConfig config = {});
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    Host(Host&&) noexcept;
    Host& operator=(Host&&) noexcept;

    [[nodiscard]] std::expected<void, Error> start_server(std::uint16_t port, std::string_view bind_ip = "::");
    [[nodiscard]] std::expected<void, Error> start_p2p(std::uint16_t port, std::string_view bind_ip = "::");
    [[nodiscard]] std::expected<PeerId, Error> connect(const Address& remote);
    [[nodiscard]] std::expected<void, Error> send(PeerId peer, std::span<const std::byte> bytes, SendOptions options = {});
    [[nodiscard]] std::expected<StreamId, Error> open_stream(PeerId peer, StreamOpenOptions options = {});
    [[nodiscard]] std::expected<void, Error> send_stream(PeerId peer, StreamId stream, std::span<const std::byte> bytes, StreamSendOptions options = {});
    [[nodiscard]] std::expected<void, Error> close_stream(PeerId peer, StreamId stream, std::uint64_t error_code = 0);
    [[nodiscard]] std::expected<std::uint64_t, Error> send_file(PeerId peer, const std::filesystem::path& local_path, FileSendOptions options = {});
    [[nodiscard]] std::expected<void, Error> accept_file(PeerId peer, std::uint64_t transfer_id, const std::filesystem::path& destination_path);
    [[nodiscard]] std::expected<void, Error> reject_file(PeerId peer, std::uint64_t transfer_id, std::string_view reason = "rejected");
    [[nodiscard]] std::expected<void, Error> disconnect(PeerId peer, DisconnectReason reason = DisconnectReason::Requested);

    void service();
    [[nodiscard]] std::optional<Event> poll_event();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_server() const noexcept;
    [[nodiscard]] std::vector<PeerId> connected_peers() const;
    [[nodiscard]] std::optional<Address> peer_address(PeerId peer) const;
    [[nodiscard]] std::optional<PeerStats> peer_stats(PeerId peer) const;
    [[nodiscard]] std::optional<Address> upnp_external_address() const;
    [[nodiscard]] std::optional<Error> upnp_last_error() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view text);
[[nodiscard]] std::string to_string(std::span<const std::byte> bytes);

}  // namespace unet
