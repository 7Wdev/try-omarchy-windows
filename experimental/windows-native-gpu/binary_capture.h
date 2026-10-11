// SPDX-License-Identifier: MIT
// Diagnostic readback only. Each fixed-name capture is created exactly once.
#pragma once
#include "diagnostic_profile.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <windows.h>
namespace driver_capture {
inline std::filesystem::path directory() {
    const auto size = GetEnvironmentVariableW(L"WDDM_BRIDGE_CAPTURE_DIRECTORY", nullptr, 0);
    if (!size || size > 32768) throw std::runtime_error("4K capture requires WDDM_BRIDGE_CAPTURE_DIRECTORY");
    std::wstring value(size, L'\0');
    if (GetEnvironmentVariableW(L"WDDM_BRIDGE_CAPTURE_DIRECTORY", value.data(), size) != size - 1)
        throw std::runtime_error("4K capture environment changed");
    value.resize(size - 1);
    const std::filesystem::path path(value);
    if (!path.is_absolute() || !std::filesystem::is_directory(path))
        throw std::runtime_error("4K capture directory must be an existing absolute directory");
    return path;
}
inline HRESULT write(const char* prefix, unsigned round, const void* mapped,
                     std::uint64_t offset, unsigned pitch, std::uint64_t& hash) {
    if (!mapped || !prefix || !round || round > 2 || offset != 512 || pitch < gpu_diagnostic::Width * 4 ||
        pitch > gpu_diagnostic::MaxRowPitch || pitch % 256) return E_INVALIDARG;
    const bool back = std::string(prefix) == "NATIVE_SHARED_TEXTURE_BACKBUFFER_ROW";
    if (!back && std::string(prefix) != "NATIVE_SHARED_TEXTURE_PIXEL_ROW") return E_INVALIDARG;
    const auto name = std::string(back ? "native-backbuffer-" : "native-copy-") + std::to_string(round) + ".rgba";
    const auto path = directory() / name;
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    hash = UINT64_C(14695981039346656037);
    bool ok = true;
    for (unsigned y = 0; ok && y < gpu_diagnostic::Height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped) + offset + std::uint64_t(y) * pitch;
        DWORD written = 0;
        ok = WriteFile(file, row, gpu_diagnostic::Width * 4, &written, nullptr) && written == gpu_diagnostic::Width * 4;
        for (unsigned x = 0; ok && x < gpu_diagnostic::Width * 4; ++x)
            hash = (hash ^ row[x]) * UINT64_C(1099511628211);
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    if (!closed) ExitProcess(3);
    // Retain partial files as failure evidence. Never truncate or retry them.
    if (!ok) return E_FAIL;
    std::cerr << "{\"nativeBinaryCapture\":true,\"file\":\"" << name << "\",\"round\":" << round
              << ",\"bytes\":" << std::uint64_t(gpu_diagnostic::Width) * gpu_diagnostic::Height * 4
              << ",\"fnv1a\":" << hash << "}\n";
    return S_OK;
}
} // namespace driver_capture
