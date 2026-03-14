#pragma once

#include "unet/address.hpp"
#include "unet/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace unet {

struct ConnectEvent {
    PeerId peer{invalid_peer_id};
    Address address{};
};

struct DisconnectEvent {
    PeerId peer{invalid_peer_id};
    DisconnectReason reason{DisconnectReason::Requested};
};

struct MessageEvent {
    PeerId peer{invalid_peer_id};
    std::uint8_t channel{};
    Delivery delivery{Delivery::ReliableOrdered};
    std::vector<std::byte> payload{};
};

struct StreamOpenEvent {
    PeerId peer{invalid_peer_id};
    StreamId stream{invalid_stream_id};
    std::uint8_t channel{};
    bool bidirectional{true};
    bool remote_initiated{};
};

struct StreamDataEvent {
    PeerId peer{invalid_peer_id};
    StreamId stream{invalid_stream_id};
    std::vector<std::byte> payload{};
    bool fin{};
};

struct StreamCloseEvent {
    PeerId peer{invalid_peer_id};
    StreamId stream{invalid_stream_id};
    std::uint64_t error_code{};
    bool remote{};
};

struct FileOfferEvent {
    PeerId peer{invalid_peer_id};
    std::uint64_t transfer_id{};
    std::string filename{};
    std::uint64_t total_bytes{};
    bool auto_accepted{};
};

struct FileProgressEvent {
    PeerId peer{invalid_peer_id};
    std::uint64_t transfer_id{};
    std::uint64_t bytes_transferred{};
    std::uint64_t total_bytes{};
    bool upload{};
};

struct FileCompleteEvent {
    PeerId peer{invalid_peer_id};
    std::uint64_t transfer_id{};
    std::string filename{};
    std::uint64_t total_bytes{};
    std::string path{};
    bool upload{};
};

struct FileRejectedEvent {
    PeerId peer{invalid_peer_id};
    std::uint64_t transfer_id{};
    std::string reason{};
};

struct Event {
    enum class Type {
        Connect,
        Disconnect,
        Message,
        StreamOpen,
        StreamData,
        StreamClose,
        FileOffer,
        FileProgress,
        FileComplete,
        FileRejected
    };

    Type type{Type::Connect};
    ConnectEvent connect{};
    DisconnectEvent disconnect{};
    MessageEvent message{};
    StreamOpenEvent stream_open{};
    StreamDataEvent stream_data{};
    StreamCloseEvent stream_close{};
    FileOfferEvent file_offer{};
    FileProgressEvent file_progress{};
    FileCompleteEvent file_complete{};
    FileRejectedEvent file_rejected{};
};

}  // namespace unet
