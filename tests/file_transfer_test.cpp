#include "unet/unet.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

std::vector<std::byte> read_all_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return bytes;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;

    const fs::path base = fs::temp_directory_path() / "unet_file_transfer_test";
    const fs::path source = base / "source.bin";
    const fs::path downloads = base / "downloads";
    std::error_code ec{};
    fs::remove_all(base, ec);
    fs::create_directories(downloads, ec);

    {
        std::ofstream src(source, std::ios::binary | std::ios::trunc);
        assert(src.is_open());
        for (int i = 0; i < 4000; ++i) {
            src << "line:" << i << " lorem ipsum dolor sit amet\n";
        }
    }

    unet::HostConfig config{};
    config.channel_count = 8;
    config.file_transfer_channel = 7;
    config.file_chunk_size = 4096;
    config.auto_accept_file_transfers = true;
    config.download_directory = downloads.string();

    unet::Host peer_a(config);
    unet::Host peer_b(config);

    auto a_listen = peer_a.start_p2p(35791, "127.0.0.1");
    auto b_listen = peer_b.start_p2p(35792, "127.0.0.1");
    assert(a_listen.has_value());
    assert(b_listen.has_value());

    auto b_addr = unet::Address::resolve("127.0.0.1", 35792, false);
    assert(b_addr.has_value());
    auto a_to_b = peer_a.connect(*b_addr);
    assert(a_to_b.has_value());

    unet::PeerId a_peer = *a_to_b;
    unet::PeerId b_peer = unet::invalid_peer_id;
    bool a_connected = false;
    bool b_connected = false;

    const bool connected = pump_until(std::chrono::milliseconds(3000), [&]() {
        peer_a.service();
        peer_b.service();

        while (auto event = peer_a.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                a_connected = true;
            }
        }
        while (auto event = peer_b.poll_event()) {
            if (event->type == unet::Event::Type::Connect) {
                b_connected = true;
                b_peer = event->connect.peer;
            }
        }
        return a_connected && b_connected;
    });
    assert(connected);
    assert(b_peer != unet::invalid_peer_id);

    auto transfer = peer_a.send_file(a_peer, source);
    assert(transfer.has_value());

    bool upload_done = false;
    bool download_done = false;
    fs::path downloaded_file{};

    const bool transferred = pump_until(std::chrono::milliseconds(8000), [&]() {
        peer_a.service();
        peer_b.service();

        while (auto event = peer_a.poll_event()) {
            if (event->type == unet::Event::Type::FileRejected) {
                return false;
            }
            if (event->type == unet::Event::Type::FileComplete &&
                event->file_complete.upload &&
                event->file_complete.transfer_id == *transfer) {
                upload_done = true;
            }
        }
        while (auto event = peer_b.poll_event()) {
            if (event->type == unet::Event::Type::FileRejected) {
                return false;
            }
            if (event->type == unet::Event::Type::FileComplete &&
                !event->file_complete.upload &&
                event->file_complete.transfer_id == *transfer) {
                download_done = true;
                downloaded_file = event->file_complete.path;
            }
        }
        return upload_done && download_done;
    });
    assert(transferred);

    const auto src_bytes = read_all_bytes(source);
    const auto dst_bytes = read_all_bytes(downloaded_file);
    assert(!src_bytes.empty());
    assert(src_bytes == dst_bytes);

    fs::remove_all(base, ec);
    return 0;
}

