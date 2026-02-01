#pragma once
#include <Windows.h>

extern "C" {
    BOOL proxy_init();
    void proxy_free();
    FARPROC __stdcall resolve_export(size_t index);
}
