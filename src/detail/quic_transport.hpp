#pragma once

#include "unet/address.hpp"
#include "unet/error.hpp"
#include "unet/event.hpp"
#include "unet/host.hpp"
#include "unet/types.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace unet::detail {

class QuicTransport final {
public:
    explicit QuicTransport(HostConfig config);
    ~QuicTransport();

    QuicTransport(const QuicTransport&) = delete;
    QuicTransport& operator=(const QuicTransport&) = delete;
    QuicTransport(QuicTransport&&) = delete;
    QuicTransport& operator=(QuicTransport&&) = delete;

    [[nodiscard]] static bool available() noexcept;

    [[nodiscard]] std::expected<void, Error> start_server(std::uint16_t port, std::string_view bind_ip);
    [[nodiscard]] std::expected<void, Error> start_p2p(std::uint16_t port, std::string_view bind_ip);
    [[nodiscard]] std::expected<PeerId, Error> connect(const Address& remote);
    [[nodiscard]] std::expected<void, Error> send(PeerId peer, std::span<const std::byte> bytes, SendOptions options);
    [[nodiscard]] std::expected<void, Error> disconnect(PeerId peer, DisconnectReason reason);
    [[nodiscard]] std::expected<StreamId, Error> open_stream(PeerId peer, StreamOpenOptions options);
    [[nodiscard]] std::expected<void, Error> send_stream(PeerId peer, StreamId stream, std::span<const std::byte> bytes, StreamSendOptions options);
    [[nodiscard]] std::expected<void, Error> close_stream(PeerId peer, StreamId stream, std::uint64_t error_code);

    void service();
    [[nodiscard]] std::optional<Event> poll_event();

    [[nodiscard]] std::vector<PeerId> connected_peers() const;
    [[nodiscard]] std::optional<Address> peer_address(PeerId peer) const;
    [[nodiscard]] std::optional<PeerStats> peer_stats(PeerId peer) const;
    [[nodiscard]] bool is_peer_connected(PeerId peer) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace unet::detail

