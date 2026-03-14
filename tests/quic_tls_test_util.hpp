#pragma once

#include "unet/unet.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace unet::test {

inline std::string quote_cmd_arg(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (char c : value) {
        if (c == '"') {
            quoted.push_back('\\');
        }
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

inline std::string trim_ascii_whitespace(std::string text) {
    auto is_space = [](char c) {
        return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
    };
    while (!text.empty() && is_space(text.front())) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_space(text.back())) {
        text.pop_back();
    }
    return text;
}

inline std::optional<std::string> ensure_quic_test_thumbprint(std::string_view cert_name) {
#if defined(_WIN32)
    std::error_code ec{};
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "unet-quic-tests";
    if (ec) {
        return std::nullopt;
    }
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return std::nullopt;
    }

    const std::filesystem::path script_path = root / "ensure_unet_quic_cert.ps1";
    const std::filesystem::path thumbprint_path = root / (std::string(cert_name) + ".thumbprint.txt");

    std::ofstream script(script_path, std::ios::binary | std::ios::trunc);
    if (!script.is_open()) {
        return std::nullopt;
    }
    script <<
R"(param([string]$FriendlyName, [string]$OutPath)
$ErrorActionPreference = 'Stop'
$minExpiry = (Get-Date).AddHours(24)
$cert = Get-ChildItem -Path Cert:\CurrentUser\My |
    Where-Object { $_.FriendlyName -eq $FriendlyName -and $_.NotAfter -gt $minExpiry } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -DnsName 'localhost' -FriendlyName $FriendlyName -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddDays(7)
}
Set-Content -Path $OutPath -Value $cert.Thumbprint -NoNewline
)";
    script.close();

    const std::string command =
        "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
        quote_cmd_arg(script_path.string()) + " " +
        quote_cmd_arg(std::string(cert_name)) + " " +
        quote_cmd_arg(thumbprint_path.string());
    if (std::system(command.c_str()) != 0) {
        return std::nullopt;
    }

    std::ifstream input(thumbprint_path, std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }
    std::string thumbprint(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    thumbprint = trim_ascii_whitespace(std::move(thumbprint));
    if (thumbprint.empty()) {
        return std::nullopt;
    }
    return thumbprint;
#else
    (void)cert_name;
    return std::nullopt;
#endif
}

inline bool configure_quic_tls(HostConfig& config, std::string_view cert_name) {
    const auto thumbprint = ensure_quic_test_thumbprint(cert_name);
    if (!thumbprint.has_value()) {
        return false;
    }

    config.quic_certificate_thumbprint = *thumbprint;
    config.quic_certificate_store_name = "MY";
    config.quic_certificate_store_machine = false;
    config.quic_insecure_skip_verify = true;
    config.quic_pkcs12_file.clear();
    config.quic_pkcs12_password.clear();
    config.quic_certificate_file.clear();
    config.quic_private_key_file.clear();
    config.quic_private_key_password.clear();
    return true;
}

}  // namespace unet::test
