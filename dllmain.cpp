#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <Psapi.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <fstream>
#include <mutex>
#include <vector>
#include <functional>
#include <atomic>
#include <MinHook.h>

#pragma comment(lib, "psapi.lib")

//=============================================================================
// Configuration
//=============================================================================
namespace Config {
    const char* TRANSLATION_FILE = ".\\translation.tsv";
    const char* NAMES_FILE = ".\\unique_names.tsv";  // Global names fallback
    const char* UNTRANSLATED_LOG = ".\\untranslated.tsv";

    bool enableConsole = true;
    bool enableTextLogging = true;
    bool dumpUntranslated = false;

    int reloadHotkey = VK_F5;
}

//=============================================================================
// Logging
//=============================================================================
static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;

static void InitConsole() {
    if (!Config::enableConsole) return;
    AllocConsole();
    SetConsoleTitleW(L"よついろ★パッショナート！ - Translation Hook");
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    freopen_s(&g_logFile, "CONOUT$", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);
}

static void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fflush(g_logFile);
}

//=============================================================================
// Encoding Detection & Conversion
//=============================================================================
namespace Encoding {
    enum class Type {
        Unknown,
        UTF8_BOM,
        UTF8,
        ShiftJIS
    };

    // Detect encoding of a buffer
    Type Detect(const char* data, size_t size) {
        if (size == 0) return Type::Unknown;

        // Check for UTF-8 BOM
        if (size >= 3 &&
            (unsigned char)data[0] == 0xEF &&
            (unsigned char)data[1] == 0xBB &&
            (unsigned char)data[2] == 0xBF) {
            return Type::UTF8_BOM;
        }

        // Try to detect UTF-8 by looking for valid multi-byte sequences
        int utf8Score = 0;
        int sjisScore = 0;

        for (size_t i = 0; i < size && i < 1000; i++) {
            unsigned char c = (unsigned char)data[i];

            // UTF-8 multi-byte detection
            if (c >= 0xC0 && c <= 0xDF && i + 1 < size) {
                unsigned char c2 = (unsigned char)data[i + 1];
                if (c2 >= 0x80 && c2 <= 0xBF) {
                    utf8Score += 2;
                    i++;continue;
                }
            }
            if (c >= 0xE0 && c <= 0xEF && i + 2 < size) {
                unsigned char c2 = (unsigned char)data[i + 1];
                unsigned char c3 = (unsigned char)data[i + 2];
                if (c2 >= 0x80 && c2 <= 0xBF && c3 >= 0x80 && c3 <= 0xBF) {
                    utf8Score += 3;
                    i += 2;
                    continue;
                }
            }

            // Shift-JIS detection (common Japanese ranges)
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
                if (i + 1 < size) {
                    unsigned char c2 = (unsigned char)data[i + 1];
                    if ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0x80 && c2 <= 0xFC)) {
                        sjisScore += 2;
                        i++;
                        continue;
                    }
                }
            }
        }

        if (utf8Score > sjisScore * 2) return Type::UTF8;
        if (sjisScore > 0) return Type::ShiftJIS;
        return Type::UTF8;// Default to UTF-8 for ASCII-only
    }

    // Shift-JIS -> UTF-8
    std::string SjisToUtf8(const char* sjis) {
        if (!sjis || !*sjis) return "";

        int wideLen = MultiByteToWideChar(932, 0, sjis, -1, nullptr, 0);
        if (wideLen <= 0) return "";

        std::wstring wide(wideLen, 0);
        MultiByteToWideChar(932, 0, sjis, -1, &wide[0], wideLen);

        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len <= 0) return "";

        std::string utf8(utf8Len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], utf8Len, nullptr, nullptr);

        if (!utf8.empty() && utf8.back() == 0) utf8.pop_back();
        return utf8;
    }

    // UTF-8 -> Shift-JIS
    std::string Utf8ToSjis(const char* utf8) {
        if (!utf8 || !*utf8) return "";

        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (wideLen <= 0) return "";

        std::wstring wide(wideLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], wideLen);

        int sjisLen = WideCharToMultiByte(932, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (sjisLen <= 0) return "";

        std::string sjis(sjisLen, 0);
        WideCharToMultiByte(932, 0, wide.c_str(), -1, &sjis[0], sjisLen, nullptr, nullptr);

        if (!sjis.empty() && sjis.back() == 0) sjis.pop_back();
        return sjis;
    }

    // Convert any encoding to UTF-8
    std::string ToUtf8(const std::string& data, Type encoding) {
        switch (encoding) {
        case Type::UTF8_BOM:
            return data.size() >= 3 ? data.substr(3) : data;
        case Type::UTF8:
            return data;
        case Type::ShiftJIS:
            return SjisToUtf8(data.c_str());
        default:
            return data;
        }
    }
}

//=============================================================================
// Function Offsets (ImageBase 0x10000000)
//=============================================================================
namespace Offsets {
    constexpr uintptr_t AdvCharSay = 0x00039A10;
    constexpr uintptr_t PrintEx    = 0x00049A50;
    constexpr uintptr_t LoadRld    = 0x000C1010;
    constexpr uintptr_t Decrypt    = 0x00192030;
}

//=============================================================================
// Translation Database
//=============================================================================
class TranslationDB {
public:
    bool Load(const char* tsvPath, const char* namesPath) {
        std::lock_guard<std::mutex> lock(m_dataMutex);

        m_names.clear();
        m_contextualNames.clear();
        m_messages.clear();
        m_originalNamesByIndex.clear();

        int globalCount = 0;
        int contextualCount = 0;
        int textCount = 0;

        // === STEP 1: Load global names from unique_names.tsv ===
        if (namesPath) {
            globalCount = LoadGlobalNames(namesPath);
        }

        // === STEP 2: Load translation.tsv (overrides global) ===
        std::ifstream file(tsvPath, std::ios::binary | std::ios::ate);
        if (!file) {
            Log("[TL] Cannot open: %s\n", tsvPath);
            return globalCount > 0;  // Still OK if we loaded names
        }

        size_t fileSize = (size_t)file.tellg();
        file.seekg(0);

        std::string content(fileSize, 0);
        file.read(&content[0], fileSize);Encoding::Type encoding = Encoding::Detect(content.c_str(), content.size());
        std::string utf8Content = Encoding::ToUtf8(content, encoding);

        // First pass: collect entries by index
        std::map<std::pair<std::string, int>, std::string> namesByIndex;
        std::map<std::pair<std::string, int>, std::string> textsByIndex;

        std::istringstream iss(utf8Content);
        std::string line;

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            std::vector<std::string> parts;
            size_t start = 0, end;
            while ((end = line.find('\t', start)) != std::string::npos) {
                parts.push_back(line.substr(start, end - start));
                start = end + 1;
            }
            parts.push_back(line.substr(start));

            if (parts.size() < 5|| parts[4].empty()) continue;

            std::string fileId = parts[0];
            int index = std::atoi(parts[1].c_str());
            std::string type = parts[2];
            std::string original = parts[3];
            std::string translated = parts[4];

            UnescapeString(original);
            UnescapeString(translated);

            auto key = std::make_pair(fileId, index);

            if (type == "NAME") {
                namesByIndex[key] = translated;
                m_originalNamesByIndex[key] = original;
            } else if (type == "TEXT" || type == "MSG") {
                textsByIndex[key] = original;
                m_messages[original] = translated;
                textCount++;
            }
        }

        // Second pass: build contextual names (these OVERRIDE global)
        for (auto& [key, translatedName] : namesByIndex) {
            std::string originalName = m_originalNamesByIndex[key];

            auto textIt = textsByIndex.find(key);
            if (textIt != textsByIndex.end()) {
                // Contextual: "name|message" -> translation
                std::string contextKey = originalName + "|" + textIt->second;
                m_contextualNames[contextKey] = translatedName;
                contextualCount++;
            } else {
                // No text at same index - override global
                m_names[originalName] = translatedName;
                // Don't increment globalCount - it was loaded from unique_names
            }
        }

        const char* encodingName = "Unknown";
        switch (encoding) {
            case Encoding::Type::UTF8_BOM: encodingName = "UTF-8 (BOM)"; break;
            case Encoding::Type::UTF8: encodingName = "UTF-8"; break;
            case Encoding::Type::ShiftJIS: encodingName = "Shift-JIS"; break;
            default: break;
        }

        Log("[TL] Loaded (%s):\n", encodingName);
        Log("[TL]   %d global names (from %s)\n", globalCount, namesPath ? namesPath : "none");
        Log("[TL]   %d contextual names (from %s)\n", contextualCount, tsvPath);
        Log("[TL]   %d texts\n", textCount);

        return true;
    }

    void Reload() {
        Log("[TL] Reloading...\n");
        Load(Config::TRANSLATION_FILE, Config::NAMES_FILE);
    }

    // Context-aware name lookup
    const std::string* FindNameTranslation(const char* sjisName, const char* sjisMessage) {
        if (!sjisName || !*sjisName) return nullptr;

        std::string utf8Name = Encoding::SjisToUtf8(sjisName);
        if (utf8Name.empty()) return nullptr;

        std::lock_guard<std::mutex> lock(m_dataMutex);

        // Try contextual lookup first (name + message)
        if (sjisMessage && *sjisMessage) {
            std::string utf8Msg = Encoding::SjisToUtf8(sjisMessage);
            std::string contextKey = utf8Name + "|" + utf8Msg;

            auto it = m_contextualNames.find(contextKey);
            if (it != m_contextualNames.end()) {
                return &it->second;
            }
        }

        // Fall back to global name lookup
        auto it = m_names.find(utf8Name);
        if (it != m_names.end()) {
            return &it->second;
        }

        LogMissing(utf8Name.c_str(), "NAME");
        return nullptr;
    }

    // Message lookup (unchanged)
    const std::string* FindMessageTranslation(const char* sjisMessage) {
        if (!sjisMessage || !*sjisMessage) return nullptr;

        std::string utf8Key = Encoding::SjisToUtf8(sjisMessage);
        if (utf8Key.empty()) return nullptr;

        std::lock_guard<std::mutex> lock(m_dataMutex);

        auto it = m_messages.find(utf8Key);
        if (it != m_messages.end()) {
            return &it->second;
        }

        LogMissing(utf8Key.c_str(), "TEXT");
        return nullptr;
    }

private:
    void UnescapeString(std::string& s) {
        size_t pos = 0;
        while ((pos = s.find("\\n", pos)) != std::string::npos) {
            s.replace(pos, 2, "\n");
            pos += 1;
        }
        pos = 0;
        while ((pos = s.find("\\t", pos)) != std::string::npos) {
            s.replace(pos, 2, "\t");
            pos += 1;
        }
    }

    void LogMissing(const char* utf8Text, const char* type) {
        if (!Config::dumpUntranslated) return;

        std::lock_guard<std::mutex> lock(m_logMutex);

        std::string key(utf8Text);
        if (m_logged.count(key)) return;
        m_logged.insert(key);

        std::ofstream file(Config::UNTRANSLATED_LOG, std::ios::app | std::ios::binary);
        if (file) {
            std::string escaped = key;
            size_t pos = 0;
            while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                escaped.replace(pos, 1, "\\n");
                pos += 2;
            }
            file << "RUNTIME\t0\t" << type << "\t" << escaped << "\t\r\n";
        }
    }

    int LoadGlobalNames(const char* namesPath) {
        std::ifstream file(namesPath, std::ios::binary | std::ios::ate);
        if (!file) {
            Log("[TL] No global names file: %s\n", namesPath);
            return 0;
        }

        size_t fileSize = (size_t)file.tellg();
        file.seekg(0);

        std::string content(fileSize, 0);
        file.read(&content[0], fileSize);Encoding::Type encoding = Encoding::Detect(content.c_str(), content.size());
        std::string utf8Content = Encoding::ToUtf8(content, encoding);

        int count = 0;
        std::istringstream iss(utf8Content);
        std::string line;

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            // Skip header line
            if (line.find("ORIGINAL") == 0) continue;

            // Split by tab
            size_t tabPos = line.find('\t');
            if (tabPos == std::string::npos) continue;

            std::string original = line.substr(0, tabPos);
            std::string translated = line.substr(tabPos + 1);

            // Remove trailing tab/count if present
            size_t tabPos2 = translated.find('\t');
            if (tabPos2 != std::string::npos) {
                translated = translated.substr(0, tabPos2);
            }

            // Trim whitespace
            while (!translated.empty() && (translated.back() == ' ' || translated.back() == '\t')) {
                translated.pop_back();
            }

            // Skip if empty (user hasn't filled it in yet)
            if (original.empty() || translated.empty()) continue;

            UnescapeString(original);
            UnescapeString(translated);

            m_names[original] = translated;
            count++;
        }

        return count;
    }

    // Storage
    std::unordered_map<std::string, std::string> m_contextualNames;  // "name|message" -> translated_name
    std::unordered_map<std::string, std::string> m_names;            // name -> translated (fallback)
    std::unordered_map<std::string, std::string> m_messages;         // message -> translated
    std::map<std::pair<std::string, int>, std::string> m_originalNamesByIndex;
    std::unordered_set<std::string> m_logged;
    std::mutex m_dataMutex;
    std::mutex m_logMutex;
};

static TranslationDB g_translationDB;

//=============================================================================
// String Pool
//=============================================================================
class StringPool {
public:
    const char* Store(const std::string& str) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pool.find(str);
        if (it != m_pool.end()) return it->second.c_str();
        m_pool[str] = str;
        return m_pool[str].c_str();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pool.clear();
    }

private:
    std::unordered_map<std::string, std::string> m_pool;
    std::mutex m_mutex;
};

static StringPool g_stringPool;

//=============================================================================
// Hot Reload Thread
//=============================================================================
static std::atomic<bool> g_running{true};
static HANDLE g_hotkeyThread = nullptr;

static DWORD WINAPI HotkeyThreadProc(LPVOID) {
    Log("[*] Hot reload enabled: Press F5 to reload translations\n");

    while (g_running) {
        // Check hotkey (F5)
        if (GetAsyncKeyState(Config::reloadHotkey) & 0x8000) {
            // Wait for key release
            while (GetAsyncKeyState(Config::reloadHotkey) & 0x8000) {
                Sleep(10);
            }

            // Reload translations
            g_stringPool.Clear();
            g_translationDB.Reload();

            // Beep to confirm
            MessageBeep(MB_OK);
        }

        Sleep(50);
    }

    return 0;
}

//=============================================================================
// Hook: RetouchAdvCharacter::say
//=============================================================================
typedef void(__thiscall* Fn_AdvCharSay)(
    void* pThis, int voiceId, const char* name, const char* message,
    bool flag, int flags, int p1, int p2, int p3, void* printParam
);
static Fn_AdvCharSay g_origAdvCharSay = nullptr;

static void __fastcall AdvCharSay_Hook(
    void* pThis, void* edx,
    int voiceId, const char* name, const char* message,
    bool flag, int flags, int p1, int p2, int p3, void* printParam)
{
    const char* finalName = name;
    const char* finalMsg = message;

    // Translate name
    if (name && *name) {
        const std::string* tlUtf8 = g_translationDB.FindNameTranslation(name, message);
        if (tlUtf8) {
            std::string sjis = Encoding::Utf8ToSjis(tlUtf8->c_str());
            if (!sjis.empty()) {
                finalName = g_stringPool.Store(sjis);
            }
        }
    }

    // Translate message
    if (message && *message) {
        const std::string* tlUtf8 = g_translationDB.FindMessageTranslation(message);
        if (tlUtf8) {
            std::string sjis = Encoding::Utf8ToSjis(tlUtf8->c_str());
            if (!sjis.empty()) {
                finalMsg = g_stringPool.Store(sjis);
            }
        }
    }

    if (Config::enableTextLogging) {
        std::string nameUtf8 = name ? Encoding::SjisToUtf8(name) : "";
        std::string msgUtf8 = message ? Encoding::SjisToUtf8(message) : "";

        if (finalName != name || finalMsg != message) {
            std::string tlNameUtf8 = finalName ? Encoding::SjisToUtf8(finalName) : "";
            std::string tlMsgUtf8 = finalMsg ? Encoding::SjisToUtf8(finalMsg) : "";
            Log("[SAY] %s: %s\n", nameUtf8.c_str(), msgUtf8.c_str());Log("  -> %s: %s\n", tlNameUtf8.c_str(), tlMsgUtf8.c_str());
        } else {
            Log("[SAY] %s: %s\n", nameUtf8.c_str(), msgUtf8.c_str());
        }
    }

    g_origAdvCharSay(pThis, voiceId, finalName, finalMsg, flag, flags, p1, p2, p3, printParam);
}

//=============================================================================
// Hook: printEx
//=============================================================================
typedef void(__thiscall* Fn_PrintEx)(
    void* pThis, int charId, int msgId, const char* name, const char* message,
    unsigned long flags, unsigned long linkData
);
static Fn_PrintEx g_origPrintEx = nullptr;

static void __fastcall PrintEx_Hook(
    void* pThis, void* edx,
    int charId, int msgId, const char* name, const char* message,
    unsigned long flags, unsigned long linkData)
{
    const char* finalMsg = message;

    if (message && *message) {
        const std::string* tlUtf8 = g_translationDB.FindMessageTranslation(message);
        if (tlUtf8) {
            std::string sjis = Encoding::Utf8ToSjis(tlUtf8->c_str());
            if (!sjis.empty()) {
                finalMsg = g_stringPool.Store(sjis);
            }
        }
    }

    g_origPrintEx(pThis, charId, msgId, name, finalMsg, flags, linkData);
}

//=============================================================================
// File Watcher
//=============================================================================
class FileWatcher {
public:
    void Start(const char* directory, const std::vector<std::string>& watchFiles, std::function<void()> onChange) {
        m_watchFiles = watchFiles;
        m_onChange = onChange;
        m_running = true;

        // Get full directory path
        char fullPath[MAX_PATH];
        GetFullPathNameA(directory, MAX_PATH, fullPath, nullptr);
        m_directory = fullPath;

        m_lastWriteTime = GetLatestModTime();
        m_thread = CreateThread(nullptr, 0, WatchThreadProc, this, 0, nullptr);
    }

    void Stop() {
        m_running = false;
        if (m_stopEvent) {
            SetEvent(m_stopEvent);
        }
        if (m_thread) {
            WaitForSingleObject(m_thread, 2000);
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
        if (m_stopEvent) {
            CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }
    }

private:
    static DWORD WINAPI WatchThreadProc(LPVOID param) {
        return ((FileWatcher*)param)->WatchThread();
    }

    DWORD WatchThread() {
        m_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        HANDLE hDir = CreateFileA(
            m_directory.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (hDir == INVALID_HANDLE_VALUE) {
            Log("[FileWatcher] Failed to open directory: %s\n", m_directory.c_str());
            return 1;
        }

        Log("[FileWatcher] Watching directory: %s\n", m_directory.c_str());
        for (auto& f : m_watchFiles) {
            Log("[FileWatcher]   - %s\n", f.c_str());
        }

        char buffer[4096];
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        HANDLE waitHandles[2] = { overlapped.hEvent, m_stopEvent };

        while (m_running) {
            DWORD bytesReturned = 0;
            ResetEvent(overlapped.hEvent);

            BOOL result = ReadDirectoryChangesW(
                hDir, buffer, sizeof(buffer), FALSE,
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                &bytesReturned, &overlapped, nullptr
            );

            if (!result && GetLastError() != ERROR_IO_PENDING) break;

            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0) {
                if (GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE)) {
                    FILE_NOTIFY_INFORMATION* info = (FILE_NOTIFY_INFORMATION*)buffer;

                    bool shouldReload = false;do {
                        std::wstring changedFileW(info->FileName, info->FileNameLength / sizeof(WCHAR));
                        char changedFile[MAX_PATH];
                        WideCharToMultiByte(CP_ACP, 0, changedFileW.c_str(), -1, changedFile, MAX_PATH, nullptr, nullptr);

                        // Check if it's one of our watched files
                        for (auto& watchFile : m_watchFiles) {
                            if (_stricmp(changedFile, watchFile.c_str()) == 0) {
                                shouldReload = true;
                                Log("[FileWatcher] %s changed\n", changedFile);
                                break;
                            }
                        }

                        if (info->NextEntryOffset == 0) break;
                        info = (FILE_NOTIFY_INFORMATION*)((char*)info + info->NextEntryOffset);
                    } while (true);

                    if (shouldReload) {
                        Sleep(100);  // Debounce
                        FILETIME newTime = GetLatestModTime();
                        if (CompareFileTime(&newTime, &m_lastWriteTime) != 0) {
                            m_lastWriteTime = newTime;
                            if (m_onChange) {
                                m_onChange();
                            }
                        }
                    }
                }
            } else if (waitResult == WAIT_OBJECT_0 + 1) {
                break;
            }
        }

        CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
        return 0;
    }

    FILETIME GetLatestModTime() {
        FILETIME latest = {};
        for (auto& filename : m_watchFiles) {
            std::string fullPath = m_directory + "\\" + filename;
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (GetFileAttributesExA(fullPath.c_str(), GetFileExInfoStandard, &data)) {
                if (CompareFileTime(&data.ftLastWriteTime, &latest) > 0) {
                    latest = data.ftLastWriteTime;
                }
            }
        }
        return latest;
    }

    std::vector<std::string> m_watchFiles;
    std::string m_directory;
    std::function<void()> m_onChange;
    std::atomic<bool> m_running{false};
    HANDLE m_thread = nullptr;
    HANDLE m_stopEvent = nullptr;
    FILETIME m_lastWriteTime = {};
};

static FileWatcher g_fileWatcher;

//=============================================================================
// Hook Installation
//=============================================================================
typedef HMODULE(WINAPI* Fn_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);
static Fn_LoadLibraryExA g_origLoadLibraryExA = nullptr;

static bool InstallHooks(HMODULE hResident) {
    uintptr_t base = (uintptr_t)hResident;
    Log("[*] resident.dll base: 0x%p\n", (void*)base);

    // Hook say()
    {
        uintptr_t addr = base + Offsets::AdvCharSay;
        if (MH_CreateHook((void*)addr, (void*)&AdvCharSay_Hook, (void**)&g_origAdvCharSay) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] say() hooked\n");
        } else {
            Log("[!] Failed to hook say()\n");
        }
    }

    // Hook printEx()
    {
        uintptr_t addr = base + Offsets::PrintEx;
        if (MH_CreateHook((void*)addr, (void*)&PrintEx_Hook, (void**)&g_origPrintEx) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] printEx() hooked\n");
        } else {
            Log("[!] Failed to hook printEx()\n");
        }
    }

    Log("\n========================================\n");
    Log("Translation Hook Active!\n");
    Log("Press F5 to reload translations\n");
    Log("========================================\n\n");

    return true;
}

static HMODULE WINAPI LoadLibraryExA_Hook(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE result = g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);

    if (lpLibFileName && result) {
        const char* name = strrchr(lpLibFileName, '\\');
        name = name ? name + 1 : lpLibFileName;

        if (_stricmp(name, "resident.dll") == 0) {
            Log("[*] resident.dll loaded\n");
            InstallHooks(result);
            MH_DisableHook((void*)&LoadLibraryExA);
        }
    }

    return result;
}

//=============================================================================
// Initialization
//=============================================================================
static bool Initialize() {
    InitConsole();

    Log("==================================================\n");
    Log("%s\n", Encoding::SjisToUtf8("よついろ★パッショナート！ - Translation Hook").c_str());
    Log("==================================================\n\n");

    // Load translations (auto-detect encoding)
    g_translationDB.Load(Config::TRANSLATION_FILE, Config::NAMES_FILE);

    if (MH_Initialize() != MH_OK) {
        Log("[!] MinHook init failed\n");
        return false;
    }

    // Start file watcher
    g_fileWatcher.Start(".", {"translation.tsv", "unique_names.tsv"}, []() {
        g_stringPool.Clear();
        g_translationDB.Reload();
        MessageBeep(MB_OK);
    });

    // Start hotkey thread
    g_hotkeyThread = CreateThread(nullptr, 0, HotkeyThreadProc, nullptr, 0, nullptr);

    HMODULE hResident = GetModuleHandleA("resident.dll");
    if (hResident) {
        return InstallHooks(hResident);
    }

    if (MH_CreateHookApi(L"kernel32", "LoadLibraryExA",
        (void*)&LoadLibraryExA_Hook, (void**)&g_origLoadLibraryExA) == MH_OK) {
        MH_EnableHook((void*)&LoadLibraryExA);
        Log("[*] Waiting for resident.dll...\n");
        return true;
    }

    return false;
}

static void Shutdown() {
    g_running = false;
    g_fileWatcher.Stop();

    if (g_hotkeyThread) {
        WaitForSingleObject(g_hotkeyThread, 1000);
        CloseHandle(g_hotkeyThread);
    }

    Log("\n[*] Shutting down...\n");
    MH_Uninitialize();

    if (g_logFile) {
        fclose(g_logFile);
        FreeConsole();
    }
}

//=============================================================================
// ASI Entry Point (no DLL proxy needed)
//=============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        Initialize();
        break;
    case DLL_PROCESS_DETACH:
        Shutdown();
        break;
    }
    return TRUE;
}
