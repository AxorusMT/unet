#pragma once

#include "unet/error.hpp"

#include <expected>
#include <string>

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace unet::detail {

#if defined(_WIN32)
using SocketHandle = SOCKET;
using SockLen = int;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
using SockLen = socklen_t;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

std::expected<void, Error> initialize_sockets();
void shutdown_sockets() noexcept;

int last_socket_error() noexcept;
std::string socket_error_message(int code);

void close_socket(SocketHandle handle) noexcept;
std::expected<void, Error> set_non_blocking(SocketHandle handle);

}  // namespace unet::detail

