#pragma once

// Guest printf-family formatting for the kernel's CRT exports (sprintf,
// _vsnprintf; V380). They were unresolved-import traps: a library debug print
// on an error path (sub_8286C3C8: _vsnprintf into 260 bytes, then DbgPrint)
// ended the game instead of printing. Varargs are 8-byte slots: a va_list
// points at them (the caller spilled r4..r10 into its save area, stack
// arguments follow at +0x50), sprintf's come from r5..r10 and then the stack.
// A 32-bit integer or pointer is the low word of its slot; a double is the
// slot's raw bits.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

struct RuntimeGuestFormatArgs {
    // Next 8-byte argument slot.
    std::function<uint64_t()> next;
    // Guest C string / UTF-16BE string at an address (bounded; empty if null).
    std::function<std::string(uint32_t)> read_string;
    std::function<std::u16string(uint32_t)> read_wide;
};

inline std::string RuntimeUtf16ToUtf8(const std::u16string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t c = text[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t(text[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Formats like the Microsoft CRT for the common conversions (d i u o x X c s S
// p f F e E g G a A n-ignored %), flags "-+ #0", width/precision incl. '*',
// length prefixes h hh l ll I64 I32 z j t L. Unknown conversions are copied.
inline std::string RuntimeFormatGuestString(const char* format, RuntimeGuestFormatArgs& args,
                                            size_t limit = 64 * 1024) {
    std::string out;
    if (!format) return out;
    for (const char* p = format; *p && out.size() < limit; ++p) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            continue;
        }
        std::string flags;
        while (*p && std::strchr("-+ #0", *p)) flags += *p++;
        std::string width;
        if (*p == '*') {
            width = std::to_string(int32_t(uint32_t(args.next())));
            ++p;
        } else {
            while (*p >= '0' && *p <= '9') width += *p++;
        }
        std::string precision;
        bool has_precision = false;
        if (*p == '.') {
            has_precision = true;
            ++p;
            if (*p == '*') {
                precision = std::to_string(int32_t(uint32_t(args.next())));
                ++p;
            } else {
                while (*p >= '0' && *p <= '9') precision += *p++;
            }
        }
        int length = 0;  // 0 int, 1 short, 2 char, 3 long, 4 64-bit, 5 long double
        if (*p == 'h') {
            ++p;
            length = 1;
            if (*p == 'h') {
                ++p;
                length = 2;
            }
        } else if (*p == 'l') {
            ++p;
            length = 3;
            if (*p == 'l') {
                ++p;
                length = 4;
            }
        } else if (*p == 'I' && p[1] == '6' && p[2] == '4') {
            p += 3;
            length = 4;
        } else if (*p == 'I' && p[1] == '3' && p[2] == '2') {
            p += 3;
        } else if (*p == 'z' || *p == 'j' || *p == 't') {
            length = *p == 'j' ? 4 : 0;  // 32-bit size_t/ptrdiff_t on the guest
            ++p;
        } else if (*p == 'L') {
            ++p;
            length = 5;
        }
        const char conversion = *p;
        if (!conversion) break;
        std::string spec = "%" + flags + width + (has_precision ? "." + precision : std::string());
        char buffer[512];
        switch (conversion) {
            case 'd':
            case 'i': {
                const uint64_t slot = args.next();
                int64_t value = length == 4 ? int64_t(slot)
                                : length == 1 ? int64_t(int16_t(uint16_t(slot)))
                                : length == 2 ? int64_t(int8_t(uint8_t(slot)))
                                              : int64_t(int32_t(uint32_t(slot)));
                std::snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(),
                              static_cast<long long>(value));
                out += buffer;
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                const uint64_t slot = args.next();
                uint64_t value = length == 4 ? slot
                                 : length == 1 ? uint64_t(uint16_t(slot))
                                 : length == 2 ? uint64_t(uint8_t(slot))
                                               : uint64_t(uint32_t(slot));
                std::snprintf(buffer, sizeof(buffer), (spec + "ll" + conversion).c_str(),
                              static_cast<unsigned long long>(value));
                out += buffer;
                break;
            }
            case 'p': {
                std::snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.next()));
                out += buffer;
                break;
            }
            case 'c':
            case 'C': {
                const uint32_t value = uint32_t(args.next());
                std::string text = (conversion == 'C' || length == 3)
                                       ? RuntimeUtf16ToUtf8(std::u16string(1, char16_t(value)))
                                       : std::string(1, char(value & 0xFF));
                std::snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), text.c_str());
                out += buffer;
                break;
            }
            case 's':
            case 'S': {
                const uint32_t address = uint32_t(args.next());
                const bool wide = (conversion == 'S') != (length == 3);
                std::string text = !address ? std::string("(null)")
                                   : wide  ? RuntimeUtf16ToUtf8(args.read_wide(address))
                                           : args.read_string(address);
                if (has_precision && !precision.empty()) {
                    const size_t cap = size_t(std::max(0, std::atoi(precision.c_str())));
                    if (text.size() > cap) text.resize(cap);
                }
                const int field = std::atoi(width.c_str());
                if (field > int(text.size())) {
                    const std::string pad(size_t(field) - text.size(), ' ');
                    text = flags.find('-') != std::string::npos ? text + pad : pad + text;
                }
                out += text;
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                const uint64_t bits = args.next();
                double value;
                std::memcpy(&value, &bits, sizeof(value));
                std::snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), value);
                out += buffer;
                break;
            }
            case 'n':
                args.next();  // never written back (and never needed by debug prints)
                break;
            default:
                out.append(start, size_t(p - start + 1));
                break;
        }
    }
    if (out.size() > limit) out.resize(limit);
    return out;
}
