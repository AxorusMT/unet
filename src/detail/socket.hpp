#pragma once

#include "detail/platform.hpp"
#include "unet/address.hpp"
#include "unet/error.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace unet::detail {

struct ReceiveResult {
    Address from{};
    std::size_t size{};
};

class UdpSocket final {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& rhs) noexcept;
    UdpSocket& operator=(UdpSocket&& rhs) noexcept;

    [[nodiscard]] std::expected<void, Error> open(std::string_view bind_ip, std::uint16_t port, bool enable_ipv6);
    [[nodiscard]] std::expected<std::size_t, Error> send_to(std::span<const std::byte> bytes, const Address& address);
    [[nodiscard]] std::expected<std::optional<ReceiveResult>, Error> receive(std::span<std::byte> bytes);

    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

private:
    SocketHandle handle_{kInvalidSocket};
};

}  // namespace unet::detail

