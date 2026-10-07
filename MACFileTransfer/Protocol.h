#pragma once
#include "BaseLayer.h"

namespace Wire {

constexpr size_t EthernetHeader = 14;
constexpr size_t MTU = 1500;

// 표준 EtherType이다.
constexpr uint16_t IpType = 0x0800;
constexpr uint16_t ArpType = 0x0806;

constexpr MacAddress BroadcastMac{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// 모든 다중 바이트 정수는 네트워크 바이트 순서(big endian)로 직렬화한다.
inline uint16_t Read16(const unsigned char* data) {
    return static_cast<uint16_t>(data[0]) << 8 | data[1];
}

inline uint32_t Read32(const unsigned char* data) {
    return static_cast<uint32_t>(Read16(data)) << 16 | Read16(data + 2);
}

inline uint64_t Read64(const unsigned char* data) {
    return static_cast<uint64_t>(Read32(data)) << 32 | Read32(data + 4);
}

inline void Write16(unsigned char* data, uint16_t value) {
    data[0] = static_cast<unsigned char>(value >> 8);
    data[1] = static_cast<unsigned char>(value);
}

inline void Write32(unsigned char* data, uint32_t value) {
    Write16(data, static_cast<uint16_t>(value >> 16));
    Write16(data + 2, static_cast<uint16_t>(value));
}

inline void Write64(unsigned char* data, uint64_t value) {
    Write32(data, static_cast<uint32_t>(value >> 32));
    Write32(data + 4, static_cast<uint32_t>(value));
}

inline std::wstring MacText(const MacAddress& address) {
    wchar_t text[18];
    swprintf_s(
        text,
        L"%02X:%02X:%02X:%02X:%02X:%02X",
        address[0],
        address[1],
        address[2],
        address[3],
        address[4],
        address[5]);
    return text;
}

inline bool ParseMac(const std::wstring& text, MacAddress& address) {
    if (text.size() != 17) {
        return false;
    }

    const auto HexValue = [](wchar_t character) -> int {
        if (character >= L'0' && character <= L'9') {
            return character - L'0';
        }
        if (character >= L'a' && character <= L'f') {
            return character - L'a' + 10;
        }
        if (character >= L'A' && character <= L'F') {
            return character - L'A' + 10;
        }
        return -1;
    };

    for (size_t i = 0; i < address.size(); ++i) {
        const int high = HexValue(text[3 * i]);
        const int low = HexValue(text[3 * i + 1]);
        const bool invalidSeparator =
            i < address.size() - 1 && text[3 * i + 2] != L':' && text[3 * i + 2] != L'-';

        if (high < 0 || low < 0 || invalidSeparator) {
            return false;
        }

        address[i] = static_cast<unsigned char>(high * 16 + low);
    }

    // 00:00:00:00:00:00은 실제 통신 주소로 사용하지 않는다.
    return std::any_of(address.begin(), address.end(), [](unsigned char byte) {
        return byte != 0;
    });
}

inline std::wstring IpText(const IpAddress& address) {
    wchar_t text[16];
    swprintf_s(text, L"%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
    return text;
}

// 점으로 구분한 10진수 4개(각 0~255)만 허용한다.
inline bool ParseIp(const std::wstring& text, IpAddress& address) {
    size_t position = 0;
    for (size_t i = 0; i < address.size(); ++i) {
        if (i > 0) {
            if (position >= text.size() || text[position] != L'.') {
                return false;
            }
            ++position;
        }

        unsigned value = 0;
        size_t digits = 0;
        while (position < text.size() && text[position] >= L'0' && text[position] <= L'9' && digits < 3) {
            value = value * 10 + (text[position] - L'0');
            ++position;
            ++digits;
        }

        if (digits == 0 || value > 255) {
            return false;
        }
        address[i] = static_cast<unsigned char>(value);
    }
    return position == text.size();
}

inline std::string Utf8(const std::wstring& text) {
    const int length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (length <= 0) {
        return {};
    }

    std::string result(length, 0);
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        &result[0],
        length,
        nullptr,
        nullptr);
    return result;
}

inline std::wstring Wide(const std::string& text) {
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);

    if (length <= 0) {
        return {};
    }

    std::wstring result(length, 0);
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        &result[0],
        length);
    return result;
}

}  // namespace Wire
