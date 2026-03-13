#pragma once

#include "unet/error.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace unet::detail {

struct UpnpRequest {
    std::uint16_t internal_port{};
    std::uint16_t external_port{};
    std::string protocol{"UDP"};
    std::string internal_client{};
    std::string description{"unet"};
    std::chrono::seconds lease_duration{0};
    std::string discovery_address{"239.255.255.250"};
    std::uint16_t discovery_port{1900};
    std::chrono::milliseconds discovery_timeout{1500};
};

struct UpnpMapping {
    std::string service_type{};
    std::string control_url{};
    std::string external_ip{};
    std::string internal_client{};
    std::uint16_t external_port{};
    std::uint16_t internal_port{};
    std::string protocol{};
};

[[nodiscard]] std::expected<UpnpMapping, Error> upnp_add_port_mapping(const UpnpRequest& request);
[[nodiscard]] std::expected<void, Error> upnp_remove_port_mapping(const UpnpMapping& mapping, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
[[nodiscard]] std::expected<std::string, Error> detect_outbound_ipv4(std::string_view remote_host, std::uint16_t remote_port);

}  // namespace unet::detail

