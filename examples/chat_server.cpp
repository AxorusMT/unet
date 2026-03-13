#include "unet/unet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    const std::uint16_t port = (argc > 1) ? static_cast<std::uint16_t>(std::stoi(argv[1])) : 7777;

    unet::Host server;
    if (auto started = server.start_server(port); !started.has_value()) {
        std::cerr << "start_server failed: " << started.error().message << '\n';
        return 1;
    }

    std::cout << "Server listening on port " << port << '\n';

    while (true) {
        server.service();

        while (auto event = server.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                std::cout << "Peer connected: id=" << event->connect.peer
                          << " addr=" << event->connect.address.to_string() << '\n';
                continue;
            }

            if (event->type == unet::Event::Type::Disconnect) {
                std::cout << "Peer disconnected: id=" << event->disconnect.peer << '\n';
                continue;
            }

            const std::string text = unet::to_string(event->message.payload);
            std::cout << '[' << event->message.peer << "] " << text << '\n';

            for (unet::PeerId peer : server.connected_peers()) {
                if (peer == event->message.peer) {
                    continue;
                }
                unet::SendOptions options{};
                options.channel = event->message.channel;
                options.delivery = unet::Delivery::ReliableOrdered;
                (void)server.send(peer, event->message.payload, options);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

