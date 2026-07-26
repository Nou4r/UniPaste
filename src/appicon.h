// UniPaste application icon, generated at runtime - there is no .ico resource.
// One entry point: ask for a pixel size, get an HICON back.

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace appicon {

// Cached per pixel size; the returned HICON is owned by this module.
HICON Get(int px);
void  Shutdown();

} // namespace appicon
