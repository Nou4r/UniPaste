#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "spoof.h"

namespace settings {

using GetModeFn = uni::Mode (*)();
using SetModeFn = void (*)(uni::Mode);

bool Init(HINSTANCE hInst, GetModeFn getMode, SetModeFn setMode);
void Show();                    // create-or-activate the settings window
void NotifyModeChanged();       // mode changed elsewhere; refresh the UI
void Shutdown();
bool HandleDialogMessage(MSG* msg);  // main loop calls this before TranslateMessage

} // namespace settings
