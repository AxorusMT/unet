#include "unet/address.hpp"

#include "detail/address_native.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <string>

namespace unet {

namespace {

std::string_view family_to_default_ip(int family) {
    if (family == AF_INET6) {
        return "::";
    }
    return "0.0.0.0";
}

std::expected<Address, Error> resolve_impl(std::string_view host, std::uint16_t port, bool numeric_only, bool prefer_ipv6) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_ADDRCONFIG;
    if (numeric_only) {
        hints.ai_flags |= AI_NUMERICHOST;
    }

    std::array<char, 8> service{};
    const auto [ptr, ec] = std::to_chars(service.data(), service.data() + service.size(), port);
    if (ec != std::errc{}) {
        return std::unexpected(Error{
            .code = ErrorCode::AddressResolutionFailed,
            .message = "Invalid port number"
        });
    }
    *ptr = '\0';

    const std::string host_storage = host.empty() ? std::string(family_to_default_ip(prefer_ipv6 ? AF_INET6 : AF_INET)) : std::string(host);

    addrinfo* result = nullptr;
    const int status = getaddrinfo(host_storage.c_str(), service.data(), &hints, &result);
    if (status != 0 || result == nullptr) {
        return std::unexpected(Error{
            .code = ErrorCode::AddressResolutionFailed,
            .message = gai_strerror(status)
        });
    }

    Address output{};
    const addrinfo* selected = result;
    if (prefer_ipv6) {
        for (const addrinfo* it = result; it != nullptr; it = it->ai_next) {
            if (it->ai_family == AF_INET6) {
                selected = it;
                break;
            }
        }
    } else {
        for (const addrinfo* it = result; it != nullptr; it = it->ai_next) {
            if (it->ai_family == AF_INET) {
                selected = it;
                break;
            }
        }
    }

    detail::NativeAddressAccess::set(output, selected->ai_addr, static_cast<detail::SockLen>(selected->ai_addrlen));
    freeaddrinfo(result);

    return output;
}

}  // namespace

std::expected<Address, Error> Address::resolve(std::string_view host, std::uint16_t port, bool prefer_ipv6) {
    return resolve_impl(host, port, false, prefer_ipv6);
}

std::expected<Address, Error> Address::from_ip(std::string_view ip, std::uint16_t port) {
    return resolve_impl(ip, port, true, true);
}

bool Address::is_valid() const noexcept {
    return length_ > 0;
}

std::string Address::ip() const {
    if (!is_valid()) {
        return {};
    }

    std::array<char, NI_MAXHOST> host{};
    const int status = getnameinfo(
        detail::NativeAddressAccess::sockaddr_ref(*this),
        detail::NativeAddressAccess::length(*this),
        host.data(),
        static_cast<detail::SockLen>(host.size()),
        nullptr,
        0,
        NI_NUMERICHOST);
    if (status != 0) {
        return {};
    }
    return std::string(host.data());
}

std::uint16_t Address::port() const {
    if (!is_valid()) {
        return 0;
    }

    const sockaddr* native = detail::NativeAddressAccess::sockaddr_ref(*this);
    if (native->sa_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(native)->sin_port);
    }
    if (native->sa_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(native)->sin6_port);
    }
    return 0;
}

std::string Address::to_string() const {
    if (!is_valid()) {
        return "<invalid>";
    }

    if (detail::NativeAddressAccess::sockaddr_ref(*this)->sa_family == AF_INET6) {
        return "[" + ip() + "]:" + std::to_string(port());
    }
    return ip() + ":" + std::to_string(port());
}

bool Address::operator==(const Address& rhs) const noexcept {
    if (length_ == 0 || rhs.length_ == 0) {
        return false;
    }

    const sockaddr* left = detail::NativeAddressAccess::sockaddr_ref(*this);
    const sockaddr* right = detail::NativeAddressAccess::sockaddr_ref(rhs);
    if (left->sa_family != right->sa_family) {
        return false;
    }

    if (left->sa_family == AF_INET) {
        const auto* la = reinterpret_cast<const sockaddr_in*>(left);
        const auto* ra = reinterpret_cast<const sockaddr_in*>(right);
        return la->sin_port == ra->sin_port && la->sin_addr.s_addr == ra->sin_addr.s_addr;
    }

    if (left->sa_family == AF_INET6) {
        const auto* la = reinterpret_cast<const sockaddr_in6*>(left);
        const auto* ra = reinterpret_cast<const sockaddr_in6*>(right);
        return la->sin6_port == ra->sin6_port &&
               std::memcmp(&la->sin6_addr, &ra->sin6_addr, sizeof(in6_addr)) == 0;
    }

    return false;
}

}  // namespace unet

