#include "unet/unet.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    const std::uint16_t port = (argc > 1) ? static_cast<std::uint16_t>(std::stoi(argv[1])) : 7780;

    unet::HostConfig config{};
    config.enable_upnp = true;
    config.require_upnp = false;
    config.upnp_description = "unet upnp example";

    unet::Host server(config);
    if (auto started = server.start_server(port, "0.0.0.0"); !started.has_value()) {
        std::cerr << "start_server failed: " << started.error().message << '\n';
        return 1;
    }

    if (auto mapped = server.upnp_external_address(); mapped.has_value()) {
        std::cout << "UPnP mapped external endpoint: " << mapped->to_string() << '\n';
    } else if (auto upnp_error = server.upnp_last_error(); upnp_error.has_value()) {
        std::cout << "UPnP mapping unavailable: " << upnp_error->message << '\n';
    } else {
        std::cout << "UPnP not configured.\n";
    }

    std::cout << "Server listening on UDP " << port << '\n';

    for (;;) {
        server.service();

        while (auto event = server.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                std::cout << "Peer connected: " << event->connect.address.to_string() << '\n';
            } else if (event->type == unet::Event::Type::Disconnect) {
                std::cout << "Peer disconnected: " << event->disconnect.peer << '\n';
            } else if (event->type == unet::Event::Type::Message) {
                std::cout << "Message: " << unet::to_string(event->message.payload) << '\n';
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

