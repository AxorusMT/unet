#pragma once

#include "detail/platform.hpp"
#include "unet/address.hpp"

#include <cstring>

namespace unet::detail {

struct NativeAddressAccess {
    static sockaddr* sockaddr_mut(Address& address) noexcept {
        return reinterpret_cast<sockaddr*>(address.storage_.data());
    }

    static const sockaddr* sockaddr_ref(const Address& address) noexcept {
        return reinterpret_cast<const sockaddr*>(address.storage_.data());
    }

    static SockLen length(const Address& address) noexcept {
        return static_cast<SockLen>(address.length_);
    }

    static SockLen capacity() noexcept {
        Address address{};
        return static_cast<SockLen>(address.storage_.size());
    }

    static void set_length(Address& address, SockLen len) noexcept {
        address.length_ = static_cast<std::uint16_t>(len);
    }

    static void set(Address& address, const sockaddr* native, SockLen len) noexcept {
        if (native == nullptr || len <= 0) {
            address.length_ = 0;
            return;
        }

        const auto clamped = static_cast<std::size_t>(len) > address.storage_.size()
            ? address.storage_.size()
            : static_cast<std::size_t>(len);
        std::memcpy(address.storage_.data(), native, clamped);
        address.length_ = static_cast<std::uint16_t>(clamped);
    }
};

}  // namespace unet::detail
