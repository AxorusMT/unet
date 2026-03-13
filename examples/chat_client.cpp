#include "unet/unet.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: unetcode_chat_client <host> <port>\n";
        return 1;
    }

    const std::string host = argv[1];
    const std::uint16_t port = static_cast<std::uint16_t>(std::stoi(argv[2]));

    auto address = unet::Address::resolve(host, port);
    if (!address.has_value()) {
        std::cerr << "Address resolve failed: " << address.error().message << '\n';
        return 1;
    }

    unet::Host client;
    auto peer_result = client.connect(*address);
    if (!peer_result.has_value()) {
        std::cerr << "connect failed: " << peer_result.error().message << '\n';
        return 1;
    }

    const unet::PeerId peer = *peer_result;
    std::cout << "Connecting to " << address->to_string() << " (peer " << peer << ")\n";
    std::cout << "Type messages, /quit to exit.\n";

    std::atomic<bool> running{true};
    std::atomic<bool> connected{false};
    std::mutex outgoing_mutex;
    std::deque<std::string> outgoing;

    std::jthread input_thread([&]() {
        std::string line;
        while (running.load()) {
            if (!std::getline(std::cin, line)) {
                running.store(false);
                break;
            }
            if (line == "/quit") {
                running.store(false);
                break;
            }
            std::scoped_lock lock(outgoing_mutex);
            outgoing.push_back(line);
        }
    });

    while (running.load()) {
        client.service();

        while (auto event = client.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                connected.store(true);
                std::cout << "Connected.\n";
                continue;
            }
            if (event->type == unet::Event::Type::Disconnect) {
                std::cout << "Disconnected.\n";
                running.store(false);
                continue;
            }
            std::cout << unet::to_string(event->message.payload) << '\n';
        }

        if (connected.load()) {
            std::deque<std::string> send_queue;
            {
                std::scoped_lock lock(outgoing_mutex);
                send_queue.swap(outgoing);
            }

            for (const std::string& line : send_queue) {
                const auto payload = unet::to_bytes(line);
                unet::SendOptions options{};
                options.channel = 0;
                options.delivery = unet::Delivery::ReliableOrdered;
                (void)client.send(peer, payload, options);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    (void)client.disconnect(peer, unet::DisconnectReason::Requested);
    return 0;
}

