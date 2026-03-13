#include "unet/unet.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  unetcode_p2p_file_node <listen_port>\n"
                  << "  unetcode_p2p_file_node <listen_port> <connect_host> <connect_port> [send_file_path]\n";
        return 1;
    }

    const std::uint16_t listen_port = static_cast<std::uint16_t>(std::stoi(argv[1]));
    std::optional<std::string> connect_host;
    std::optional<std::uint16_t> connect_port;
    std::optional<std::filesystem::path> file_to_send;
    if (argc >= 4) {
        connect_host = argv[2];
        connect_port = static_cast<std::uint16_t>(std::stoi(argv[3]));
    }
    if (argc >= 5) {
        file_to_send = std::filesystem::path(argv[4]);
    }

    unet::HostConfig config{};
    config.file_transfer_channel = 7;
    config.download_directory = "downloads";

    unet::Host host(config);
    if (auto started = host.start_p2p(listen_port, "0.0.0.0"); !started.has_value()) {
        std::cerr << "start_p2p failed: " << started.error().message << '\n';
        return 1;
    }
    std::cout << "P2P node listening on UDP " << listen_port << '\n';

    std::optional<unet::PeerId> connected_peer;
    bool file_sent = false;

    if (connect_host.has_value() && connect_port.has_value()) {
        auto address = unet::Address::resolve(*connect_host, *connect_port, false);
        if (!address.has_value()) {
            std::cerr << "resolve failed: " << address.error().message << '\n';
            return 1;
        }
        auto connection = host.connect(*address);
        if (!connection.has_value()) {
            std::cerr << "connect failed: " << connection.error().message << '\n';
            return 1;
        }
        connected_peer = *connection;
        std::cout << "Dialing " << address->to_string() << '\n';
    }

    for (;;) {
        host.service();

        while (auto event = host.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                connected_peer = event->connect.peer;
                std::cout << "Connected peer " << event->connect.peer
                          << " @ " << event->connect.address.to_string() << '\n';
            } else if (event->type == unet::Event::Type::Disconnect) {
                std::cout << "Peer disconnected " << event->disconnect.peer << '\n';
            } else if (event->type == unet::Event::Type::FileOffer) {
                std::cout << "Incoming file offer id=" << event->file_offer.transfer_id
                          << " name=" << event->file_offer.filename
                          << " bytes=" << event->file_offer.total_bytes << '\n';
            } else if (event->type == unet::Event::Type::FileProgress) {
                std::cout << (event->file_progress.upload ? "Upload" : "Download")
                          << " id=" << event->file_progress.transfer_id
                          << " " << event->file_progress.bytes_transferred
                          << "/" << event->file_progress.total_bytes << '\n';
            } else if (event->type == unet::Event::Type::FileComplete) {
                std::cout << "File complete id=" << event->file_complete.transfer_id
                          << " path=" << event->file_complete.path << '\n';
            } else if (event->type == unet::Event::Type::FileRejected) {
                std::cout << "File rejected id=" << event->file_rejected.transfer_id
                          << " reason=" << event->file_rejected.reason << '\n';
            } else if (event->type == unet::Event::Type::Message) {
                std::cout << "msg: " << unet::to_string(event->message.payload) << '\n';
            }
        }

        if (!file_sent && connected_peer.has_value() && file_to_send.has_value()) {
            auto transfer = host.send_file(*connected_peer, *file_to_send);
            if (transfer.has_value()) {
                std::cout << "Started upload id=" << *transfer << " file=" << file_to_send->string() << '\n';
                file_sent = true;
            } else {
                std::cout << "send_file failed: " << transfer.error().message << '\n';
                file_sent = true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

