#include "unet/unet.hpp"
#include "quic_tls_test_util.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace {

bool wait_until(std::chrono::milliseconds timeout, auto&& fn) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

int main() {
    const char* run_native_quic = std::getenv("UNET_ENABLE_NATIVE_QUIC_TESTS");
    if (run_native_quic == nullptr || std::string_view(run_native_quic) != "1") {
        std::cerr << "http3_test skipped (set UNET_ENABLE_NATIVE_QUIC_TESTS=1 to run)\n";
        return 0;
    }

    constexpr std::uint16_t kPort = 34570;

    unet::Http3Config server_config{};
    server_config.host.transport = unet::Transport::Quic;
    server_config.host.max_peers = 8;
    server_config.host.channel_count = 4;
    server_config.stream_channel = 2;
    if (!unet::test::configure_quic_tls(server_config.host, "unet-http3-test")) {
        std::cerr << "http3_test skipped (TLS test certificate unavailable)\n";
        return 0;
    }

    unet::Http3Server server(server_config);
    server.set_handler([](const unet::Http3Request& request) -> unet::Http3Response {
        unet::Http3Response response{};
        if (request.path == "/echo") {
            response.status = 201;
            response.headers.push_back({"content-type", "text/plain"});
            response.headers.push_back({"x-method", request.method});
            response.body = request.body;
            return response;
        }

        response.status = 404;
        response.body = unet::to_bytes("not found");
        return response;
    });

    auto started = server.start(kPort, "127.0.0.1");
    if (!started.has_value() && started.error().code == unet::ErrorCode::InvalidState) {
        std::cerr << "http3_test skipped (" << started.error().message << ")\n";
        return 0;
    }
    if (!started.has_value()) {
        std::cerr << "http3_test start failed: " << started.error().message << "\n";
    }
    assert(started.has_value());

    std::atomic<bool> running{true};
    std::jthread server_thread([&](std::stop_token token) {
        while (running.load() && !token.stop_requested()) {
            server.service();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    unet::Http3Config client_config = server_config;
    client_config.host.quic_pkcs12_file.clear();
    client_config.host.quic_pkcs12_password.clear();
    unet::Http3Client client(client_config);

    auto address = unet::Address::resolve("127.0.0.1", kPort, false);
    assert(address.has_value());
    assert(client.connect(*address).has_value());

    const bool connected = wait_until(std::chrono::milliseconds(3000), [&]() {
        client.service();
        return client.is_connected();
    });
    assert(connected);

    unet::Http3Request request{};
    request.method = "POST";
    request.authority = "localhost";
    request.path = "/echo";
    request.headers.push_back({"content-type", "text/plain"});
    request.body = unet::to_bytes("hello over quic stream");

    auto response = client.request(request, std::chrono::milliseconds(5000));
    assert(response.has_value());
    assert(response->status == 201);
    assert(unet::to_string(response->body) == "hello over quic stream");

    unet::Http3Request missing{};
    missing.method = "GET";
    missing.authority = "localhost";
    missing.path = "/missing";
    auto missing_response = client.request(missing, std::chrono::milliseconds(5000));
    assert(missing_response.has_value());
    assert(missing_response->status == 404);
    assert(unet::to_string(missing_response->body) == "not found");

    running.store(false);
    server_thread.request_stop();
    std::cerr << "http3_test passed\n";
    return 0;
}
