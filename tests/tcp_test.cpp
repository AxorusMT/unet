#include "unet/unet.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
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
    constexpr std::uint16_t kPort = 34568;

    unet::HostConfig config{};
    config.transport = unet::Transport::Tcp;
    config.max_peers = 8;
    config.channel_count = 4;
    config.mtu = 1200;
    config.disconnect_timeout = std::chrono::milliseconds(4000);

    unet::Host server(config);
    auto started = server.start_server(kPort, "127.0.0.1");
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

    const auto ping_payload = unet::to_bytes("tcp-ping");
    unet::SendOptions options{};
    options.channel = 0;
    options.delivery = unet::Delivery::Unreliable;
    assert(client.send(client_peer, ping_payload, options).has_value());

    bool server_received = false;
    bool client_received = false;
    const bool message_roundtrip = pump_until(std::chrono::milliseconds(3000), [&]() {
        server.service();
        client.service();

        while (auto event = server.poll_event()) {
            if (event->type == unet::Event::Type::Message) {
                if (unet::to_string(event->message.payload) == "tcp-ping") {
                    server_received = true;
                    const auto pong = unet::to_bytes("tcp-pong");
                    unet::SendOptions reply{};
                    reply.channel = 0;
                    reply.delivery = unet::Delivery::ReliableOrdered;
                    (void)server.send(server_peer, pong, reply);
                }
            }
        }
        while (auto event = client.poll_event()) {
            if (event->type == unet::Event::Type::Message) {
                if (unet::to_string(event->message.payload) == "tcp-pong") {
                    client_received = true;
                }
            }
        }
        return server_received && client_received;
    });
    assert(message_roundtrip);

    return 0;
}

