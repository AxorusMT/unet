#include "detail/upnp.hpp"

#include "detail/platform.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace unet::detail {

namespace {

using Clock = std::chrono::steady_clock;

struct ParsedUrl {
    std::string host{};
    std::uint16_t port{80};
    std::string path{"/"};
};

struct HttpResponse {
    int status{0};
    std::string headers{};
    std::string body{};
};

bool is_connect_in_progress(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
    return err == EINPROGRESS || err == EALREADY || err == EWOULDBLOCK || err == EAGAIN;
#endif
}

bool is_retryable_block(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK;
#else
    return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

bool is_socket_timeout(int err) {
#if defined(_WIN32)
    return err == WSAETIMEDOUT;
#else
    return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        begin += 1;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end -= 1;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string lower_copy(std::string_view value) {
    std::string lower(value.size(), '\0');
    std::transform(value.begin(), value.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower;
}

std::optional<ParsedUrl> parse_http_url(std::string_view url) {
    constexpr std::string_view prefix = "http://";
    if (!url.starts_with(prefix)) {
        return std::nullopt;
    }

    std::string_view rest = url.substr(prefix.size());
    const std::size_t path_pos = rest.find('/');
    std::string_view authority = (path_pos == std::string_view::npos) ? rest : rest.substr(0, path_pos);
    std::string_view path = (path_pos == std::string_view::npos) ? std::string_view("/") : rest.substr(path_pos);
    if (authority.empty()) {
        return std::nullopt;
    }

    ParsedUrl parsed{};
    parsed.path = std::string(path.empty() ? std::string_view("/") : path);

    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos || close <= 1) {
            return std::nullopt;
        }
        parsed.host = std::string(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::nullopt;
            }
            std::uint16_t port = 0;
            const auto [_, ec] = std::from_chars(
                authority.data() + static_cast<std::ptrdiff_t>(close + 2),
                authority.data() + static_cast<std::ptrdiff_t>(authority.size()),
                port);
            if (ec != std::errc{}) {
                return std::nullopt;
            }
            parsed.port = port;
        }
        return parsed;
    }

    const std::size_t last_colon = authority.rfind(':');
    if (last_colon != std::string_view::npos && authority.find(':') == last_colon) {
        std::uint16_t port = 0;
        const auto [_, ec] = std::from_chars(
            authority.data() + static_cast<std::ptrdiff_t>(last_colon + 1),
            authority.data() + static_cast<std::ptrdiff_t>(authority.size()),
            port);
        if (ec == std::errc{}) {
            parsed.host = std::string(authority.substr(0, last_colon));
            parsed.port = port;
            return parsed;
        }
    }

    parsed.host = std::string(authority);
    return parsed;
}

std::string format_origin(const ParsedUrl& parsed) {
    const bool needs_brackets = parsed.host.find(':') != std::string::npos;
    const std::string host = needs_brackets ? ("[" + parsed.host + "]") : parsed.host;
    return "http://" + host + ":" + std::to_string(parsed.port);
}

std::optional<std::string> extract_tag(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const std::size_t begin = xml.find(open);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t value_begin = begin + open.size();
    const std::size_t end = xml.find(close, value_begin);
    if (end == std::string_view::npos || end < value_begin) {
        return std::nullopt;
    }
    return trim_copy(xml.substr(value_begin, end - value_begin));
}

std::optional<std::pair<std::string, std::string>> find_upnp_control_service(std::string_view xml) {
    std::size_t search = 0;
    while (true) {
        const std::size_t service_begin = xml.find("<service>", search);
        if (service_begin == std::string_view::npos) {
            return std::nullopt;
        }
        const std::size_t service_end = xml.find("</service>", service_begin);
        if (service_end == std::string_view::npos) {
            return std::nullopt;
        }

        const std::string_view block = xml.substr(service_begin, service_end - service_begin);
        const auto service_type = extract_tag(block, "serviceType");
        const auto control_url = extract_tag(block, "controlURL");
        if (service_type.has_value() && control_url.has_value()) {
            if (service_type->find("WANIPConnection") != std::string::npos ||
                service_type->find("WANPPPConnection") != std::string::npos) {
                return std::pair<std::string, std::string>{*service_type, *control_url};
            }
        }

        search = service_end + std::string_view("</service>").size();
    }
}

std::optional<std::string> parse_ssdp_location(std::string_view response) {
    std::size_t pos = 0;
    while (pos < response.size()) {
        const std::size_t line_end = response.find('\n', pos);
        const std::size_t end = (line_end == std::string_view::npos) ? response.size() : line_end;
        std::string line = trim_copy(response.substr(pos, end - pos));
        if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string name = lower_copy(line.substr(0, colon));
                if (name == "location") {
                    return trim_copy(std::string_view(line).substr(colon + 1));
                }
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        pos = line_end + 1;
    }
    return std::nullopt;
}

std::string xml_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '\'':
            out += "&apos;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::expected<void, Error> set_blocking_mode(SocketHandle handle, bool blocking) {
#if defined(_WIN32)
    u_long value = blocking ? 0 : 1;
    if (ioctlsocket(handle, FIONBIO, &value) != 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOptionFailed,
            .message = socket_error_message(last_socket_error())
        });
    }
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOptionFailed,
            .message = socket_error_message(last_socket_error())
        });
    }
    const int next_flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    if (fcntl(handle, F_SETFL, next_flags) != 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOptionFailed,
            .message = socket_error_message(last_socket_error())
        });
    }
#endif
    return {};
}

void set_socket_timeouts(SocketHandle handle, std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    const DWORD ms = static_cast<DWORD>(std::max<std::int64_t>(1, timeout.count()));
    (void)setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    (void)setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    const auto clamped = std::max<std::int64_t>(1, timeout.count());
    tv.tv_sec = static_cast<long>(clamped / 1000);
    tv.tv_usec = static_cast<long>((clamped % 1000) * 1000);
    (void)setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    (void)setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
}

bool wait_for_socket(SocketHandle handle, bool writable, std::chrono::milliseconds timeout) {
    fd_set set{};
    FD_ZERO(&set);
    FD_SET(handle, &set);

    timeval tv{};
    const auto clamped = std::max<std::int64_t>(0, timeout.count());
    tv.tv_sec = static_cast<long>(clamped / 1000);
    tv.tv_usec = static_cast<long>((clamped % 1000) * 1000);

    const int result = writable
        ? select(static_cast<int>(handle + 1), nullptr, &set, nullptr, &tv)
        : select(static_cast<int>(handle + 1), &set, nullptr, nullptr, &tv);
    return result > 0;
}

std::expected<SocketHandle, Error> connect_tcp(const ParsedUrl& parsed, std::chrono::milliseconds timeout, ErrorCode error_code) {
    std::array<char, 8> port_chars{};
    const auto [port_end, ec] = std::to_chars(
        port_chars.data(),
        port_chars.data() + port_chars.size(),
        parsed.port);
    if (ec != std::errc{}) {
        return std::unexpected(Error{
            .code = error_code,
            .message = "Invalid URL port"
        });
    }
    *port_end = '\0';

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* resolved = nullptr;
    if (getaddrinfo(parsed.host.c_str(), port_chars.data(), &hints, &resolved) != 0 || resolved == nullptr) {
        return std::unexpected(Error{
            .code = error_code,
            .message = "Failed to resolve UPnP control endpoint"
        });
    }

    Error last_error{
        .code = error_code,
        .message = "Failed to connect to UPnP control endpoint"
    };

    for (addrinfo* it = resolved; it != nullptr; it = it->ai_next) {
        SocketHandle handle = ::socket(it->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (handle == kInvalidSocket) {
            last_error.message = socket_error_message(last_socket_error());
            continue;
        }

        if (const auto non_blocking = set_blocking_mode(handle, false); !non_blocking.has_value()) {
            last_error = non_blocking.error();
            close_socket(handle);
            continue;
        }

        const int connect_result = ::connect(handle, it->ai_addr, static_cast<SockLen>(it->ai_addrlen));
        if (connect_result != 0) {
            const int err = last_socket_error();
            if (!is_connect_in_progress(err)) {
                last_error.message = socket_error_message(err);
                close_socket(handle);
                continue;
            }

            if (!wait_for_socket(handle, true, timeout)) {
                last_error.message = "Timed out connecting to UPnP control endpoint";
                close_socket(handle);
                continue;
            }

            int so_error = 0;
            SockLen so_len = static_cast<SockLen>(sizeof(so_error));
            if (getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len) != 0 || so_error != 0) {
                last_error.message = socket_error_message(so_error == 0 ? last_socket_error() : so_error);
                close_socket(handle);
                continue;
            }
        }

        if (const auto blocking = set_blocking_mode(handle, true); !blocking.has_value()) {
            last_error = blocking.error();
            close_socket(handle);
            continue;
        }

        set_socket_timeouts(handle, timeout);
        freeaddrinfo(resolved);
        return handle;
    }

    freeaddrinfo(resolved);
    return std::unexpected(last_error);
}

std::expected<void, Error> send_all(SocketHandle handle, std::string_view bytes, ErrorCode error_code) {
    std::size_t sent_total = 0;
    while (sent_total < bytes.size()) {
        const int sent = ::send(
            handle,
            bytes.data() + static_cast<std::ptrdiff_t>(sent_total),
            static_cast<int>(bytes.size() - sent_total),
            0);
        if (sent <= 0) {
            return std::unexpected(Error{
                .code = error_code,
                .message = socket_error_message(last_socket_error())
            });
        }
        sent_total += static_cast<std::size_t>(sent);
    }
    return {};
}

std::expected<std::string, Error> receive_all(SocketHandle handle, ErrorCode error_code) {
    std::string response;
    std::array<char, 4096> buffer{};
    while (true) {
        const int received = ::recv(handle, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received > 0) {
            response.append(buffer.data(), static_cast<std::size_t>(received));
            continue;
        }
        if (received == 0) {
            break;
        }
        const int err = last_socket_error();
        if (is_socket_timeout(err) && !response.empty()) {
            break;
        }
        return std::unexpected(Error{
            .code = error_code,
            .message = socket_error_message(err)
        });
    }
    return response;
}

std::expected<HttpResponse, Error> http_request(
    std::string_view method,
    std::string_view url,
    std::string_view headers,
    std::string_view body,
    std::chrono::milliseconds timeout,
    ErrorCode error_code) {
    auto parsed = parse_http_url(url);
    if (!parsed.has_value()) {
        return std::unexpected(Error{
            .code = error_code,
            .message = "Invalid HTTP URL"
        });
    }

    auto connected = connect_tcp(*parsed, timeout, error_code);
    if (!connected.has_value()) {
        return std::unexpected(connected.error());
    }
    const SocketHandle handle = *connected;

    const bool default_port = parsed->port == 80;
    const bool needs_brackets = parsed->host.find(':') != std::string::npos;
    const std::string host_header =
        needs_brackets
            ? ("[" + parsed->host + "]")
            : parsed->host;

    std::string request;
    request.reserve(method.size() + parsed->path.size() + headers.size() + body.size() + 128);
    request.append(method);
    request.append(" ");
    request.append(parsed->path);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host_header);
    if (!default_port) {
        request.push_back(':');
        request.append(std::to_string(parsed->port));
    }
    request.append("\r\nConnection: close\r\n");
    request.append(headers);
    if (!headers.empty() && !headers.ends_with("\r\n")) {
        request.append("\r\n");
    }
    if (!body.empty()) {
        request.append("Content-Length: ");
        request.append(std::to_string(body.size()));
        request.append("\r\n");
    }
    request.append("\r\n");
    request.append(body);

    auto sent = send_all(handle, request, error_code);
    if (!sent.has_value()) {
        close_socket(handle);
        return std::unexpected(sent.error());
    }

    auto raw = receive_all(handle, error_code);
    close_socket(handle);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }

    const std::size_t split = raw->find("\r\n\r\n");
    if (split == std::string::npos) {
        return std::unexpected(Error{
            .code = error_code,
            .message = "Malformed HTTP response from UPnP endpoint"
        });
    }

    const std::size_t first_line_end = raw->find("\r\n");
    if (first_line_end == std::string::npos) {
        return std::unexpected(Error{
            .code = error_code,
            .message = "Malformed HTTP status line"
        });
    }
    const std::string_view status_line(raw->data(), first_line_end);
    const std::size_t first_space = status_line.find(' ');
    if (first_space == std::string_view::npos) {
        return std::unexpected(Error{
            .code = error_code,
            .message = "Malformed HTTP status line"
        });
    }
    int status = 0;
    const auto [_, parse_ec] = std::from_chars(
        status_line.data() + static_cast<std::ptrdiff_t>(first_space + 1),
        status_line.data() + static_cast<std::ptrdiff_t>(status_line.size()),
        status);
    if (parse_ec != std::errc{}) {
        return std::unexpected(Error{
            .code = error_code,
            .message = "Malformed HTTP status code"
        });
    }

    HttpResponse response{};
    response.status = status;
    response.headers = raw->substr(0, split);
    response.body = raw->substr(split + 4);
    return response;
}

std::expected<std::string, Error> discover_upnp_location(const UpnpRequest& request) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    std::array<char, 8> port_chars{};
    const auto [port_end, ec] = std::to_chars(
        port_chars.data(),
        port_chars.data() + port_chars.size(),
        request.discovery_port);
    if (ec != std::errc{}) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = "Invalid SSDP port"
        });
    }
    *port_end = '\0';

    addrinfo* resolved = nullptr;
    if (getaddrinfo(request.discovery_address.c_str(), port_chars.data(), &hints, &resolved) != 0 || resolved == nullptr) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = "Failed to resolve SSDP endpoint"
        });
    }

    SocketHandle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalidSocket) {
        freeaddrinfo(resolved);
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = socket_error_message(last_socket_error())
        });
    }

    auto non_blocking = set_non_blocking(handle);
    if (!non_blocking.has_value()) {
        close_socket(handle);
        freeaddrinfo(resolved);
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = non_blocking.error().message
        });
    }

    constexpr std::array<std::string_view, 2> search_targets = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "upnp:rootdevice"
    };
    for (const std::string_view st : search_targets) {
        std::string packet;
        packet.reserve(256);
        packet.append("M-SEARCH * HTTP/1.1\r\n");
        packet.append("HOST: ");
        packet.append(request.discovery_address);
        packet.push_back(':');
        packet.append(std::to_string(request.discovery_port));
        packet.append("\r\nMAN: \"ssdp:discover\"\r\n");
        packet.append("MX: 1\r\n");
        packet.append("ST: ");
        packet.append(st);
        packet.append("\r\n\r\n");

        (void)::sendto(
            handle,
            packet.data(),
            static_cast<int>(packet.size()),
            0,
            resolved->ai_addr,
            static_cast<SockLen>(resolved->ai_addrlen));
    }

    freeaddrinfo(resolved);

    const auto deadline = Clock::now() + request.discovery_timeout;
    std::array<char, 4096> buffer{};
    while (Clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            break;
        }
        if (!wait_for_socket(handle, false, remaining)) {
            continue;
        }

        sockaddr_storage from{};
        SockLen from_len = static_cast<SockLen>(sizeof(from));
        const int received = ::recvfrom(handle, buffer.data(), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received <= 0) {
            const int err = last_socket_error();
            if (is_retryable_block(err)) {
                continue;
            }
            close_socket(handle);
            return std::unexpected(Error{
                .code = ErrorCode::UpnpDiscoveryFailed,
                .message = socket_error_message(err)
            });
        }

        std::string_view response(buffer.data(), static_cast<std::size_t>(received));
        auto location = parse_ssdp_location(response);
        if (location.has_value() && !location->empty()) {
            close_socket(handle);
            return *location;
        }
    }

    close_socket(handle);
    return std::unexpected(Error{
        .code = ErrorCode::UpnpDiscoveryFailed,
        .message = "No UPnP IGD responder found"
    });
}

std::expected<std::string, Error> call_soap(
    std::string_view control_url,
    std::string_view service_type,
    std::string_view action,
    std::string_view payload_xml,
    std::chrono::milliseconds timeout) {
    std::string body;
    body.reserve(payload_xml.size() + 256);
    body.append("<?xml version=\"1.0\"?>");
    body.append("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ");
    body.append("s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">");
    body.append("<s:Body>");
    body.append("<u:");
    body.append(action);
    body.append(" xmlns:u=\"");
    body.append(service_type);
    body.append("\">");
    body.append(payload_xml);
    body.append("</u:");
    body.append(action);
    body.append(">");
    body.append("</s:Body></s:Envelope>");

    std::string headers;
    headers.reserve(service_type.size() + action.size() + 128);
    headers.append("Content-Type: text/xml; charset=\"utf-8\"\r\n");
    headers.append("SOAPAction: \"");
    headers.append(service_type);
    headers.push_back('#');
    headers.append(action);
    headers.append("\"\r\n");

    auto response = http_request("POST", control_url, headers, body, timeout, ErrorCode::UpnpControlFailed);
    if (!response.has_value()) {
        return std::unexpected(response.error());
    }

    if (response->status >= 200 && response->status < 300) {
        return response->body;
    }

    const auto fault = extract_tag(response->body, "faultstring");
    return std::unexpected(Error{
        .code = ErrorCode::UpnpControlFailed,
        .message = fault.has_value()
            ? ("UPnP SOAP fault: " + *fault)
            : ("UPnP SOAP call failed with HTTP " + std::to_string(response->status))
    });
}

std::expected<std::string, Error> fallback_ipv4() {
    std::array<char, 256> host{};
    if (gethostname(host.data(), static_cast<int>(host.size())) != 0) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = "Unable to determine local hostname"
        });
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* resolved = nullptr;
    if (getaddrinfo(host.data(), nullptr, &hints, &resolved) != 0 || resolved == nullptr) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = "Unable to resolve local hostname to IPv4"
        });
    }

    char ip[INET_ADDRSTRLEN]{};
    const auto* ipv4 = reinterpret_cast<sockaddr_in*>(resolved->ai_addr);
    const char* text = inet_ntop(AF_INET, &ipv4->sin_addr, ip, static_cast<SockLen>(sizeof(ip)));
    freeaddrinfo(resolved);
    if (text == nullptr) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = "Unable to convert local IPv4 address"
        });
    }
    return std::string(text);
}

}  // namespace

std::expected<std::string, Error> detect_outbound_ipv4(std::string_view remote_host, std::uint16_t remote_port) {
    if (remote_host.empty()) {
        return fallback_ipv4();
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    std::array<char, 8> port_chars{};
    const auto [port_end, ec] = std::to_chars(
        port_chars.data(),
        port_chars.data() + port_chars.size(),
        remote_port);
    if (ec != std::errc{}) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = "Invalid remote port for local IP detection"
        });
    }
    *port_end = '\0';

    std::string host_copy(remote_host);
    addrinfo* resolved = nullptr;
    if (getaddrinfo(host_copy.c_str(), port_chars.data(), &hints, &resolved) != 0 || resolved == nullptr) {
        return fallback_ipv4();
    }

    SocketHandle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalidSocket) {
        freeaddrinfo(resolved);
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDiscoveryFailed,
            .message = socket_error_message(last_socket_error())
        });
    }

    if (::connect(handle, resolved->ai_addr, static_cast<SockLen>(resolved->ai_addrlen)) != 0) {
        close_socket(handle);
        freeaddrinfo(resolved);
        return fallback_ipv4();
    }

    sockaddr_in local{};
    SockLen local_len = static_cast<SockLen>(sizeof(local));
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&local), &local_len) != 0) {
        close_socket(handle);
        freeaddrinfo(resolved);
        return fallback_ipv4();
    }
    close_socket(handle);
    freeaddrinfo(resolved);

    char ip[INET_ADDRSTRLEN]{};
    const char* text = inet_ntop(AF_INET, &local.sin_addr, ip, static_cast<SockLen>(sizeof(ip)));
    if (text == nullptr) {
        return fallback_ipv4();
    }
    return std::string(text);
}

std::expected<UpnpMapping, Error> upnp_add_port_mapping(const UpnpRequest& request) {
    if (request.internal_port == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpControlFailed,
            .message = "UPnP requires a non-zero internal port"
        });
    }

    std::string protocol = lower_copy(request.protocol);
    std::transform(protocol.begin(), protocol.end(), protocol.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (protocol != "UDP" && protocol != "TCP") {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpControlFailed,
            .message = "UPnP protocol must be UDP or TCP"
        });
    }

    UpnpRequest effective = request;
    if (effective.discovery_timeout <= std::chrono::milliseconds::zero()) {
        effective.discovery_timeout = std::chrono::milliseconds(1);
    }
    if (effective.external_port == 0) {
        effective.external_port = effective.internal_port;
    }

    auto location = discover_upnp_location(effective);
    if (!location.has_value()) {
        return std::unexpected(location.error());
    }

    auto description = http_request(
        "GET",
        *location,
        {},
        {},
        effective.discovery_timeout,
        ErrorCode::UpnpDescriptionFailed);
    if (!description.has_value()) {
        return std::unexpected(description.error());
    }
    if (description->status < 200 || description->status >= 300) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDescriptionFailed,
            .message = "Failed to fetch UPnP device description"
        });
    }

    auto service = find_upnp_control_service(description->body);
    if (!service.has_value()) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDescriptionFailed,
            .message = "No WANIPConnection/WANPPPConnection service found"
        });
    }

    std::string service_type = service->first;
    std::string control_url = trim_copy(service->second);
    if (!control_url.starts_with("http://")) {
        std::string base;
        auto url_base = extract_tag(description->body, "URLBase");
        if (url_base.has_value() && url_base->starts_with("http://")) {
            base = *url_base;
        } else {
            const auto parsed_location = parse_http_url(*location);
            if (!parsed_location.has_value()) {
                return std::unexpected(Error{
                    .code = ErrorCode::UpnpDescriptionFailed,
                    .message = "Invalid UPnP location URL"
                });
            }
            base = format_origin(*parsed_location);
        }
        if (!control_url.empty() && control_url.front() == '/') {
            control_url = base + control_url;
        } else {
            if (!base.empty() && base.back() != '/') {
                base.push_back('/');
            }
            control_url = base + control_url;
        }
    }

    auto parsed_control = parse_http_url(control_url);
    if (!parsed_control.has_value()) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpDescriptionFailed,
            .message = "Invalid UPnP control URL"
        });
    }

    std::string internal_client = effective.internal_client;
    if (internal_client.empty()) {
        auto detected_ip = detect_outbound_ipv4(parsed_control->host, parsed_control->port);
        if (!detected_ip.has_value()) {
            return std::unexpected(detected_ip.error());
        }
        internal_client = *detected_ip;
    }

    std::string add_payload;
    add_payload.reserve(256 + effective.description.size() + internal_client.size());
    add_payload.append("<NewRemoteHost></NewRemoteHost>");
    add_payload.append("<NewExternalPort>");
    add_payload.append(std::to_string(effective.external_port));
    add_payload.append("</NewExternalPort>");
    add_payload.append("<NewProtocol>");
    add_payload.append(protocol);
    add_payload.append("</NewProtocol>");
    add_payload.append("<NewInternalPort>");
    add_payload.append(std::to_string(effective.internal_port));
    add_payload.append("</NewInternalPort>");
    add_payload.append("<NewInternalClient>");
    add_payload.append(xml_escape(internal_client));
    add_payload.append("</NewInternalClient>");
    add_payload.append("<NewEnabled>1</NewEnabled>");
    add_payload.append("<NewPortMappingDescription>");
    add_payload.append(xml_escape(effective.description));
    add_payload.append("</NewPortMappingDescription>");
    add_payload.append("<NewLeaseDuration>");
    add_payload.append(std::to_string(effective.lease_duration.count()));
    add_payload.append("</NewLeaseDuration>");

    auto add_result = call_soap(
        control_url,
        service_type,
        "AddPortMapping",
        add_payload,
        effective.discovery_timeout);
    if (!add_result.has_value()) {
        return std::unexpected(add_result.error());
    }

    std::string external_ip;
    auto external_result = call_soap(
        control_url,
        service_type,
        "GetExternalIPAddress",
        {},
        effective.discovery_timeout);
    if (external_result.has_value()) {
        auto maybe_ip = extract_tag(*external_result, "NewExternalIPAddress");
        if (maybe_ip.has_value()) {
            external_ip = *maybe_ip;
        }
    }

    UpnpMapping mapping{};
    mapping.service_type = std::move(service_type);
    mapping.control_url = std::move(control_url);
    mapping.external_ip = std::move(external_ip);
    mapping.internal_client = std::move(internal_client);
    mapping.external_port = effective.external_port;
    mapping.internal_port = effective.internal_port;
    mapping.protocol = std::move(protocol);
    return mapping;
}

std::expected<void, Error> upnp_remove_port_mapping(const UpnpMapping& mapping, std::chrono::milliseconds timeout) {
    if (mapping.control_url.empty() || mapping.service_type.empty() || mapping.protocol.empty() || mapping.external_port == 0) {
        return std::unexpected(Error{
            .code = ErrorCode::UpnpControlFailed,
            .message = "UPnP mapping record is incomplete"
        });
    }

    std::string payload;
    payload.reserve(128);
    payload.append("<NewRemoteHost></NewRemoteHost>");
    payload.append("<NewExternalPort>");
    payload.append(std::to_string(mapping.external_port));
    payload.append("</NewExternalPort>");
    payload.append("<NewProtocol>");
    payload.append(mapping.protocol);
    payload.append("</NewProtocol>");

    auto result = call_soap(
        mapping.control_url,
        mapping.service_type,
        "DeletePortMapping",
        payload,
        timeout <= std::chrono::milliseconds::zero() ? std::chrono::milliseconds(1) : timeout);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }
    return {};
}

}  // namespace unet::detail
