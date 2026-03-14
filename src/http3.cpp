#include "unet/http3.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace unet {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::byte kWireRequest = static_cast<std::byte>(0x01);
constexpr std::byte kWireResponse = static_cast<std::byte>(0x02);

void write_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

void write_u32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> input, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[offset]) << 8) |
        static_cast<std::uint16_t>(input[offset + 1]));
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> input, std::size_t offset) {
    return (static_cast<std::uint32_t>(input[offset]) << 24) |
           (static_cast<std::uint32_t>(input[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(input[offset + 2]) << 8) |
           static_cast<std::uint32_t>(input[offset + 3]);
}

bool write_text(std::vector<std::byte>& out, std::string_view text) {
    if (text.size() > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    write_u16(out, static_cast<std::uint16_t>(text.size()));
    for (char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return true;
}

bool read_text(std::span<const std::byte> input, std::size_t& offset, std::string& out) {
    if (offset + 2 > input.size()) {
        return false;
    }
    const std::uint16_t size = read_u16(input, offset);
    offset += 2;
    if (offset + size > input.size()) {
        return false;
    }
    out.resize(size);
    if (size > 0) {
        std::memcpy(out.data(), input.data() + static_cast<std::ptrdiff_t>(offset), size);
    }
    offset += size;
    return true;
}

std::expected<std::vector<std::byte>, Error> encode_request(const Http3Request& request) {
    std::vector<std::byte> payload;
    payload.reserve(128 + request.body.size() + request.headers.size() * 32);
    payload.push_back(kWireRequest);
    if (!write_text(payload, request.method) ||
        !write_text(payload, request.scheme) ||
        !write_text(payload, request.authority) ||
        !write_text(payload, request.path)) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "HTTP/3 request field exceeds size limits"});
    }
    if (request.headers.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "HTTP/3 request has too many headers"});
    }
    write_u16(payload, static_cast<std::uint16_t>(request.headers.size()));
    for (const auto& header : request.headers) {
        if (!write_text(payload, header.name) || !write_text(payload, header.value)) {
            return std::unexpected(Error{ErrorCode::MessageTooLarge, "HTTP/3 header exceeds size limits"});
        }
    }
    if (request.body.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "HTTP/3 request body is too large"});
    }
    write_u32(payload, static_cast<std::uint32_t>(request.body.size()));
    payload.insert(payload.end(), request.body.begin(), request.body.end());
    return payload;
}

std::expected<std::vector<std::byte>, Error> encode_response(const Http3Response& response) {
    std::vector<std::byte> payload;
    payload.reserve(128 + response.body.size() + response.headers.size() * 32);
    payload.push_back(kWireResponse);
    write_u16(payload, response.status);
    if (response.headers.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "HTTP/3 response has too many headers"});
    }
    write_u16(payload, static_cast<std::uint16_t>(response.headers.size()));
    for (const auto& header : response.headers) {
        if (!write_text(payload, header.name) || !write_text(payload, header.value)) {
            return std::unexpected(Error{ErrorCode::MessageTooLarge, "HTTP/3 header exceeds size limits"});
        }
    }
    if (response.body.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(Error{ErrorCode::MessageTooLarge, "HTTP/3 response body is too large"});
    }
    write_u32(payload, static_cast<std::uint32_t>(response.body.size()));
    payload.insert(payload.end(), response.body.begin(), response.body.end());
    return payload;
}

std::expected<Http3Request, Error> decode_request(std::span<const std::byte> payload) {
    if (payload.size() < 1 || payload[0] != kWireRequest) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "HTTP/3 request marker mismatch"});
    }
    std::size_t offset = 1;
    Http3Request request{};
    if (!read_text(payload, offset, request.method) ||
        !read_text(payload, offset, request.scheme) ||
        !read_text(payload, offset, request.authority) ||
        !read_text(payload, offset, request.path)) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 request fields"});
    }
    if (offset + 2 > payload.size()) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 request header count"});
    }
    const std::uint16_t header_count = read_u16(payload, offset);
    offset += 2;
    request.headers.reserve(header_count);
    for (std::uint16_t i = 0; i < header_count; ++i) {
        Http3Header header{};
        if (!read_text(payload, offset, header.name) || !read_text(payload, offset, header.value)) {
            return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 request header"});
        }
        request.headers.push_back(std::move(header));
    }
    if (offset + 4 > payload.size()) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 request body size"});
    }
    const std::uint32_t body_size = read_u32(payload, offset);
    offset += 4;
    if (offset + body_size != payload.size()) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 request body"});
    }
    request.body.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end());
    return request;
}

std::expected<Http3Response, Error> decode_response(std::span<const std::byte> payload) {
    if (payload.size() < 1 || payload[0] != kWireResponse) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "HTTP/3 response marker mismatch"});
    }
    std::size_t offset = 1;
    Http3Response response{};
    if (offset + 2 > payload.size()) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 response status"});
    }
    response.status = read_u16(payload, offset);
    offset += 2;
    if (offset + 2 > payload.size()) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 response header count"});
    }
    const std::uint16_t header_count = read_u16(payload, offset);
    offset += 2;
    response.headers.reserve(header_count);
    for (std::uint16_t i = 0; i < header_count; ++i) {
        Http3Header header{};
        if (!read_text(payload, offset, header.name) || !read_text(payload, offset, header.value)) {
            return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 response header"});
        }
        response.headers.push_back(std::move(header));
    }
    if (offset + 4 > payload.size()) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 response body size"});
    }
    const std::uint32_t body_size = read_u32(payload, offset);
    offset += 4;
    if (offset + body_size != payload.size()) {
        return std::unexpected(Error{ErrorCode::ProtocolMismatch, "Malformed HTTP/3 response body"});
    }
    response.body.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end());
    return response;
}

}  // namespace

class Http3Server::Impl {
public:
    explicit Impl(Http3Config config)
        : config_(std::move(config)),
          host_([&]() {
              HostConfig host_config = config_.host;
              host_config.transport = Transport::Quic;
              return host_config;
          }()) {}

    [[nodiscard]] std::expected<void, Error> start(std::uint16_t port, std::string_view bind_ip) {
        if (running_) {
            return std::unexpected(Error{ErrorCode::InvalidState, "HTTP/3 server already running"});
        }
        auto started = host_.start_server(port, bind_ip);
        if (!started.has_value()) {
            last_error_ = started.error();
            return std::unexpected(started.error());
        }
        running_ = true;
        return {};
    }

    void service() {
        if (!running_) {
            return;
        }

        host_.service();
        while (auto event = host_.poll_event()) {
            switch (event->type) {
            case Event::Type::Connect:
                break;
            case Event::Type::Disconnect:
                streams_.erase(event->disconnect.peer);
                break;
            case Event::Type::StreamOpen:
                streams_[event->stream_open.peer][event->stream_open.stream] = {};
                break;
            case Event::Type::StreamData: {
                auto& buffer = streams_[event->stream_data.peer][event->stream_data.stream];
                buffer.insert(buffer.end(), event->stream_data.payload.begin(), event->stream_data.payload.end());
                if (!event->stream_data.fin) {
                    break;
                }

                auto request = decode_request(buffer);
                if (!request.has_value()) {
                    last_error_ = request.error();
                    (void)host_.close_stream(event->stream_data.peer, event->stream_data.stream, 1);
                    streams_[event->stream_data.peer].erase(event->stream_data.stream);
                    break;
                }

                Http3Response response = handler_(*request);
                auto encoded = encode_response(response);
                if (!encoded.has_value()) {
                    last_error_ = encoded.error();
                    (void)host_.close_stream(event->stream_data.peer, event->stream_data.stream, 2);
                    streams_[event->stream_data.peer].erase(event->stream_data.stream);
                    break;
                }

                StreamSendOptions options{};
                options.fin = true;
                auto sent = host_.send_stream(event->stream_data.peer, event->stream_data.stream, *encoded, options);
                if (!sent.has_value()) {
                    last_error_ = sent.error();
                    (void)host_.close_stream(event->stream_data.peer, event->stream_data.stream, 3);
                }
                streams_[event->stream_data.peer].erase(event->stream_data.stream);
                break;
            }
            case Event::Type::StreamClose:
                streams_[event->stream_close.peer].erase(event->stream_close.stream);
                break;
            case Event::Type::Message:
            case Event::Type::FileOffer:
            case Event::Type::FileProgress:
            case Event::Type::FileComplete:
            case Event::Type::FileRejected:
                break;
            }
        }
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_;
    }

    [[nodiscard]] std::optional<Error> last_error() const {
        return last_error_;
    }

    void set_handler(Handler handler) {
        if (handler) {
            handler_ = std::move(handler);
        }
    }

private:
    Http3Config config_{};
    Host host_{};
    bool running_{false};
    std::optional<Error> last_error_{};
    Handler handler_{[](const Http3Request&) {
        Http3Response response{};
        response.status = 404;
        response.body = to_bytes("not found");
        return response;
    }};
    std::unordered_map<PeerId, std::unordered_map<StreamId, std::vector<std::byte>>> streams_{};
};

class Http3Client::Impl {
public:
    explicit Impl(Http3Config config)
        : config_(std::move(config)),
          host_([&]() {
              HostConfig host_config = config_.host;
              host_config.transport = Transport::Quic;
              return host_config;
          }()) {}

    [[nodiscard]] std::expected<void, Error> connect(const Address& remote) {
        auto result = host_.connect(remote);
        if (!result.has_value()) {
            last_error_ = result.error();
            return std::unexpected(result.error());
        }
        peer_ = *result;
        return {};
    }

    void service() {
        host_.service();
        while (auto event = host_.poll_event()) {
            switch (event->type) {
            case Event::Type::Connect:
                connected_ = true;
                break;
            case Event::Type::Disconnect:
                if (event->disconnect.peer == peer_) {
                    connected_ = false;
                }
                break;
            case Event::Type::StreamData: {
                auto& response = pending_streams_[event->stream_data.stream];
                response.insert(response.end(), event->stream_data.payload.begin(), event->stream_data.payload.end());
                if (event->stream_data.fin) {
                    completed_streams_.push_back(event->stream_data.stream);
                }
                break;
            }
            case Event::Type::StreamClose:
                completed_streams_.push_back(event->stream_close.stream);
                break;
            case Event::Type::Message:
            case Event::Type::StreamOpen:
            case Event::Type::FileOffer:
            case Event::Type::FileProgress:
            case Event::Type::FileComplete:
            case Event::Type::FileRejected:
                break;
            }
        }
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return connected_;
    }

    [[nodiscard]] std::optional<Error> last_error() const {
        return last_error_;
    }

    [[nodiscard]] std::expected<Http3Response, Error> request(const Http3Request& request, std::chrono::milliseconds timeout) {
        if (peer_ == invalid_peer_id) {
            return std::unexpected(Error{ErrorCode::InvalidState, "HTTP/3 client is not connected"});
        }

        const auto deadline = Clock::now() + timeout;
        while (!connected_ && Clock::now() < deadline) {
            service();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!connected_) {
            return std::unexpected(Error{ErrorCode::InvalidState, "HTTP/3 connect timeout"});
        }

        StreamOpenOptions stream_options{};
        stream_options.channel = config_.stream_channel;
        stream_options.bidirectional = true;
        auto stream = host_.open_stream(peer_, stream_options);
        if (!stream.has_value()) {
            last_error_ = stream.error();
            return std::unexpected(stream.error());
        }

        auto encoded = encode_request(request);
        if (!encoded.has_value()) {
            last_error_ = encoded.error();
            return std::unexpected(encoded.error());
        }

        StreamSendOptions send_options{};
        send_options.fin = true;
        auto sent = host_.send_stream(peer_, *stream, *encoded, send_options);
        if (!sent.has_value()) {
            last_error_ = sent.error();
            return std::unexpected(sent.error());
        }

        while (Clock::now() < deadline) {
            service();

            auto done_it = std::find(completed_streams_.begin(), completed_streams_.end(), *stream);
            if (done_it != completed_streams_.end()) {
                completed_streams_.erase(done_it);
                auto payload_it = pending_streams_.find(*stream);
                if (payload_it == pending_streams_.end()) {
                    return std::unexpected(Error{ErrorCode::ProtocolMismatch, "HTTP/3 response stream missing payload"});
                }
                auto decoded = decode_response(payload_it->second);
                pending_streams_.erase(payload_it);
                if (!decoded.has_value()) {
                    last_error_ = decoded.error();
                    return std::unexpected(decoded.error());
                }
                return *decoded;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return std::unexpected(Error{ErrorCode::InvalidState, "HTTP/3 request timeout"});
    }

private:
    Http3Config config_{};
    Host host_{};
    PeerId peer_{invalid_peer_id};
    bool connected_{false};
    std::optional<Error> last_error_{};
    std::unordered_map<StreamId, std::vector<std::byte>> pending_streams_{};
    std::deque<StreamId> completed_streams_{};
};

Http3Server::Http3Server(Http3Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Http3Server::~Http3Server() = default;
Http3Server::Http3Server(Http3Server&&) noexcept = default;
Http3Server& Http3Server::operator=(Http3Server&&) noexcept = default;

std::expected<void, Error> Http3Server::start(std::uint16_t port, std::string_view bind_ip) {
    return impl_->start(port, bind_ip);
}

void Http3Server::service() {
    impl_->service();
}

bool Http3Server::is_running() const noexcept {
    return impl_->is_running();
}

std::optional<Error> Http3Server::last_error() const {
    return impl_->last_error();
}

void Http3Server::set_handler(Handler handler) {
    impl_->set_handler(std::move(handler));
}

Http3Client::Http3Client(Http3Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Http3Client::~Http3Client() = default;
Http3Client::Http3Client(Http3Client&&) noexcept = default;
Http3Client& Http3Client::operator=(Http3Client&&) noexcept = default;

std::expected<void, Error> Http3Client::connect(const Address& remote) {
    return impl_->connect(remote);
}

void Http3Client::service() {
    impl_->service();
}

bool Http3Client::is_connected() const noexcept {
    return impl_->is_connected();
}

std::optional<Error> Http3Client::last_error() const {
    return impl_->last_error();
}

std::expected<Http3Response, Error> Http3Client::request(const Http3Request& request, std::chrono::milliseconds timeout) {
    return impl_->request(request, timeout);
}

}  // namespace unet

