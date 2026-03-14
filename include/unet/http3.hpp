#pragma once

#include "unet/address.hpp"
#include "unet/error.hpp"
#include "unet/host.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace unet {

struct Http3Header {
    std::string name{};
    std::string value{};
};

struct Http3Request {
    std::string method{"GET"};
    std::string scheme{"https"};
    std::string authority{};
    std::string path{"/"};
    std::vector<Http3Header> headers{};
    std::vector<std::byte> body{};
};

struct Http3Response {
    std::uint16_t status{200};
    std::vector<Http3Header> headers{};
    std::vector<std::byte> body{};
};

struct Http3Config {
    HostConfig host{};
    std::uint8_t stream_channel{0};
    std::chrono::milliseconds request_timeout{5000};
};

class Http3Server final {
public:
    explicit Http3Server(Http3Config config = {});
    ~Http3Server();

    Http3Server(const Http3Server&) = delete;
    Http3Server& operator=(const Http3Server&) = delete;
    Http3Server(Http3Server&&) noexcept;
    Http3Server& operator=(Http3Server&&) noexcept;

    using Handler = std::function<Http3Response(const Http3Request&)>;

    [[nodiscard]] std::expected<void, Error> start(std::uint16_t port, std::string_view bind_ip = "::");
    void service();
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::optional<Error> last_error() const;
    void set_handler(Handler handler);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class Http3Client final {
public:
    explicit Http3Client(Http3Config config = {});
    ~Http3Client();

    Http3Client(const Http3Client&) = delete;
    Http3Client& operator=(const Http3Client&) = delete;
    Http3Client(Http3Client&&) noexcept;
    Http3Client& operator=(Http3Client&&) noexcept;

    [[nodiscard]] std::expected<void, Error> connect(const Address& remote);
    void service();
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] std::optional<Error> last_error() const;
    [[nodiscard]] std::expected<Http3Response, Error> request(
        const Http3Request& request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace unet

