// UniPaste - the one place that decides which code units are protected.
//
// Every conversion path (the paste hotkey, convert-on-copy, the settings
// preview) goes through BuildMask/Apply, so what the preview shows is exactly
// what a paste produces.
//
// The two list policies are expressed through the same protected-span mask, so
// uni::Convert never learns about them:
//
//   normal mode    protected = Never-list spans        OR link spans
//   blacklist mode protected = NOT(Only-list spans)    OR link spans
//
// The inversion is what makes "convert only these words" work without a second
// code path: everything the user did not list is protected. Link protection is
// ORed on top in both modes, so a listed word sitting inside a URL still
// survives.
//
// The mask handed to uni::Convert is always either empty (nothing protected -
// the converter's cheap path) or exactly input.size() flags long.

#include "policy.h"

#include <cstddef>

#include "links.h"
#include "wordlist.h"

namespace uni {
namespace policy {
namespace {

// uni::Convert reads an empty mask as "nothing protected", which is both faster
// and the only size other than input.size() it accepts. An all-false mask means
// the same thing, so collapse it.
void Normalise(std::size_t length, std::vector<bool>& mask) {
    if (mask.empty())
        return;

    if (mask.size() != length) {
        mask.clear();
        return;
    }

    for (std::size_t i = 0; i < length; ++i) {
        if (mask[i])
            return;
    }
    mask.clear();
}

} // namespace

void BuildMask(std::wstring_view input, const Options& opt, std::vector<bool>& mask) {
    mask.clear();

    const std::size_t length = input.size();
    if (length == 0)
        return;

    if (opt.blacklistMode) {
        // Mark() leaves `only` empty when the list is empty or nothing matched.
        // Inverting that is an all-true mask: nothing on the list, nothing to
        // convert. Silently converting everything instead would be the exact
        // opposite of what the user asked for.
        std::vector<bool> only;
        wordlist::Mark(wordlist::Kind::Only, input, only);

        mask.assign(length, true);
        if (!only.empty()) {
            for (std::size_t i = 0; i < length; ++i)
                mask[i] = !only[i];
        }
    } else {
        wordlist::Mark(wordlist::Kind::Never, input, mask);
    }

    if (opt.protectLinks)
        (void)links::MarkInto(input, mask);  // resizes and ORs; never clears

    Normalise(length, mask);
}

std::wstring Apply(std::wstring_view input, Mode mode, const Options& opt) {
    std::vector<bool> mask;
    BuildMask(input, opt, mask);
    return Convert(input, mode, mask);
}

} // namespace policy
} // namespace uni
