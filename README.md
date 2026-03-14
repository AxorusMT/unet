# unet
`unet` is a modern C++23 game networking library inspired by ENet, with UDP, TCP, and QUIC transport modes.

It provides:

- Connection-oriented host/peer model over UDP, TCP, or QUIC
- P2P node mode (listen + dial on the same host)
- Reliable ordered delivery with retransmission/ordering
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
- Optional UPnP IGD port mapping for incoming UDP/TCP/QUIC endpoints
- Native platform sockets (WinSock2 / POSIX sockets), no external deps

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
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

## Transport selection

```cpp
unet::HostConfig cfg{};
cfg.transport = unet::Transport::Quic; // default is Transport::Udp

unet::Host server(cfg);
server.start_server(7777, "0.0.0.0");
```

`Transport::Quic` currently uses the datagram transport path (UDP socket + unet packet protocol). It does not yet implement TLS, HTTP/3, or QUIC stream primitives.

`DirectoryServer` / `DirectoryClient` use `DirectoryConfig::host.transport`.

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

## UPnP Port Mapping

```cpp
unet::HostConfig cfg{};
cfg.enable_upnp = true;
cfg.require_upnp = false; // start still succeeds if mapping fails
cfg.upnp_description = "my-unet-server";

unet::Host server(cfg);
server.start_server(7777, "0.0.0.0");

if (auto public_ep = server.upnp_external_address()) {
    // Router mapping succeeded
    std::cout << public_ep->to_string() << "\n";
} else if (auto upnp_error = server.upnp_last_error()) {
    // Mapping was attempted but failed
    std::cout << upnp_error->message << "\n";
}
```

UPnP fields in `HostConfig`:

- `enable_upnp`
- `require_upnp`
- `upnp_external_port` (`0` = same as listen port)
- `upnp_discovery_timeout`
- `upnp_lease_duration`
- `upnp_description`
- `upnp_discovery_address` / `upnp_discovery_port` (useful for testing)

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

- `unet_chat_server`
- `unet_chat_client`
- `unet_p2p_file_node`
- `unet_tcp_chat_server`
- `unet_tcp_chat_client`
- `unet_upnp_server`
