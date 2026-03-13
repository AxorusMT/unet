#include "detail/platform.hpp"

#include <atomic>
#include <cstring>

namespace unet::detail {

namespace {
std::atomic<int> g_socket_users{0};
}

std::expected<void, Error> initialize_sockets() {
#if defined(_WIN32)
    const int previous = g_socket_users.fetch_add(1, std::memory_order_acq_rel);
    if (previous == 0) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            g_socket_users.fetch_sub(1, std::memory_order_acq_rel);
            return std::unexpected(Error{
                .code = ErrorCode::SocketInitializationFailed,
                .message = "WSAStartup failed"
            });
        }
    }
#else
    g_socket_users.fetch_add(1, std::memory_order_acq_rel);
#endif
    return {};
}

void shutdown_sockets() noexcept {
    const int remaining = g_socket_users.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining < 0) {
        g_socket_users.store(0, std::memory_order_release);
        return;
    }

#if defined(_WIN32)
    if (remaining == 0) {
        WSACleanup();
    }
#endif
}

int last_socket_error() noexcept {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string socket_error_message(int code) {
#if defined(_WIN32)
    char* text = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(flags, nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
    std::string message = (length > 0 && text != nullptr) ? std::string(text, length) : "Unknown socket error";
    if (text != nullptr) {
        LocalFree(text);
    }
    return message;
#else
    return std::strerror(code);
#endif
}

void close_socket(SocketHandle handle) noexcept {
    if (handle == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    closesocket(handle);
#else
    close(handle);
#endif
}

std::expected<void, Error> set_non_blocking(SocketHandle handle) {
#if defined(_WIN32)
    u_long enabled = 1;
    if (ioctlsocket(handle, FIONBIO, &enabled) != 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOptionFailed,
            .message = socket_error_message(last_socket_error())
        });
    }
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOptionFailed,
            .message = socket_error_message(last_socket_error())
        });
    }
    if (fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        return std::unexpected(Error{
            .code = ErrorCode::SocketOptionFailed,
            .message = socket_error_message(last_socket_error())
        });
    }
#endif
    return {};
}

}  // namespace unet::detail
