#include "unet/unet.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <optional>
#include <string>
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
    unet::DirectoryConfig config{};
    config.channel = 1;
    config.host.channel_count = 8;
    config.match_size = 2;
    config.base_mmr_tolerance = 100;
    config.mmr_tolerance_per_second = 10;

    unet::DirectoryServer directory(config);
    auto started = directory.start(38990, "127.0.0.1");
    assert(started.has_value());

    std::atomic<bool> running{true};
    std::jthread directory_thread([&]() {
        while (running.load()) {
            directory.service();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    auto directory_address = unet::Address::resolve("127.0.0.1", 38990, false);
    assert(directory_address.has_value());

    unet::DirectoryClient server_client(config);
    unet::DirectoryClient player_a(config);
    unet::DirectoryClient player_b(config);

    assert(server_client.start_node(38991, "127.0.0.1").has_value());
    assert(player_a.start_node(38992, "127.0.0.1").has_value());
    assert(player_b.start_node(38993, "127.0.0.1").has_value());

    assert(server_client.connect(*directory_address).has_value());
    assert(player_a.connect(*directory_address).has_value());
    assert(player_b.connect(*directory_address).has_value());

    // Server browser.
    unet::ServerRegistration registration{};
    registration.name = "Arena One";
    registration.map = "Ruins";
    registration.mode = "duel";
    registration.region = "eu-west";
    registration.max_players = 16;
    registration.current_players = 3;
    registration.connect_port = 38991;

    auto server_id = server_client.register_server(registration);
    assert(server_id.has_value());
    auto heartbeat = server_client.heartbeat_server(*server_id, 4, 16, 38991);
    assert(heartbeat.has_value());

    auto listed = player_a.list_servers("eu-west", "duel");
    assert(listed.has_value());
    assert(!listed->empty());
    assert((*listed)[0].server_id == *server_id);
    assert((*listed)[0].name == "Arena One");

    // Comments (including replies and likes).
    auto root_comment = player_a.post_comment(*server_id, "alice", "Great server");
    assert(root_comment.has_value());
    auto reply_comment = player_b.post_comment(*server_id, "bob", "Agreed", *root_comment);
    assert(reply_comment.has_value());
    assert(player_b.like_comment(*server_id, *root_comment, "bob-like").has_value());

    auto comments = player_a.list_comments(*server_id);
    assert(comments.has_value());
    assert(comments->size() == 2);
    bool saw_root = false;
    bool saw_reply = false;
    for (const auto& comment : *comments) {
        if (comment.comment_id == *root_comment) {
            saw_root = true;
            assert(comment.likes == 1);
        }
        if (comment.comment_id == *reply_comment) {
            saw_reply = true;
            assert(comment.parent_id == *root_comment);
        }
    }
    assert(saw_root && saw_reply);

    // Matchmaking from player data.
    unet::MatchmakingProfile ticket_a{};
    ticket_a.player_id = "alice";
    ticket_a.region = "eu-west";
    ticket_a.playlist = "ranked-duel";
    ticket_a.mmr = 1200;
    ticket_a.party_size = 1;
    auto ticket_a_id = player_a.enqueue_matchmaking(ticket_a);
    assert(ticket_a_id.has_value());

    unet::MatchmakingProfile ticket_b{};
    ticket_b.player_id = "bob";
    ticket_b.region = "eu-west";
    ticket_b.playlist = "ranked-duel";
    ticket_b.mmr = 1250;
    ticket_b.party_size = 1;
    auto ticket_b_id = player_b.enqueue_matchmaking(ticket_b);
    assert(ticket_b_id.has_value());

    std::optional<unet::MatchFoundEvent> match_a;
    std::optional<unet::MatchFoundEvent> match_b;
    const bool matched = wait_until(std::chrono::milliseconds(4000), [&]() {
        player_a.service();
        player_b.service();
        if (!match_a.has_value()) {
            match_a = player_a.poll_match_found();
        }
        if (!match_b.has_value()) {
            match_b = player_b.poll_match_found();
        }
        return match_a.has_value() && match_b.has_value();
    });
    assert(matched);
    assert(match_a->match_id == match_b->match_id);

    // NAT traversal coordination + direct peer connect.
    assert(player_a.register_nat_identity("alice-id").has_value());
    assert(player_b.register_nat_identity("bob-id").has_value());

    auto bob_address = player_a.request_nat_punch("bob-id");
    assert(bob_address.has_value());
    auto alice_to_bob = player_a.transport().connect(*bob_address);
    assert(alice_to_bob.has_value());

    std::optional<unet::NatPunchEvent> bob_punch;
    const bool got_bob_punch = wait_until(std::chrono::milliseconds(2000), [&]() {
        player_a.service();
        player_b.service();
        if (!bob_punch.has_value()) {
            bob_punch = player_b.poll_nat_punch();
        }
        return bob_punch.has_value();
    });
    assert(got_bob_punch);

    auto bob_to_alice = player_b.transport().connect(bob_punch->requester_address);
    assert(bob_to_alice.has_value());

    bool a_connected_to_b = false;
    bool b_connected_to_a = false;
    unet::PeerId b_seen_peer = unet::invalid_peer_id;
    const bool p2p_connected = wait_until(std::chrono::milliseconds(4000), [&]() {
        player_a.service();
        player_b.service();

        while (auto event = player_a.poll_network_event()) {
            if (event->type == unet::Event::Type::Connect && event->connect.peer == *alice_to_bob) {
                a_connected_to_b = true;
            }
        }

        while (auto event = player_b.poll_network_event()) {
            if (event->type == unet::Event::Type::Connect) {
                b_connected_to_a = true;
                b_seen_peer = event->connect.peer;
            }
        }

        return a_connected_to_b && b_connected_to_a;
    });
    assert(p2p_connected);
    assert(b_seen_peer != unet::invalid_peer_id);

    // Verify direct post-punch transport path.
    const auto ping_payload = unet::to_bytes("p2p-check");
    assert(player_a.transport().send(*alice_to_bob, ping_payload).has_value());
    bool p2p_message_ok = false;
    const bool got_message = wait_until(std::chrono::milliseconds(3000), [&]() {
        player_a.service();
        player_b.service();

        while (auto event = player_b.poll_network_event()) {
            if (event->type == unet::Event::Type::Message) {
                const std::string text = unet::to_string(event->message.payload);
                if (text == "p2p-check") {
                    p2p_message_ok = true;
                    return true;
                }
            }
        }
        return p2p_message_ok;
    });
    assert(got_message);

    running.store(false);
    return 0;
}
