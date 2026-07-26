#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "spoof.h"

namespace uni {
namespace policy {

struct Options {
    bool blacklistMode = false;   // convert ONLY the words on the Only list
    bool protectLinks  = true;    // never convert URLs, e-mails, @handles, paths
};

// The single place that decides which code units are protected.
//   normal   : protected = Never-list spans  OR link spans
//   blacklist: protected = NOT(Only-list spans)  OR link spans
void BuildMask(std::wstring_view input, const Options& opt, std::vector<bool>& mask);

// BuildMask followed by uni::Convert.
std::wstring Apply(std::wstring_view input, Mode mode, const Options& opt);

} // namespace policy
} // namespace uni
