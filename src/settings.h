#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "policy.h"
#include "spoof.h"

namespace settings {

struct Callbacks {
    uni::Mode           (*getMode)();
    void                (*setMode)(uni::Mode);
    uni::policy::Options(*getOptions)();
    void                (*setOptions)(const uni::policy::Options&);
    bool                (*getAutoConvert)();
    void                (*setAutoConvert)(bool);
};

bool Init(HINSTANCE hInst, const Callbacks& cb);
void Show();
void NotifyStateChanged();      // mode/options/auto-convert changed elsewhere
void Shutdown();
bool HandleDialogMessage(MSG* msg);

} // namespace settings
