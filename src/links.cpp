// UniPaste - finds the spans that must never be converted.
//
// Homoglyph substitution breaks anything that is parsed rather than read, and a
// silently mangled URL is the most damaging output this tool can produce. This
// module marks those spans so the converter steps over them.
//
// RECOGNISED, case-insensitively, scanning left to right; the first rule that
// matches at a start position wins and the scan resumes after its span:
//
//   1. Scheme URLs   <scheme>://rest, scheme = [A-Za-z][A-Za-z0-9+.-]* - so
//                    http, https, ftp, file, git+ssh and anything else all
//                    work - plus the slashless "mailto:rest" form.
//   2. ://-tokens    any run of non-whitespace that contains "://" and that
//                    rule 1 missed, e.g. "see:https://a.io" or "[x://y]".
//   3. E-mail        local@host, local = [A-Za-z0-9._%+-]+, host per rule 4.
//   4. Bare hosts    label(.label)+, label = [A-Za-z0-9]([A-Za-z0-9-]*
//                    [A-Za-z0-9])?, with the LAST label 2-24 ASCII letters,
//                    then an optional :port (1-5 digits) and /path. This is
//                    what catches "example.com" and "discord.gg/foo".
//                    A four-part all-numeric dotted quad (1-3 digits a part)
//                    counts as a host only when a :port or a /path follows.
//   5. Handles       '@' plus 2 or more of [A-Za-z0-9._-], the '@' itself at a
//                    word boundary, so "@octocat" is a handle while the '@' in
//                    "a@b" is left to rule 3.
//   6. Windows paths X:\... and X:/... for a single drive letter X, and UNC
//                    \\server\share...; the span ends at whitespace or a quote.
//
// NOT recognised, deliberately - each of these fires on ordinary prose:
//   - Hosts whose last label is numeric or a single letter: "v1.2.3", "1.2",
//     "e.g" and "i.e." are text, not links.
//   - A bare dotted quad with nothing after it ("192.168.0.1").
//   - Single-label hosts: "localhost", "intranet" - no dot, no signal.
//   - A bare "@a": one character after the '@' is noise, not a handle.
//   - Internationalised domain labels ("café.com"): labels are ASCII only.
//   - POSIX and relative paths ("/usr/bin", "./src/main.cpp"): far too common
//     as plain text, and a leading slash says nothing about intent.
//
// KNOWN over-matching, accepted on purpose: a missing space after a full stop
// makes "etc.So" look exactly like a host, and rule 2 swallows whatever prose
// is glued to a URL by punctuation. Protecting a few extra code units is a much
// cheaper mistake than handing the user a broken link.
//
// SPAN ENDS. A URL body runs to the first whitespace, control character or one
// of " < > ` { }, and to a closing ) or ] that has no matching opener inside the
// span - so "(see https://a.io/x)" leaves its wrapping bracket outside while
// ".../Foo_(bar)" keeps its balanced pair inside. Trailing sentence punctuation
// (. , ; : ! ?) is then dropped, which is what keeps the full stop in
// "see https://a.io/x." out of the protected span. An unbalanced closer can
// never be the last code unit of a span, so the trim only has to consider
// punctuation.
//
// Hand-written on purpose: <regex> pays a construction and a backtracking
// engine for every pattern, and this runs on every clipboard change.

#include "links.h"

#include <cstddef>

namespace uni {
namespace links {
namespace {

constexpr bool IsAsciiAlpha(wchar_t c) noexcept {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

constexpr bool IsAsciiDigit(wchar_t c) noexcept {
    return c >= L'0' && c <= L'9';
}

constexpr bool IsAsciiAlnum(wchar_t c) noexcept {
    return IsAsciiAlpha(c) || IsAsciiDigit(c);
}

// The same definition the word-list matcher uses: an already converted
// homoglyph (>= 0x80) counts as part of a word, so a link can never start in
// the middle of one.
constexpr bool IsWordChar(wchar_t c) noexcept {
    return IsAsciiAlnum(c) || c == L'_' || static_cast<unsigned int>(c) >= 0x80u;
}

constexpr bool IsSpaceOrControl(wchar_t c) noexcept {
    const unsigned int u = static_cast<unsigned int>(c);
    return u <= 0x20u || u == 0x7Fu;
}

// Hard terminators for a URL body. Brackets are handled separately because a
// balanced pair belongs inside the span.
constexpr bool IsUrlHardStop(wchar_t c) noexcept {
    return IsSpaceOrControl(c) || c == L'"' || c == L'<' || c == L'>' || c == L'`' ||
           c == L'{' || c == L'}';
}

// "ends at whitespace or a quote"; < > | are illegal in Windows paths anyway.
constexpr bool IsPathStop(wchar_t c) noexcept {
    return IsSpaceOrControl(c) || c == L'"' || c == L'\'' || c == L'<' || c == L'>' ||
           c == L'|';
}

constexpr bool IsSentenceTail(wchar_t c) noexcept {
    return c == L'.' || c == L',' || c == L';' || c == L':' || c == L'!' || c == L'?';
}

constexpr bool IsEmailLocalChar(wchar_t c) noexcept {
    return IsAsciiAlnum(c) || c == L'.' || c == L'_' || c == L'%' || c == L'+' || c == L'-';
}

constexpr bool IsHandleChar(wchar_t c) noexcept {
    return IsAsciiAlnum(c) || c == L'.' || c == L'_' || c == L'-';
}

bool EqualsFoldAscii(std::wstring_view text, const wchar_t* literal) noexcept {
    std::size_t i = 0;
    for (; i < text.size(); ++i) {
        const wchar_t expected = literal[i];
        if (expected == L'\0')
            return false;
        const wchar_t actual = (text[i] >= L'A' && text[i] <= L'Z')
                                   ? static_cast<wchar_t>(text[i] + (L'a' - L'A'))
                                   : text[i];
        if (actual != expected)
            return false;
    }
    return literal[i] == L'\0';
}

// A link may not begin inside a word. Everything else - punctuation, brackets,
// slashes - is a legal left edge.
bool IsLinkStart(std::wstring_view text, std::size_t index) noexcept {
    return index == 0 || !IsWordChar(text[index - 1]);
}

std::size_t TrimTail(std::wstring_view text, std::size_t begin, std::size_t end) noexcept {
    while (end > begin && IsSentenceTail(text[end - 1]))
        --end;
    return end;
}

std::size_t ScanUrlBody(std::wstring_view text, std::size_t index) noexcept {
    std::size_t round  = 0;
    std::size_t square = 0;

    std::size_t i = index;
    while (i < text.size()) {
        const wchar_t c = text[i];
        if (IsUrlHardStop(c))
            break;
        if (c == L'(') {
            ++round;
        } else if (c == L'[') {
            ++square;
        } else if (c == L')') {
            if (round == 0)
                break;  // wraps the URL rather than belonging to it
            --round;
        } else if (c == L']') {
            if (square == 0)
                break;
            --square;
        }
        ++i;
    }
    return i;
}

std::size_t ScanPathBody(std::wstring_view text, std::size_t index) noexcept {
    std::size_t i = index;
    while (i < text.size() && !IsPathStop(text[i]))
        ++i;
    return i;
}

bool ContainsSchemeSeparator(std::wstring_view text, std::size_t begin,
                             std::size_t end) noexcept {
    if (end < 3)
        return false;
    for (std::size_t i = begin; i + 2 < end; ++i) {
        if (text[i] == L':' && text[i + 1] == L'/' && text[i + 2] == L'/')
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Host names
// ---------------------------------------------------------------------------

struct Host {
    std::size_t end      = 0;
    bool        isDomain = false;  // last label is 2-24 ASCII letters
    bool        isQuad   = false;  // four all-numeric labels of 1-3 digits
};

// Scans label(.label)+ at `index`. Returns false unless the result is a usable
// host, i.e. a real domain or a dotted quad (whose caller still has to see a
// port or a path before trusting it).
bool ScanHost(std::wstring_view text, std::size_t index, Host& out) noexcept {
    const std::size_t length = text.size();

    std::size_t i           = index;
    std::size_t labels      = 0;
    std::size_t lastBegin   = index;
    std::size_t lastEnd     = index;
    bool        numericOnly = true;
    bool        quadShaped  = true;

    for (;;) {
        if (i >= length || !IsAsciiAlnum(text[i]))
            break;  // a label has to start with an alphanumeric

        const std::size_t begin = i;
        ++i;
        while (i < length && (IsAsciiAlnum(text[i]) || text[i] == L'-'))
            ++i;

        std::size_t stop = i;
        while (stop > begin && text[stop - 1] == L'-')
            --stop;  // a label may not end in '-'
        const bool truncated = (stop != i);

        ++labels;
        lastBegin = begin;
        lastEnd   = stop;

        bool digitsOnly = true;
        for (std::size_t k = begin; k < stop; ++k) {
            if (!IsAsciiDigit(text[k])) {
                digitsOnly = false;
                break;
            }
        }
        if (!digitsOnly)
            numericOnly = false;
        if (!digitsOnly || (stop - begin) > 3)
            quadShaped = false;

        if (truncated) {
            i = stop;  // the trailing hyphen ends the host
            break;
        }
        if (i + 1 < length && text[i] == L'.' && IsAsciiAlnum(text[i + 1])) {
            ++i;  // dot joins another label
            continue;
        }
        break;
    }

    if (labels < 2)
        return false;

    const std::size_t lastLength = lastEnd - lastBegin;

    bool lastAlpha = (lastLength > 0);
    for (std::size_t k = lastBegin; k < lastEnd; ++k) {
        if (!IsAsciiAlpha(text[k])) {
            lastAlpha = false;
            break;
        }
    }

    out.end      = lastEnd;
    out.isDomain = lastAlpha && lastLength >= 2 && lastLength <= 24;
    out.isQuad   = numericOnly && quadShaped && labels == 4;
    return out.isDomain || out.isQuad;
}

// ---------------------------------------------------------------------------
// Individual rules. Each reports the exclusive end of the span it claims.
// ---------------------------------------------------------------------------

bool MatchWindowsPath(std::wstring_view text, std::size_t index, std::size_t& end) noexcept {
    const std::size_t length = text.size();
    if (index + 2 >= length)
        return false;

    bool isPath = false;
    if (IsAsciiAlpha(text[index]) && text[index + 1] == L':' &&
        (text[index + 2] == L'\\' || text[index + 2] == L'/')) {
        // "x://y" is a one-letter scheme, not a drive.
        isPath = !(text[index + 2] == L'/' && index + 3 < length && text[index + 3] == L'/');
    } else if (text[index] == L'\\' && text[index + 1] == L'\\' &&
               IsAsciiAlnum(text[index + 2])) {
        isPath = true;  // UNC
    }
    if (!isPath)
        return false;

    end = TrimTail(text, index, ScanPathBody(text, index));
    return end > index;
}

bool MatchSchemeUrl(std::wstring_view text, std::size_t index, std::size_t& end) noexcept {
    const std::size_t length = text.size();
    if (index >= length || !IsAsciiAlpha(text[index]))
        return false;

    std::size_t i = index + 1;
    while (i < length && (IsAsciiAlnum(text[i]) || text[i] == L'+' || text[i] == L'.' ||
                          text[i] == L'-'))
        ++i;
    if (i >= length || text[i] != L':')
        return false;

    std::size_t bodyBegin = 0;
    if (i + 2 < length && text[i + 1] == L'/' && text[i + 2] == L'/') {
        bodyBegin = i + 3;
    } else if (EqualsFoldAscii(text.substr(index, i - index), L"mailto")) {
        bodyBegin = i + 1;
    } else {
        return false;
    }

    const std::size_t bodyEnd = ScanUrlBody(text, bodyBegin);
    if (bodyEnd == bodyBegin)
        return false;  // a scheme with nothing behind it

    end = TrimTail(text, index, bodyEnd);
    return end > index;
}

bool MatchEmail(std::wstring_view text, std::size_t index, std::size_t& end) noexcept {
    const std::size_t length = text.size();

    std::size_t i = index;
    while (i < length && IsEmailLocalChar(text[i]))
        ++i;
    if (i == index || i >= length || text[i] != L'@')
        return false;

    Host host;
    if (!ScanHost(text, i + 1, host) || !host.isDomain)
        return false;

    end = host.end;
    return end > index;
}

bool MatchBareHost(std::wstring_view text, std::size_t index, std::size_t& end) noexcept {
    const std::size_t length = text.size();

    Host host;
    if (!ScanHost(text, index, host))
        return false;

    std::size_t i       = host.end;
    bool        hasPort = false;
    if (i < length && text[i] == L':') {
        std::size_t after  = i + 1;
        std::size_t digits = 0;
        while (after < length && IsAsciiDigit(text[after]) && digits < 5) {
            ++after;
            ++digits;
        }
        if (digits > 0 && (after >= length || !IsAsciiDigit(text[after]))) {
            i       = after;
            hasPort = true;
        }
    }

    const bool hasPath = (i < length && text[i] == L'/');
    if (hasPath)
        i = ScanUrlBody(text, i);

    // A dotted quad is only a host once something host-shaped follows it;
    // "1.2.3.4" on its own is as likely to be a version or a score.
    if (!host.isDomain && !(host.isQuad && (hasPort || hasPath)))
        return false;

    end = TrimTail(text, index, i);
    return end > index;
}

bool MatchHandle(std::wstring_view text, std::size_t index, std::size_t& end) noexcept {
    const std::size_t length = text.size();
    if (index >= length || text[index] != L'@')
        return false;

    std::size_t i = index + 1;
    while (i < length && IsHandleChar(text[i]))
        ++i;

    const std::size_t trimmed = TrimTail(text, index + 1, i);
    if (trimmed < index + 3)
        return false;  // "@a" is noise

    end = trimmed;
    return true;
}

bool MatchAt(std::wstring_view text, std::size_t index, std::size_t tokenEnd,
             bool tokenHasScheme, std::size_t& end) noexcept {
    if (MatchWindowsPath(text, index, end))
        return true;
    if (MatchSchemeUrl(text, index, end))
        return true;
    if (tokenHasScheme && ContainsSchemeSeparator(text, index, tokenEnd)) {
        end = TrimTail(text, index, tokenEnd);
        return end > index;
    }
    if (MatchEmail(text, index, end))
        return true;
    if (MatchBareHost(text, index, end))
        return true;
    return MatchHandle(text, index, end);
}

} // namespace

bool MarkInto(std::wstring_view input, std::vector<bool>& mask) {
    const std::size_t length = input.size();
    if (mask.size() < length)
        mask.resize(length, false);  // never shrink: other passes own their flags
    if (length == 0)
        return false;

    bool marked = false;

    // The "token contains ://" rule needs the whitespace-delimited token around
    // the cursor. Caching it keeps the scan linear instead of re-walking the
    // token at every candidate start inside it.
    std::size_t tokenEnd       = 0;
    bool        tokenHasScheme = false;

    std::size_t i = 0;
    while (i < length) {
        if (i >= tokenEnd) {
            if (IsSpaceOrControl(input[i])) {
                tokenEnd       = i + 1;
                tokenHasScheme = false;
            } else {
                std::size_t stop = i;
                while (stop < length && !IsSpaceOrControl(input[stop]))
                    ++stop;
                tokenEnd       = stop;
                tokenHasScheme = ContainsSchemeSeparator(input, i, stop);
            }
        }

        std::size_t end = i;
        if (IsLinkStart(input, i) && MatchAt(input, i, tokenEnd, tokenHasScheme, end) &&
            end > i) {
            for (std::size_t k = i; k < end; ++k)
                mask[k] = true;
            marked = true;
            i      = end;  // always makes progress: end > i
            continue;
        }
        ++i;
    }

    return marked;
}

} // namespace links
} // namespace uni
