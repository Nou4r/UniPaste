#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace uni {

enum class Mode { Basic = 0, Advanced = 1, First = 2, Smart = 3 };

// `protectedSpans` is either empty (nothing protected) or exactly input.size()
// entries long; a true entry pins that code unit to its original value.
std::wstring Convert(std::wstring_view input, Mode mode,
                     const std::vector<bool>& protectedSpans);

const wchar_t* ModeName(Mode mode);

// Basic -> Advanced -> First -> Smart -> Basic.
Mode NextMode(Mode mode);

} // namespace uni
