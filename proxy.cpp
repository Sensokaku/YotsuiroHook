#include "proxy.h"

static HMODULE g_hReal = nullptr;

static const char* g_exportNames[] = {
    MAKEINTRESOURCEA(2),        // 0 - ordinal 2
    "CloseDriver",              // 1
    "DefDriverProc",            // 2
    "DriverCallback",           // 3
    "DrvGetModuleHandle",       // 4
    "GetDriverModuleHandle",    // 5
    "OpenDriver",               // 6
    "PlaySound",                // 7
    "PlaySoundA",               // 8
    "PlaySoundW",               // 9
    "SendDriverMessage",        // 10
    "WOWAppExit",               // 11
    "auxGetDevCapsA",           // 12
    "auxGetDevCapsW",           // 13
    "auxGetNumDevs",            // 14
    "auxGetVolume",             // 15
    "auxOutMessage",            // 16
    "auxSetVolume",             // 17
    "joyConfigChanged",         // 18
    "joyGetDevCapsA",           // 19
    "joyGetDevCapsW",           // 20
    "joyGetNumDevs",            // 21
    "joyGetPos",                // 22
    "joyGetPosEx",              // 23
    "joyGetThreshold",          // 24
    "joyReleaseCapture",        // 25
    "joySetCapture",            // 26
    "joySetThreshold",          // 27
    "mciDriverNotify",          // 28
    "mciDriverYield",           // 29
    "mciExecute",               // 30
    "mciFreeCommandResource",   // 31
    "mciGetCreatorTask",        // 32
    "mciGetDeviceIDA",          // 33
    "mciGetDeviceIDFromElementIDA", // 34
    "mciGetDeviceIDFromElementIDW", // 35
    "mciGetDeviceIDW",          // 36
    "mciGetDriverData",         // 37
    "mciGetErrorStringA",       // 38
    "mciGetErrorStringW",       // 39
    "mciGetYieldProc",          // 40
    "mciLoadCommandResource",   // 41
    "mciSendCommandA",          // 42
    "mciSendCommandW",          // 43
    "mciSendStringA",           // 44
    "mciSendStringW",           // 45
    "mciSetDriverData",         // 46
    "mciSetYieldProc",          // 47
    "midiConnect",              // 48
    "midiDisconnect",           // 49
    "midiInAddBuffer",          // 50
    "midiInClose",              // 51
    "midiInGetDevCapsA",        // 52
    "midiInGetDevCapsW",        // 53
    "midiInGetErrorTextA",      // 54
    "midiInGetErrorTextW",      // 55
    "midiInGetID",              // 56
    "midiInGetNumDevs",         // 57
    "midiInMessage",            // 58
    "midiInOpen",               // 59
    "midiInPrepareHeader",      // 60
    "midiInReset",              // 61
    "midiInStart",              // 62
    "midiInStop",               // 63
    "midiInUnprepareHeader",    // 64
    "midiOutCacheDrumPatches",  // 65
    "midiOutCachePatches",      // 66
    "midiOutClose",             // 67
    "midiOutGetDevCapsA",       // 68
    "midiOutGetDevCapsW",       // 69
    "midiOutGetErrorTextA",     // 70
    "midiOutGetErrorTextW",     // 71
    "midiOutGetID",             // 72
    "midiOutGetNumDevs",        // 73
    "midiOutGetVolume",         // 74
    "midiOutLongMsg",           // 75
    "midiOutMessage",           // 76
    "midiOutOpen",              // 77
    "midiOutPrepareHeader",     // 78
    "midiOutReset",             // 79
    "midiOutSetVolume",         // 80
    "midiOutShortMsg",          // 81
    "midiOutUnprepareHeader",   // 82
    "midiStreamClose",          // 83
    "midiStreamOpen",           // 84
    "midiStreamOut",            // 85
    "midiStreamPause",          // 86
    "midiStreamPosition",       // 87
    "midiStreamProperty",       // 88
    "midiStreamRestart",        // 89
    "midiStreamStop",           // 90
    "mixerClose",               // 91
    "mixerGetControlDetailsA",  // 92
    "mixerGetControlDetailsW",  // 93
    "mixerGetDevCapsA",         // 94
    "mixerGetDevCapsW",         // 95
    "mixerGetID",               // 96
    "mixerGetLineControlsA",    // 97
    "mixerGetLineControlsW",    // 98
    "mixerGetLineInfoA",        // 99
    "mixerGetLineInfoW",        // 100
    "mixerGetNumDevs",          // 101
    "mixerMessage",             // 102
    "mixerOpen",                // 103
    "mixerSetControlDetails",   // 104
    "mmDrvInstall",             // 105
    "mmGetCurrentTask",         // 106
    "mmTaskBlock",              // 107
    "mmTaskCreate",             // 108
    "mmTaskSignal",             // 109
    "mmTaskYield",              // 110
    "mmioAdvance",              // 111
    "mmioAscend",               // 112
    "mmioClose",                // 113
    "mmioCreateChunk",          // 114
    "mmioDescend",              // 115
    "mmioFlush",                // 116
    "mmioGetInfo",              // 117
    "mmioInstallIOProcA",       // 118
    "mmioInstallIOProcW",       // 119
    "mmioOpenA",                // 120
    "mmioOpenW",                // 121
    "mmioRead",                 // 122
    "mmioRenameA",              // 123
    "mmioRenameW",              // 124
    "mmioSeek",                 // 125
    "mmioSendMessage",          // 126
    "mmioSetBuffer",            // 127
    "mmioSetInfo",              // 128
    "mmioStringToFOURCCA",      // 129
    "mmioStringToFOURCCW",      // 130
    "mmioWrite",                // 131
    "mmsystemGetVersion",       // 132
    "sndPlaySoundA",            // 133
    "sndPlaySoundW",            // 134
    "timeBeginPeriod",          // 135
    "timeEndPeriod",            // 136
    "timeGetDevCaps",           // 137
    "timeGetSystemTime",        // 138
    "timeGetTime",              // 139
    "timeKillEvent",            // 140
    "timeSetEvent",             // 141
    "waveInAddBuffer",          // 142
    "waveInClose",              // 143
    "waveInGetDevCapsA",        // 144
    "waveInGetDevCapsW",        // 145
    "waveInGetErrorTextA",      // 146
    "waveInGetErrorTextW",      // 147
    "waveInGetID",              // 148
    "waveInGetNumDevs",         // 149
    "waveInGetPosition",        // 150
    "waveInMessage",            // 151
    "waveInOpen",               // 152
    "waveInPrepareHeader",      // 153
    "waveInReset",              // 154
    "waveInStart",              // 155
    "waveInStop",               // 156
    "waveInUnprepareHeader",    // 157
    "waveOutBreakLoop",         // 158
    "waveOutClose",             // 159
    "waveOutGetDevCapsA",       // 160
    "waveOutGetDevCapsW",       // 161
    "waveOutGetErrorTextA",     // 162
    "waveOutGetErrorTextW",     // 163
    "waveOutGetID",             // 164
    "waveOutGetNumDevs",        // 165
    "waveOutGetPitch",          // 166
    "waveOutGetPlaybackRate",   // 167
    "waveOutGetPosition",       // 168
    "waveOutGetVolume",         // 169
    "waveOutMessage",           // 170
    "waveOutOpen",              // 171
    "waveOutPause",             // 172
    "waveOutPrepareHeader",     // 173
    "waveOutReset",             // 174
    "waveOutRestart",           // 175
    "waveOutSetPitch",          // 176
    "waveOutSetPlaybackRate",   // 177
    "waveOutSetVolume",         // 178
    "waveOutUnprepareHeader",   // 179
    "waveOutWrite",             // 180
    // x86 only
    "NotifyCallbackData",       // 181
    "WOW32DriverCallback",      // 182
    "WOW32ResolveMultiMediaHandle", // 183
    "aux32Message",             // 184
    "joy32Message",             // 185
    "mci32Message",             // 186
    "mid32Message",             // 187
    "mod32Message",             // 188
    "mxd32Message",             // 189
    "tid32Message",             // 190
    "wid32Message",             // 191
    "wod32Message",             // 192
};

static FARPROC g_procs[_countof(g_exportNames)] = {};

BOOL proxy_init() {
    if (g_hReal) return TRUE;

    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\winmm.dll");
    g_hReal = LoadLibraryA(path);
    return g_hReal != nullptr;
}

void proxy_free() {
    if (g_hReal) {
        FreeLibrary(g_hReal);
        g_hReal = nullptr;
    }
}

FARPROC __stdcall resolve_export(size_t index) {
    if (index >= _countof(g_exportNames)) return nullptr;
    if (g_procs[index]) return g_procs[index];
    if (!g_hReal && !proxy_init()) return nullptr;
    g_procs[index] = GetProcAddress(g_hReal, g_exportNames[index]);
    return g_procs[index];
}
