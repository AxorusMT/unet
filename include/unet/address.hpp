#pragma once

#include "unet/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace unet::detail {
struct NativeAddressAccess;
}

namespace unet {

class Address final {
public:
    Address() = default;

    [[nodiscard]] static std::expected<Address, Error> resolve(std::string_view host, std::uint16_t port, bool prefer_ipv6 = true);
    [[nodiscard]] static std::expected<Address, Error> from_ip(std::string_view ip, std::uint16_t port);

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] std::string ip() const;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const Address& rhs) const noexcept;

private:
    friend struct detail::NativeAddressAccess;

    std::array<std::byte, 128> storage_{};
    std::uint16_t length_{0};
};

}  // namespace unet

