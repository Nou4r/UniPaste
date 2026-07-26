#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace overlay {

enum class Kind { Success, Error };

// Creates the (hidden) layered toast window. Call once from the UI thread that
// pumps the message loop. Returns false on failure.
bool Init(HINSTANCE hInst);

// Shows/refreshes the toast with `text`. Non-blocking, never steals focus.
// Must be safe to call repeatedly while a toast is already on screen (restarts it).
void Show(const wchar_t* text, Kind kind = Kind::Success);

// Destroys the window and unregisters the class.
void Shutdown();

} // namespace overlay
