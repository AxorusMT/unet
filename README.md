# unetcode

`unetcode` is a modern C++23 UDP game networking library inspired by ENet.

It provides:

- Connection-oriented UDP host/peer model
- P2P node mode (listen + dial on the same host)
- Reliable ordered delivery over UDP with retransmission
- Unreliable and unreliable-sequenced delivery modes
- Channel-based streams
- Automatic MTU-aware fragmentation + reassembly
- Packet-level ack bitfield (`ack` + `ack_bits`) with RTT estimation
- Handshake (`ConnectRequest` / `ConnectAccept`)
- Heartbeats (`Ping` / `Pong`) and timeout disconnects
- Event-driven API (`Connect`, `Disconnect`, `Message`)
- Built-in file upload/download protocol with offer/accept/reject/progress/complete events
- Directory/master services:
  - Server browser registration + listing
  - Server comments with threaded replies + likes
  - Matchmaking from player profile data (region/playlist/mmr/party size)
  - NAT traversal coordination (UDP hole-punch rendezvous)
- Native platform sockets (WinSock2 / POSIX sockets), no external deps

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Quick start

```cpp
#include "unet/unet.hpp"

unet::Host server;
auto started = server.start_server(7777);

auto addr = unet::Address::resolve("127.0.0.1", 7777);
unet::Host client;
auto peer = client.connect(*addr);

for (;;) {
    server.service();
    client.service();

    while (auto e = client.poll_event()) {
        if (e->type == unet::Event::Type::Connect) {
            auto bytes = unet::to_bytes("hello");
            client.send(*peer, bytes);
        }
    }
}
```

## Delivery modes

- `Delivery::ReliableOrdered`
- `Delivery::Unreliable`
- `Delivery::UnreliableSequenced`

## P2P + files

```cpp
unet::HostConfig cfg{};
cfg.file_transfer_channel = 7;
cfg.download_directory = "downloads";
cfg.auto_accept_file_transfers = true;

unet::Host a(cfg);
unet::Host b(cfg);
a.start_p2p(40001, "127.0.0.1");
b.start_p2p(40002, "127.0.0.1");

auto addr = unet::Address::resolve("127.0.0.1", 40002, false);
auto peer = a.connect(*addr);

// Once connected:
a.send_file(*peer, "assets/map.bin");
```

File transfer events:

- `Event::Type::FileOffer`
- `Event::Type::FileProgress`
- `Event::Type::FileComplete`
- `Event::Type::FileRejected`

`file_transfer_channel` is reserved for the built-in file protocol; avoid using it for normal game payloads.

## Directory Services (Browser, Comments, Matchmaking, NAT)

```cpp
unet::DirectoryConfig cfg{};
cfg.channel = 1;               // reserved control channel
cfg.host.channel_count = 8;

unet::DirectoryServer directory(cfg);
directory.start(39000, "0.0.0.0");

unet::DirectoryClient client(cfg);
client.start_node(39001, "0.0.0.0"); // enables P2P + NAT punch use
auto addr = unet::Address::resolve("127.0.0.1", 39000, false);
client.connect(*addr);
```

Client APIs:

- Server browser:
  - `register_server(...)`
  - `heartbeat_server(...)`
  - `list_servers(region, mode)`
- Comments:
  - `post_comment(server_id, author, text, parent_id)`
  - `like_comment(server_id, comment_id, user_id)`
  - `list_comments(server_id)`
- Matchmaking:
  - `enqueue_matchmaking(profile)`
  - `cancel_matchmaking(ticket_id)`
  - `poll_match_found()`
- NAT traversal:
  - `register_nat_identity(identity)`
  - `request_nat_punch(target_identity)` (returns target public endpoint)
  - `poll_nat_punch()` (incoming punch requests)
  - then use `transport().connect(address)` for direct P2P attempt

## Examples

- `unetcode_chat_server`
- `unetcode_chat_client`
- `unetcode_p2p_file_node`
