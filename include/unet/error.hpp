#pragma once

#include <string>

namespace unet {

enum class ErrorCode {
    None = 0,
    SocketInitializationFailed,
    SocketOpenFailed,
    SocketBindFailed,
    SocketOptionFailed,
    SocketSendFailed,
    SocketReceiveFailed,
    AddressResolutionFailed,
    InvalidState,
    InvalidPeer,
    InvalidChannel,
    InvalidPacket,
    ProtocolMismatch,
    MessageTooLarge,
    CapacityExceeded,
    FileOpenFailed,
    FileReadFailed,
    FileWriteFailed,
    FileTransferNotFound,
    FileTransferStateInvalid,
    UpnpDiscoveryFailed,
    UpnpDescriptionFailed,
    UpnpControlFailed
};

struct Error {
    ErrorCode code{ErrorCode::None};
    std::string message{};
};

}  // namespace unet
