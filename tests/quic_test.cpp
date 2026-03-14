#include "unet/unet.hpp"
#include "quic_tls_test_util.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

bool pump_until(std::chrono::milliseconds timeout, auto&& fn) {
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
        std::cerr << "quic_test skipped (set UNET_ENABLE_NATIVE_QUIC_TESTS=1 to run)\n";
        return 0;
    }

    constexpr std::uint16_t kPort = 34569;

    unet::HostConfig config{};
    config.transport = unet::Transport::Quic;
    config.max_peers = 8;
    config.channel_count = 4;
    config.mtu = 1200;
    config.disconnect_timeout = std::chrono::milliseconds(4000);
    if (!unet::test::configure_quic_tls(config, "unet-quic-smoke")) {
        std::cerr << "quic_test skipped (TLS test certificate unavailable)\n";
        return 0;
    }

    unet::Host server(config);
    auto started = server.start_server(kPort, "127.0.0.1");
    if (!started.has_value() && started.error().code == unet::ErrorCode::InvalidState) {
        std::cerr << "quic_test skipped (" << started.error().message << ")\n";
        return 0;
    }
    assert(started.has_value());

    auto address = unet::Address::resolve("127.0.0.1", kPort, false);
    assert(address.has_value());

    unet::Host client(config);
    auto connect_result = client.connect(*address);
    assert(connect_result.has_value());
    const unet::PeerId client_peer = *connect_result;

    bool client_connected = false;
    bool server_connected = false;
    unet::PeerId server_peer = unet::invalid_peer_id;

    const bool connected = pump_until(std::chrono::milliseconds(3000), [&]() {
        server.service();
        client.service();

        while (auto event = server.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                server_connected = true;
                server_peer = event->connect.peer;
            }
        }
        while (auto event = client.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                client_connected = true;
            }
        }
        return client_connected && server_connected;
    });
    assert(connected);
    assert(server_peer != unet::invalid_peer_id);

    const auto ping_payload = unet::to_bytes("quic-ping");
    unet::SendOptions options{};
    options.channel = 0;
    options.delivery = unet::Delivery::ReliableOrdered;
    assert(client.send(client_peer, ping_payload, options).has_value());

    bool server_received = false;
    bool client_received = false;
    const bool message_roundtrip = pump_until(std::chrono::milliseconds(3000), [&]() {
        server.service();
        client.service();

        while (auto event = server.poll_event()) {
            if (event->type == unet::Event::Type::Message) {
                if (unet::to_string(event->message.payload) == "quic-ping") {
                    server_received = true;
                    const auto pong = unet::to_bytes("quic-pong");
                    unet::SendOptions reply{};
                    reply.channel = 0;
                    reply.delivery = unet::Delivery::UnreliableSequenced;
                    (void)server.send(server_peer, pong, reply);
                }
            }
        }
        while (auto event = client.poll_event()) {
            if (event->type == unet::Event::Type::Message) {
                if (unet::to_string(event->message.payload) == "quic-pong") {
                    client_received = true;
                }
            }
        }
        return server_received && client_received;
    });
    assert(message_roundtrip);

    unet::StreamOpenOptions stream_options{};
    stream_options.channel = 1;
    stream_options.bidirectional = true;
    auto client_stream = client.open_stream(client_peer, stream_options);
    assert(client_stream.has_value());

    unet::StreamSendOptions stream_send{};
    stream_send.fin = true;
    assert(client.send_stream(client_peer, *client_stream, unet::to_bytes("stream-ping"), stream_send).has_value());

    bool server_stream_opened = false;
    bool server_stream_received = false;
    bool client_stream_received = false;
    unet::StreamId server_stream_id = unet::invalid_stream_id;

    const bool stream_roundtrip = pump_until(std::chrono::milliseconds(4000), [&]() {
        server.service();
        client.service();

        while (auto event = server.poll_event()) {
            if (event->type == unet::Event::Type::StreamOpen &&
                event->stream_open.channel == 1) {
                server_stream_opened = true;
                server_stream_id = event->stream_open.stream;
            }
            if (event->type == unet::Event::Type::StreamData &&
                event->stream_data.fin &&
                unet::to_string(event->stream_data.payload) == "stream-ping") {
                server_stream_received = true;
                assert(server_stream_id != unet::invalid_stream_id);
                unet::StreamSendOptions reply_options{};
                reply_options.fin = true;
                assert(server.send_stream(server_peer, server_stream_id, unet::to_bytes("stream-pong"), reply_options).has_value());
            }
        }

        while (auto event = client.poll_event()) {
            if (event->type == unet::Event::Type::StreamData &&
                event->stream_data.stream == *client_stream &&
                event->stream_data.fin &&
                unet::to_string(event->stream_data.payload) == "stream-pong") {
                client_stream_received = true;
            }
        }

        return server_stream_opened && server_stream_received && client_stream_received;
    });
    assert(stream_roundtrip);

    std::cerr << "quic_test passed\n";
    return 0;
}
