#include "unet/unet.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
using SockLen = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
using SockLen = socklen_t;
constexpr SocketHandle kInvalidSocket = -1;
#endif

struct SocketRuntime {
    bool ok{true};

    SocketRuntime() {
#if defined(_WIN32)
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }

    ~SocketRuntime() {
#if defined(_WIN32)
        if (ok) {
            WSACleanup();
        }
#endif
    }
};

void close_socket(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

bool wait_readable(SocketHandle socket, std::chrono::milliseconds timeout) {
    fd_set reads{};
    FD_ZERO(&reads);
    FD_SET(socket, &reads);

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return select(static_cast<int>(socket + 1), &reads, nullptr, nullptr, &tv) > 0;
}

bool wait_writable(SocketHandle socket, std::chrono::milliseconds timeout) {
    fd_set writes{};
    FD_ZERO(&writes);
    FD_SET(socket, &writes);

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return select(static_cast<int>(socket + 1), nullptr, &writes, nullptr, &tv) > 0;
}

void set_non_blocking(SocketHandle socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    (void)ioctlsocket(socket, FIONBIO, &enabled);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(socket, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

std::uint16_t allocate_loopback_port(int sock_type, int protocol) {
    SocketHandle socket = ::socket(AF_INET, sock_type, protocol);
    assert(socket != kInvalidSocket);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(::bind(socket, reinterpret_cast<const sockaddr*>(&addr), static_cast<SockLen>(sizeof(addr))) == 0);

    SockLen len = static_cast<SockLen>(sizeof(addr));
    assert(::getsockname(socket, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const std::uint16_t port = ntohs(addr.sin_port);
    close_socket(socket);
    return port;
}

std::string read_http_request(SocketHandle client) {
    std::string request;
    std::array<char, 4096> buffer{};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_readable(client, std::chrono::milliseconds(50))) {
            continue;
        }

        const int received = ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            break;
        }
        request.append(buffer.data(), static_cast<std::size_t>(received));

        if (header_end == std::string::npos) {
            header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::string headers = request.substr(0, header_end);
                const std::size_t cl_pos = headers.find("Content-Length:");
                if (cl_pos != std::string::npos) {
                    const std::size_t line_end = headers.find("\r\n", cl_pos);
                    const std::string value = headers.substr(cl_pos + 15, line_end - (cl_pos + 15));
                    content_length = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
                }
            }
        }

        if (header_end != std::string::npos) {
            const std::size_t body_start = header_end + 4;
            if (request.size() >= body_start + content_length) {
                break;
            }
        }
    }

    return request;
}

void send_all(SocketHandle socket, std::string_view payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        if (!wait_writable(socket, std::chrono::milliseconds(1000))) {
            return;
        }
        const int sent = ::send(
            socket,
            payload.data() + static_cast<std::ptrdiff_t>(offset),
            static_cast<int>(payload.size() - offset),
            0);
        if (sent <= 0) {
            return;
        }
        offset += static_cast<std::size_t>(sent);
    }
}

std::string make_http_response(std::string_view body, int status = 200, std::string_view reason = "OK") {
    std::string response;
    response.reserve(body.size() + 128);
    response.append("HTTP/1.1 ");
    response.append(std::to_string(status));
    response.push_back(' ');
    response.append(reason);
    response.append("\r\nContent-Type: text/xml\r\nContent-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nConnection: close\r\n\r\n");
    response.append(body);
    return response;
}

bool wait_until(std::chrono::milliseconds timeout, auto&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

int main() {
    SocketRuntime runtime{};
    assert(runtime.ok);

    const std::uint16_t ssdp_port = allocate_loopback_port(SOCK_DGRAM, IPPROTO_UDP);
    const std::uint16_t http_port = allocate_loopback_port(SOCK_STREAM, IPPROTO_TCP);
    const std::uint16_t host_port = allocate_loopback_port(SOCK_DGRAM, IPPROTO_UDP);

    std::atomic<bool> running{true};
    std::atomic<int> add_calls{0};
    std::atomic<int> delete_calls{0};
    std::atomic<bool> saw_udp_protocol{false};
    std::atomic<bool> ssdp_ready{false};
    std::atomic<bool> http_ready{false};

    std::jthread ssdp_thread([&]() {
        SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(socket != kInvalidSocket);

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = htons(ssdp_port);
        assert(::bind(socket, reinterpret_cast<const sockaddr*>(&bind_addr), static_cast<SockLen>(sizeof(bind_addr))) == 0);
        set_non_blocking(socket);
        ssdp_ready.store(true);

        std::array<char, 2048> buffer{};
        while (running.load()) {
            if (!wait_readable(socket, std::chrono::milliseconds(50))) {
                continue;
            }

            sockaddr_in from{};
            SockLen from_len = static_cast<SockLen>(sizeof(from));
            const int received = ::recvfrom(
                socket,
                buffer.data(),
                static_cast<int>(buffer.size()),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &from_len);
            if (received <= 0) {
                continue;
            }

            std::string_view request(buffer.data(), static_cast<std::size_t>(received));
            if (!request.contains("M-SEARCH")) {
                continue;
            }

            std::string response;
            response.reserve(512);
            response.append("HTTP/1.1 200 OK\r\n");
            response.append("CACHE-CONTROL: max-age=120\r\n");
            response.append("EXT:\r\n");
            response.append("LOCATION: http://127.0.0.1:");
            response.append(std::to_string(http_port));
            response.append("/desc.xml\r\n");
            response.append("SERVER: unittest/1.0 UPnP/1.0 unittest/1.0\r\n");
            response.append("ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n");
            response.append("USN: uuid:test-igd::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n");
            response.append("\r\n");

            (void)::sendto(
                socket,
                response.data(),
                static_cast<int>(response.size()),
                0,
                reinterpret_cast<const sockaddr*>(&from),
                from_len);
        }

        close_socket(socket);
    });

    std::jthread http_thread([&]() {
        SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listener != kInvalidSocket);

        const int one = 1;
        (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), static_cast<SockLen>(sizeof(one)));

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = htons(http_port);
        assert(::bind(listener, reinterpret_cast<const sockaddr*>(&bind_addr), static_cast<SockLen>(sizeof(bind_addr))) == 0);
        assert(::listen(listener, 8) == 0);
        set_non_blocking(listener);
        http_ready.store(true);

        const std::string description = R"(<?xml version="1.0"?>
<root>
  <device>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
        <controlURL>/control</controlURL>
      </service>
    </serviceList>
  </device>
</root>)";

        while (running.load()) {
            if (!wait_readable(listener, std::chrono::milliseconds(50))) {
                continue;
            }

            sockaddr_in from{};
            SockLen from_len = static_cast<SockLen>(sizeof(from));
            SocketHandle client = ::accept(listener, reinterpret_cast<sockaddr*>(&from), &from_len);
            if (client == kInvalidSocket) {
                continue;
            }

            const std::string request = read_http_request(client);
            if (request.starts_with("GET /desc.xml ")) {
                const std::string response = make_http_response(description);
                send_all(client, response);
                close_socket(client);
                continue;
            }

            if (request.starts_with("POST /control ")) {
                if (request.contains("#AddPortMapping")) {
                    add_calls.fetch_add(1);
                    if (request.contains("<NewProtocol>UDP</NewProtocol>")) {
                        saw_udp_protocol.store(true);
                    }
                    const std::string body =
                        R"(<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:AddPortMappingResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1"/></s:Body></s:Envelope>)";
                    send_all(client, make_http_response(body));
                    close_socket(client);
                    continue;
                }
                if (request.contains("#GetExternalIPAddress")) {
                    const std::string body =
                        R"(<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetExternalIPAddressResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1"><NewExternalIPAddress>198.51.100.7</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>)";
                    send_all(client, make_http_response(body));
                    close_socket(client);
                    continue;
                }
                if (request.contains("#DeletePortMapping")) {
                    delete_calls.fetch_add(1);
                    const std::string body =
                        R"(<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:DeletePortMappingResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1"/></s:Body></s:Envelope>)";
                    send_all(client, make_http_response(body));
                    close_socket(client);
                    continue;
                }
            }

            send_all(client, make_http_response("bad request", 400, "Bad Request"));
            close_socket(client);
        }

        close_socket(listener);
    });

    const bool ready = wait_until(std::chrono::milliseconds(2000), [&]() {
        return ssdp_ready.load() && http_ready.load();
    });
    assert(ready);

    {
        unet::HostConfig config{};
        config.enable_upnp = true;
        config.require_upnp = true;
        config.upnp_discovery_address = "127.0.0.1";
        config.upnp_discovery_port = ssdp_port;
        config.upnp_discovery_timeout = std::chrono::milliseconds(1200);
        config.upnp_description = "unet upnp test";

        unet::Host host(config);
        auto started = host.start_server(host_port, "127.0.0.1");
        assert(started.has_value());

        const auto external = host.upnp_external_address();
        assert(external.has_value());
        assert(external->ip() == "198.51.100.7");
        assert(external->port() == host_port);
        assert(!host.upnp_last_error().has_value());
    }

    const bool deleted = wait_until(std::chrono::milliseconds(3000), [&]() {
        return delete_calls.load() > 0;
    });
    assert(deleted);
    assert(add_calls.load() > 0);
    assert(saw_udp_protocol.load());

    running.store(false);

    // Wake the SSDP loop.
    {
        SocketHandle udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp != kInvalidSocket) {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            dest.sin_port = htons(ssdp_port);
            const char ping[] = "stop";
            (void)::sendto(udp, ping, static_cast<int>(sizeof(ping)), 0, reinterpret_cast<const sockaddr*>(&dest), static_cast<SockLen>(sizeof(dest)));
            close_socket(udp);
        }
    }

    // Wake the HTTP loop.
    {
        SocketHandle tcp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcp != kInvalidSocket) {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            dest.sin_port = htons(http_port);
            (void)::connect(tcp, reinterpret_cast<const sockaddr*>(&dest), static_cast<SockLen>(sizeof(dest)));
            close_socket(tcp);
        }
    }

    return 0;
}
