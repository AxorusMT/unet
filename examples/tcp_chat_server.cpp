#include "unet/unet.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    const std::uint16_t port = (argc > 1) ? static_cast<std::uint16_t>(std::stoi(argv[1])) : 7778;

    unet::HostConfig config{};
    config.transport = unet::Transport::Tcp;

    unet::Host server(config);
    if (auto started = server.start_server(port, "0.0.0.0"); !started.has_value()) {
        std::cerr << "start_server failed: " << started.error().message << '\n';
        return 1;
    }

    std::cout << "TCP chat server listening on " << port << '\n';

    for (;;) {
        server.service();

        while (auto event = server.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                std::cout << "Connected: " << event->connect.peer << " " << event->connect.address.to_string() << '\n';
                continue;
            }
            if (event->type == unet::Event::Type::Disconnect) {
                std::cout << "Disconnected: " << event->disconnect.peer << '\n';
                continue;
            }
            if (event->type != unet::Event::Type::Message) {
                continue;
            }

            std::cout << '[' << event->message.peer << "] " << unet::to_string(event->message.payload) << '\n';

            for (const unet::PeerId peer : server.connected_peers()) {
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

