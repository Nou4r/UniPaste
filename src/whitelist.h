#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace uni {
namespace whitelist {

// Entries are never converted. Matching is case-insensitive (ASCII fold) and
// requires a word boundary at both ends, so "cat" does not protect
// "concatenate". An entry may contain spaces to protect a whole phrase.

const std::vector<std::wstring>& Entries();

bool Add(std::wstring_view entry);   // false: empty after trim, or duplicate
bool RemoveAt(size_t index);

bool Load();                          // missing file == empty list == success
bool Save();
const std::wstring& FilePath();       // %APPDATA%\UniPaste\whitelist.txt

// Fills `mask` with input.size() flags (true = protected). Clears `mask` to
// empty when the whitelist is empty or nothing matched.
void Mark(std::wstring_view input, std::vector<bool>& mask);

} // namespace whitelist
} // namespace uni
