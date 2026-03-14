#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace unet {

using PeerId = std::uint16_t;
inline constexpr PeerId invalid_peer_id = 0;
using StreamId = std::uint64_t;
inline constexpr StreamId invalid_stream_id = 0;

enum class Transport : std::uint8_t {
    Udp = 0,
    Tcp = 1,
    Quic = 2
};

enum class Delivery : std::uint8_t {
    Unreliable = 0,
    UnreliableSequenced = 1,
    ReliableOrdered = 2
};

enum class DisconnectReason : std::uint16_t {
    Requested = 0,
    Timeout = 1,
    RemoteClosed = 2,
    ProtocolError = 3
};

struct SendOptions {
    std::uint8_t channel{0};
    Delivery delivery{Delivery::ReliableOrdered};
};

struct PeerStats {
    std::uint64_t packets_sent{};
    std::uint64_t packets_received{};
    std::uint64_t bytes_sent{};
    std::uint64_t bytes_received{};
    std::uint64_t reliable_retransmissions{};
    double smoothed_rtt_ms{};
    std::chrono::milliseconds connected_for{};
};

}  // namespace unet
