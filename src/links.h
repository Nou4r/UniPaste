#pragma once
#include <string_view>
#include <vector>

namespace uni {
namespace links {

// ORs the spans of anything that must survive verbatim - URLs, bare domains,
// e-mail addresses, @handles, Windows drive paths and UNC paths - into `mask`.
// `mask` is resized to input.size() and existing true flags are preserved.
// Returns true when at least one span was marked.
bool MarkInto(std::wstring_view input, std::vector<bool>& mask);

} // namespace links
} // namespace uni
