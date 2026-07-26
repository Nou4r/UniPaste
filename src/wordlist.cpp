// UniPaste - word list storage and matching.
//
// Two independent lists live in this module, selected by wordlist::Kind. They
// share one file format, one parser and one matcher; only the storage slot and
// the file name differ:
//
//   Kind::Never - words and phrases that must survive the homoglyph conversion
//                 untouched. %APPDATA%\UniPaste\whitelist.txt.
//   Kind::Only  - in blacklist mode, the only words that ARE converted;
//                 everything else is left alone.
//                 %APPDATA%\UniPaste\blacklist.txt.
//
// The two are deliberately separate files rather than one list with a flag:
// silently inverting a curated never-convert list would convert exactly the
// words the user had protected.
//
// Both are persisted as hand-editable UTF-8 text (BOM included, so Notepad
// round-trips them) with '#' comment lines.
//
// Threading: every caller (the tray/hotkey WndProc and the settings dialog)
// runs on the single UI thread, so the module keeps its state in plain globals
// with no synchronisation. The low-level keyboard hook never touches it - it
// only PostMessage()s the trigger, and the conversion happens in WndProc.

#include "wordlist.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <climits>
#include <cstddef>

namespace uni {
namespace wordlist {
namespace {

// A corrupt or hostile file must not turn into a multi-gigabyte allocation.
// The realistic upper bound for a hand-maintained list is a few kilobytes.
constexpr DWORD kMaxFileBytes = 8u * 1024u * 1024u;

constexpr std::size_t kListCount = 2;

// One slot per Kind. `path` is filled on first use by FilePath().
struct List {
    std::vector<std::wstring> entries;
    std::wstring              path;
};

List g_lists[kListCount];

std::size_t IndexOf(Kind kind) noexcept {
    const std::size_t index = static_cast<std::size_t>(kind);
    return (index < kListCount) ? index : 0;  // never index out of the array
}

List& ListFor(Kind kind) noexcept {
    return g_lists[IndexOf(kind)];
}

// ---------------------------------------------------------------------------
// Character helpers (locale-independent, mirroring spoof.cpp's ASCII fold)
// ---------------------------------------------------------------------------

constexpr wchar_t AsciiLower(wchar_t c) noexcept {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

// ASCII whitespace only. Deliberately narrower than iswspace(): trimming must
// not depend on the caller's setlocale() state, and a non-breaking space is a
// word character for the boundary test below, so it must survive the trim
// rather than being silently eaten off the edge of an entry.
constexpr bool IsAsciiSpace(wchar_t c) noexcept {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'\f' || c == L'\r';
}

// Word character for the boundary test: ASCII letter, ASCII digit, '_', or any
// code unit >= 0x80. The last clause is the important one - an already
// converted homoglyph (U+0430 and friends) counts as part of a word, so an
// entry can never match the interior of a larger token.
constexpr bool IsWordChar(wchar_t c) noexcept {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
           (c >= L'0' && c <= L'9') || c == L'_' ||
           static_cast<unsigned int>(c) >= 0x80u;
}

std::wstring_view Trim(std::wstring_view text) noexcept {
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && IsAsciiSpace(text[begin]))
        ++begin;
    while (end > begin && IsAsciiSpace(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

bool EqualsFold(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (AsciiLower(a[i]) != AsciiLower(b[i]))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16
// ---------------------------------------------------------------------------

std::string ToUtf8(std::wstring_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX))
        return std::string();

    const int wideLen = static_cast<int>(text.size());
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLen, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return std::string();

    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLen, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const char* data, std::size_t size) {
    if (size == 0 || size > static_cast<std::size_t>(INT_MAX))
        return std::wstring();

    const int byteLen = static_cast<int>(size);
    const int chars   = MultiByteToWideChar(CP_UTF8, 0, data, byteLen, nullptr, 0);
    if (chars <= 0)
        return std::wstring();

    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, byteLen, out.data(), chars);
    return out;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

// The Never list keeps the original file name so existing users keep their
// list across the upgrade.
const wchar_t* FileNameFor(Kind kind) noexcept {
    return (kind == Kind::Only) ? L"blacklist.txt" : L"whitelist.txt";
}

std::wstring ComputeFilePath(const wchar_t* fileName) {
    wchar_t buffer[MAX_PATH] = {};
    const HRESULT hr =
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buffer);

    std::wstring path;
    if (SUCCEEDED(hr) && buffer[0] != L'\0') {
        path.assign(buffer);
        if (!path.empty() && path.back() != L'\\')
            path.push_back(L'\\');
    }
    path += L"UniPaste\\";
    path += fileName;
    return path;
}

// Directory portion of FilePath(), i.e. everything before the last backslash.
std::wstring DirectoryOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
}

// Each list writes its own header so the file explains its own role - the two
// have opposite meanings and end up side by side in the same directory.
void AppendHeader(Kind kind, std::wstring& text) {
    if (kind == Kind::Only) {
        text += L"# UniPaste convert-only list - one entry per line. In blacklist mode\r\n";
        text += L"# these are the ONLY words that get converted; everything else is\r\n";
        text += L"# left exactly as it was.\r\n";
    } else {
        text += L"# UniPaste whitelist - one entry per line; these are never converted.\r\n";
    }
    text += L"# Lines starting with '#' are comments. Matching is case-insensitive\r\n";
    text += L"# and requires a word boundary at both ends.\r\n";
}

// `missing` distinguishes "no file yet" (a normal first run) from a genuine
// I/O failure the caller has to surface.
bool ReadWholeFile(const std::wstring& path, std::string& bytes, bool& missing) {
    bytes.clear();
    missing = false;

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        missing = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
        return missing;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > static_cast<LONGLONG>(kMaxFileBytes)) {
        CloseHandle(file);
        return false;
    }

    const DWORD total = static_cast<DWORD>(size.QuadPart);
    bytes.resize(static_cast<std::size_t>(total));

    DWORD done = 0;
    while (done < total) {
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + done, total - done, &read, nullptr)) {
            CloseHandle(file);
            bytes.clear();
            return false;
        }
        if (read == 0)
            break;  // truncated under us; keep what we got
        done += read;
    }

    CloseHandle(file);
    bytes.resize(static_cast<std::size_t>(done));
    return true;
}

bool WriteWholeFile(const std::wstring& path, const std::string& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const std::size_t total = bytes.size();
    std::size_t done        = 0;
    while (done < total) {
        const std::size_t remaining = total - done;
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(remaining, 0x100000u));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + done, chunk, &written, nullptr) || written == 0) {
            CloseHandle(file);
            return false;
        }
        done += static_cast<std::size_t>(written);
    }

    CloseHandle(file);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// List management
// ---------------------------------------------------------------------------

const std::vector<std::wstring>& Entries(Kind kind) {
    return ListFor(kind).entries;
}

// Intentionally does NOT persist: the settings UI batches edits and calls
// Save() once, so a rejected duplicate never rewrites the file.
bool Add(Kind kind, std::wstring_view entry) {
    const std::wstring_view trimmed = Trim(entry);
    if (trimmed.empty())
        return false;

    std::vector<std::wstring>& entries = ListFor(kind).entries;
    for (const std::wstring& existing : entries) {
        if (EqualsFold(existing, trimmed))
            return false;
    }

    entries.emplace_back(trimmed);
    return true;
}

bool RemoveAt(Kind kind, size_t index) {
    std::vector<std::wstring>& entries = ListFor(kind).entries;
    if (index >= entries.size())
        return false;

    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

const std::wstring& FilePath(Kind kind) {
    List& list = ListFor(kind);
    if (list.path.empty())
        list.path = ComputeFilePath(FileNameFor(kind));
    return list.path;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

bool Load(Kind kind) {
    std::string bytes;
    bool missing = false;
    if (!ReadWholeFile(FilePath(kind), bytes, missing)) {
        return false;  // genuine read failure: keep whatever is in memory
    }

    ListFor(kind).entries.clear();
    if (missing || bytes.empty())
        return true;

    // Strip the UTF-8 BOM we write (and the one Notepad may add).
    const char* data  = bytes.data();
    std::size_t size  = bytes.size();
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data += 3;
        size -= 3;
    }

    const std::wstring text = FromUtf8(data, size);

    // Split on CR and LF alike so CRLF, LF and lone-CR files all round-trip.
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = start;
        while (end < text.size() && text[end] != L'\r' && text[end] != L'\n')
            ++end;

        const std::wstring_view line = Trim(std::wstring_view(text).substr(start, end - start));
        if (!line.empty() && line.front() != L'#')
            Add(kind, line);  // handles trimming and case-insensitive de-duplication

        if (end >= text.size())
            break;
        start = end + 1;
    }

    return true;
}

bool Save(Kind kind) {
    const std::wstring& path      = FilePath(kind);
    const std::wstring  directory = DirectoryOf(path);
    if (!directory.empty() && !CreateDirectoryW(directory.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
            return false;
    }

    std::wstring text;
    AppendHeader(kind, text);
    for (const std::wstring& entry : ListFor(kind).entries) {
        text += entry;
        text += L"\r\n";
    }

    std::string bytes("\xEF\xBB\xBF", 3);  // UTF-8 BOM, so Notepad round-trips it
    bytes += ToUtf8(text);
    return WriteWholeFile(path, bytes);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

void Mark(Kind kind, std::wstring_view input, std::vector<bool>& mask) {
    const std::vector<std::wstring>& entries = ListFor(kind).entries;

    const std::size_t length = input.size();
    if (entries.empty() || length == 0) {
        mask.clear();
        return;
    }

    // Longest-first so "New York" wins over a bare "New" at the same index.
    // Sorting a private index vector leaves the order seen through Entries()
    // (and therefore the settings list box and the file) untouched.
    std::vector<std::size_t> order;
    order.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].empty() && entries[i].size() <= length)
            order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&entries](std::size_t a, std::size_t b) {
        return entries[a].size() > entries[b].size();
    });
    if (order.empty()) {
        mask.clear();
        return;
    }

    // Allocated lazily: the common case is "nothing matched", and the contract
    // says that case must leave the mask empty.
    bool marked = false;

    std::size_t i = 0;
    while (i < length) {
        // The left boundary is entry-independent: when the previous code unit
        // is a word character no entry can start here at all.
        if (i > 0 && IsWordChar(input[i - 1])) {
            ++i;
            continue;
        }

        std::size_t matchedLength = 0;

        for (const std::size_t index : order) {
            const std::wstring& entry = entries[index];
            const std::size_t   count = entry.size();
            if (i + count > length)
                continue;

            // Right boundary: string edge, or a non-word character.
            if (i + count < length && IsWordChar(input[i + count]))
                continue;

            bool equal = true;
            for (std::size_t k = 0; k < count; ++k) {
                if (AsciiLower(input[i + k]) != AsciiLower(entry[k])) {
                    equal = false;
                    break;
                }
            }
            if (!equal)
                continue;

            matchedLength = count;
            break;
        }

        if (matchedLength == 0) {
            ++i;
            continue;
        }

        if (!marked) {
            mask.assign(length, false);
            marked = true;
        }
        for (std::size_t k = 0; k < matchedLength; ++k)
            mask[i + k] = true;

        i += matchedLength;  // never re-enter a protected span
    }

    if (!marked)
        mask.clear();
}

} // namespace wordlist
} // namespace uni
