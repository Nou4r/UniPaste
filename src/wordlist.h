#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace uni {
namespace wordlist {

// Two independent lists with identical semantics and file format.
//   Never - words that are never converted        (whitelist.txt)
//   Only  - the only words that ARE converted     (blacklist.txt)
// Matching is case-insensitive (ASCII fold) and word-boundary anchored at both
// ends; an entry may contain spaces to match a whole phrase.
enum class Kind { Never = 0, Only = 1 };

const std::vector<std::wstring>& Entries(Kind kind);

bool Add(Kind kind, std::wstring_view entry);   // false: empty after trim, or duplicate
bool RemoveAt(Kind kind, size_t index);

bool Load(Kind kind);                            // missing file == empty list == success
bool Save(Kind kind);
const std::wstring& FilePath(Kind kind);

// Marks spans covered by entries of `kind`. Leaves `mask` EMPTY when the list
// is empty or nothing matched.
void Mark(Kind kind, std::wstring_view input, std::vector<bool>& mask);

} // namespace wordlist
} // namespace uni
