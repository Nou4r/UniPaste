#include "spoof.h"

#include <cstddef>

namespace uni {
namespace {

// Homoglyph tables ported verbatim from index.html (unicodeConverter()).
// Indexed by (c - L'a'); a value of 0 means "no entry in the JS map object".

// modes.basic.map: e y i o p a s j x c
constexpr wchar_t kBasicMap[26] = {
    /* a */ L'\u0430', /* b */ 0,         /* c */ L'\u0441', /* d */ 0,
    /* e */ L'\u0435', /* f */ 0,         /* g */ 0,         /* h */ 0,
    /* i */ L'\u0456', /* j */ L'\u0458', /* k */ 0,         /* l */ 0,
    /* m */ 0,         /* n */ 0,         /* o */ L'\u043E', /* p */ L'\u0440',
    /* q */ 0,         /* r */ 0,         /* s */ L'\u0455', /* t */ 0,
    /* u */ 0,         /* v */ 0,         /* w */ 0,         /* x */ L'\u0445',
    /* y */ L'\u0443', /* z */ 0,
};

// modes.advanced.map: a..y (no 'z' entry in the source)
constexpr wchar_t kAdvancedMap[26] = {
    /* a */ L'\u03B1', /* b */ L'\u03B2', /* c */ L'\u03F2', /* d */ L'\u0501',
    /* e */ L'\u0435', /* f */ L'\u017F', /* g */ L'\u0261', /* h */ L'\u04BB',
    /* i */ L'\u0456', /* j */ L'\u0458', /* k */ L'\u03BA', /* l */ L'\u04CF',
    /* m */ L'\u043C', /* n */ L'\u0578', /* o */ L'\u043E', /* p */ L'\u0440',
    /* q */ L'\u051B', /* r */ L'\u0433', /* s */ L'\u0455', /* t */ L'\u0442',
    /* u */ L'\u03C5', /* v */ L'\u03BD', /* w */ L'\u0461', /* x */ L'\u0445',
    /* y */ L'\u0443', /* z */ 0,
};

// ASCII case fold. Equivalent to towlower() under the default "C" locale, but
// locale-independent so conversion output can never shift under the caller's
// setlocale() state. Every key in both maps is a lowercase ASCII letter, so a
// fold wider than ASCII could not reach any additional entry.
constexpr wchar_t AsciiLower(wchar_t c) noexcept {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

// Mirrors the JS `spoofChar(char, map)` helper: look the lowercased character
// up in the map and take the homoglyph only when it differs from both the
// original character and its lowercased form; otherwise pass the ORIGINAL
// character through untouched. Consequence (intended): 'E' yields U+0435.
constexpr wchar_t SpoofChar(wchar_t c, const wchar_t (&map)[26]) noexcept {
    const wchar_t lower = AsciiLower(c);
    if (lower < L'a' || lower > L'z') {
        return c;
    }
    const wchar_t spoofed = map[static_cast<std::size_t>(lower - L'a')];
    if (spoofed != 0 && spoofed != c && spoofed != lower) {
        return spoofed;
    }
    return c;
}

// Mirrors the JS smart-mode test `!spoof || spoof === char.toLowerCase()`.
constexpr bool UnspoofableInBasic(wchar_t c) noexcept {
    const wchar_t lower = AsciiLower(c);
    if (lower < L'a' || lower > L'z') {
        return true;
    }
    const wchar_t spoofed = kBasicMap[static_cast<std::size_t>(lower - L'a')];
    return spoofed == 0 || spoofed == lower;
}

} // namespace

std::wstring Convert(std::wstring_view input, Mode mode,
                     const std::vector<bool>& protectedSpans) {
    const std::size_t length = input.size();

    // A whitelisted code unit is emitted verbatim, whatever the mode would
    // otherwise have done with it. Every *positional* decision below still
    // reads the original characters at their original indices, so protecting a
    // span never changes which map an unprotected index uses. An empty mask
    // means "nothing protected"; a short one protects only what it covers.
    const auto pinned = [&protectedSpans](std::size_t i) noexcept {
        return i < protectedSpans.size() && protectedSpans[i];
    };

    std::wstring result;
    result.reserve(length);

    switch (mode) {
    case Mode::Basic:
        for (std::size_t i = 0; i < length; ++i) {
            result.push_back(pinned(i) ? input[i] : SpoofChar(input[i], kBasicMap));
        }
        break;

    case Mode::Advanced:
        for (std::size_t i = 0; i < length; ++i) {
            result.push_back(pinned(i) ? input[i] : SpoofChar(input[i], kAdvancedMap));
        }
        break;

    case Mode::First:
        // Index 0 uses the advanced map, every remaining index uses basic.
        for (std::size_t i = 0; i < length; ++i) {
            result.push_back(pinned(i)
                                 ? input[i]
                                 : SpoofChar(input[i], i == 0 ? kAdvancedMap : kBasicMap));
        }
        break;

    case Mode::Smart:
    default: {
        bool useAdvancedForFirstTwo = false;
        if (length >= 2) {
            useAdvancedForFirstTwo =
                UnspoofableInBasic(input[0]) && UnspoofableInBasic(input[1]);
        } else if (length == 1) {
            useAdvancedForFirstTwo = UnspoofableInBasic(input[0]);
        }

        for (std::size_t i = 0; i < length; ++i) {
            const bool advanced = useAdvancedForFirstTwo && (i == 0 || i == 1);
            result.push_back(pinned(i)
                                 ? input[i]
                                 : SpoofChar(input[i], advanced ? kAdvancedMap : kBasicMap));
        }
        break;
    }
    }

    return result;
}

const wchar_t* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Advanced:
        return L"Advanced";
    case Mode::First:
        return L"First";
    case Mode::Smart:
        return L"Smart";
    case Mode::Basic:
    default:
        return L"Basic";
    }
}

Mode NextMode(Mode mode) {
    switch (mode) {
    case Mode::Basic:
        return Mode::Advanced;
    case Mode::Advanced:
        return Mode::First;
    case Mode::First:
        return Mode::Smart;
    case Mode::Smart:
        return Mode::Basic;
    default:
        return Mode::Basic;
    }
}

} // namespace uni
