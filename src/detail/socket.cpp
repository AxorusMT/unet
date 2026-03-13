#include "detail/socket.hpp"

#include "detail/address_native.hpp"

#include <array>
#include <string>

namespace unet::detail {

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& rhs) noexcept
    : handle_(rhs.handle_) {
    rhs.handle_ = kInvalidSocket;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& rhs) noexcept {
    if (this == &rhs) {
        return *this;
    }
    close();
    handle_ = rhs.handle_;
    rhs.handle_ = kInvalidSocket;
    return *this;
}

std::expected<void, Error> UdpSocket::open(std::string_view bind_ip, std::uint16_t port, bool enable_ipv6) {
    close();

    const auto resolved = Address::resolve(bind_ip, port, enable_ipv6);
    if (!resolved.has_value()) {
        return std::unexpected(resolved.error());
    }

    const Address bind_address = *resolved;
    const sockaddr* native = NativeAddressAccess::sockaddr_ref(bind_address);
    const int family = native->sa_family;

    handle_ = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (handle_ == kInvalidSocket) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOpenFailed,
            .message = socket_error_message(last_socket_error())
        });
    }

#if !defined(_WIN32)
    const int one = 1;
    setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#endif

    if (const auto result = set_non_blocking(handle_); !result.has_value()) {
        close();
        return std::unexpected(result.error());
    }

#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
    if (family == AF_INET6) {
        const int dual_stack = 0;
        setsockopt(handle_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&dual_stack), sizeof(dual_stack));
    }
#endif

    if (::bind(handle_, native, NativeAddressAccess::length(bind_address)) != 0) {
        const Error error{
            .code = ErrorCode::SocketBindFailed,
            .message = socket_error_message(last_socket_error())
        };
        close();
        return std::unexpected(error);
    }

    return {};
}

std::expected<std::size_t, Error> UdpSocket::send_to(std::span<const std::byte> bytes, const Address& address) {
    if (!is_open()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "Socket is not open"
        });
    }

    const int sent = ::sendto(
        handle_,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        NativeAddressAccess::sockaddr_ref(address),
        NativeAddressAccess::length(address));

    if (sent < 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketSendFailed,
            .message = socket_error_message(last_socket_error())
        });
    }

    return static_cast<std::size_t>(sent);
}

std::expected<std::optional<ReceiveResult>, Error> UdpSocket::receive(std::span<std::byte> bytes) {
    if (!is_open()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidState,
            .message = "Socket is not open"
        });
    }

    Address from{};
    SockLen from_len = NativeAddressAccess::capacity();
    int received = ::recvfrom(
        handle_,
        reinterpret_cast<char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        NativeAddressAccess::sockaddr_mut(from),
        &from_len);

    if (received < 0) {
        const int err = last_socket_error();
#if defined(_WIN32)
        if (err == WSAEWOULDBLOCK) {
            return std::optional<ReceiveResult>{};
        }
#else
        if (err == EWOULDBLOCK || err == EAGAIN) {
            return std::optional<ReceiveResult>{};
        }
#endif
        return std::unexpected(Error{
            .code = ErrorCode::SocketReceiveFailed,
            .message = socket_error_message(err)
        });
    }

    NativeAddressAccess::set_length(from, from_len);
    return std::optional<ReceiveResult>(ReceiveResult{
        .from = from,
        .size = static_cast<std::size_t>(received)
    });
}

bool UdpSocket::is_open() const noexcept {
    return handle_ != kInvalidSocket;
}

void UdpSocket::close() noexcept {
    if (handle_ == kInvalidSocket) {
        return;
    }
    close_socket(handle_);
    handle_ = kInvalidSocket;
}

}  // namespace unet::detail
