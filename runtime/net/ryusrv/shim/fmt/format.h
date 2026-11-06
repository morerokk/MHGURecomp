// The little of fmt::format the server sources use: "{}" and "{:08X}" placeholders, filled in order.
#pragma once
#include <cstdio>
#include <string>
#include <type_traits>
namespace fmt {
namespace detail {
template <typename T>
inline std::string One(const std::string& spec, const T& v) {
    char b[64];
    if constexpr (std::is_integral_v<T>) {
        if (spec == ":08X") std::snprintf(b, sizeof b, "%08X", static_cast<unsigned>(v));
        else if (std::is_signed_v<T>) std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
        else std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(v));
        return b;
    } else {
        return std::string(v);
    }
}
inline void Fill(std::string& out, const char* f) { out += f; }
template <typename T, typename... R>
inline void Fill(std::string& out, const char* f, const T& v, const R&... rest) {
    for (; *f; f++) {
        if (*f == '{') {
            std::string spec;
            const char* e = f + 1;
            while (*e && *e != '}') spec += *e++;
            out += One(spec, v);
            Fill(out, *e ? e + 1 : e, rest...);
            return;
        }
        out += *f;
    }
}
} // namespace detail
template <typename... A>
inline std::string format(const char* f, const A&... a) {
    std::string out;
    detail::Fill(out, f, a...);
    return out;
}
} // namespace fmt
