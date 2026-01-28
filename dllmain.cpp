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
// Function Offsets (ImageBase 0x10000000)
//=============================================================================
namespace Offsets {
    constexpr uintptr_t AdvCharSay = 0x00039A10; // RetouchAdvCharacter::say()
    constexpr uintptr_t PrintEx    = 0x00049A50; // RetouchPrintManager::printEx()
    constexpr uintptr_t SaveDataTitle     = 0x0004D470; // RetouchSaveDataControl::title()
    constexpr uintptr_t SaveDataIsValid   = 0x0004C210; // RetouchSaveDataControl::isValid()
    constexpr uintptr_t SaveDataGetItem   = 0x0004C2E0; // RetouchSaveDataControl::getItem()
    constexpr uintptr_t PrepareQuestion   = 0x000A5A20; // RetouchSystem::prepareQuestion()
}

//=============================================================================
// Configuration
//=============================================================================
namespace Config {
    // File paths
    char translationFile[MAX_PATH] = ".\\tl\\translation.tsv";
    char namesFile[MAX_PATH] = ".\\tl\\unique_names.tsv";
    char charIdFile[MAX_PATH] = ".\\tl\\char_table.tsv";
    char configFile[MAX_PATH] = ".\\yotsuiro_tl.ini";
    char untranslatedLog[MAX_PATH] = ".\\tl\\untranslated.tsv";

    // General
    bool enableConsole = true;
    bool enableTextLogging = true;
    bool dumpUntranslated = false;

    // Text
    int wordWrapWidth = 70;

    // Hotkeys
    int reloadHotkey = VK_F5;

    // Font
    char fontName[64] = "";
    char fontNameProportional[64] = "";

    // Asset redirection
    bool enableAssetRedirect = true;
    bool logAssetRedirects = false;
    char tlAssetsPath[MAX_PATH] = ".\\tl\\assets\\";
}

//=============================================================================
// Config Save/Load
//=============================================================================
static void SaveDefaultConfig() {
    FILE* f = nullptr;
    if (fopen_s(&f, Config::configFile, "w") != 0 || !f) return;

    fprintf(f, "; Yotsuiro Passionato Translation Hook Configuration\n");
    fprintf(f, "; Auto-generated - edit as needed\n");
    fprintf(f, "\n");

    fprintf(f, "[General]\n");
    fprintf(f,"; Show debug console window\n");
    fprintf(f, "EnableConsole=true\n");
    fprintf(f, "\n");
    fprintf(f, "; Log text to console\n");
    fprintf(f, "EnableTextLogging=true\n");
    fprintf(f, "\n");
    fprintf(f, "; Dump untranslated text to file\n");
    fprintf(f, "DumpUntranslated=false\n");
    fprintf(f, "\n");

    fprintf(f, "[Text]\n");
    fprintf(f, "; Word wrap width (characters per line, 0=disable)\n");
    fprintf(f, "WordWrapWidth=70\n");
    fprintf(f, "\n");

    fprintf(f, "[Hotkeys]\n");
    fprintf(f, "; Reload hotkey (VK code, F5=116, F6=117, etc.)\n");
    fprintf(f, "ReloadKey=116\n");
    fprintf(f, "\n");

    fprintf(f, "[Font]\n");
    fprintf(f, "; Custom font name (empty=game default)\n");
    fprintf(f, "Name=\n");
    fprintf(f, "\n");
    fprintf(f, "; Proportional font (empty=same as Name)\n");
    fprintf(f, "NameProportional=\n");
    fprintf(f, "\n");

    fprintf(f, "[Files]\n");
    fprintf(f, "; Translation file paths (relative to game folder)\n");
    fprintf(f, "TranslationFile=.\\tl\\translation.tsv\n");
    fprintf(f, "NamesFile=.\\tl\\unique_names.tsv\n");
    fprintf(f, "CharIdFile=.\\tl\\char_table.tsv\n");
    fprintf(f, "\n");

    fprintf(f, "[Assets]\n");
    fprintf(f, "; Enable asset redirection from tl/assets folder\n");
    fprintf(f, "EnableRedirect=true\n");
    fprintf(f, "\n");
    fprintf(f, "; Log asset redirects to console\n");
    fprintf(f, "LogRedirects=false\n");
    fprintf(f, "\n");
    fprintf(f, "; Path to replacement assets (supports .gyu and .png)\n");
    fprintf(f, "Path=.\\tl\\assets\\\n");
    fprintf(f, "\n");

    fclose(f);
}

// Helper functions for INI reading
static bool FileExists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static bool ReadBool(const char* section, const char* key, bool defaultVal) {
    char buf[16];
    GetPrivateProfileStringA(section, key, defaultVal ? "true" : "false", buf, sizeof(buf), Config::configFile);
    return (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0 || _stricmp(buf, "yes") == 0);
}

static int ReadInt(const char* section, const char* key, int defaultVal) {
    return GetPrivateProfileIntA(section, key, defaultVal, Config::configFile);
}

static void ReadString(const char* section, const char* key, const char* defaultVal, char* out, size_t outSize) {
    GetPrivateProfileStringA(section, key, defaultVal, out, (DWORD)outSize, Config::configFile);
}

// Forward declaration for Log (defined later)
static void Log(const char* fmt, ...);

static void LoadConfig() {
    // Auto-generate config if missing
    if (!FileExists(Config::configFile)) {
        Log("[CONFIG] Creating default config: %s\n", Config::configFile);
        SaveDefaultConfig();
    }

    // General
    Config::enableConsole = ReadBool("General", "EnableConsole", true);
    Config::enableTextLogging = ReadBool("General", "EnableTextLogging", true);
    Config::dumpUntranslated = ReadBool("General", "DumpUntranslated", false);

    // Text
    Config::wordWrapWidth = ReadInt("Text", "WordWrapWidth", 70);

    // Hotkeys
    Config::reloadHotkey = ReadInt("Hotkeys", "ReloadKey", VK_F5);

    // Font
    ReadString("Font", "Name", "", Config::fontName, sizeof(Config::fontName));
    ReadString("Font", "NameProportional", "", Config::fontNameProportional, sizeof(Config::fontNameProportional));

    // Files
    ReadString("Files", "TranslationFile", ".\\tl\\translation.tsv", Config::translationFile, sizeof(Config::translationFile));
    ReadString("Files", "NamesFile", ".\\tl\\unique_names.tsv", Config::namesFile, sizeof(Config::namesFile));
    ReadString("Files", "CharIdFile", ".\\tl\\char_table.tsv", Config::charIdFile, sizeof(Config::charIdFile));

    // Asset Redirection
    Config::enableAssetRedirect = ReadBool("Assets", "EnableRedirect", true);
    Config::logAssetRedirects = ReadBool("Assets", "LogRedirects", false);
    ReadString("Assets", "Path", ".\\tl\\assets\\", Config::tlAssetsPath, sizeof(Config::tlAssetsPath));

    // Ensure path ends with backslash
    size_t len = strlen(Config::tlAssetsPath);
    if (len > 0 && Config::tlAssetsPath[len - 1] != '\\') {
        strcat_s(Config::tlAssetsPath, "\\");
    }

    // Log
    Log("[CONFIG] Loaded from %s\n", Config::configFile);
    if (Config::fontName[0] != '\0') {
        Log("[CONFIG] Font: %s\n", Config::fontName);
    }
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
// Asset Redirection
//=============================================================================
namespace AssetRedirect {
    static bool FileExists(const char* path) {
        DWORD attr = GetFileAttributesA(path);
        return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
    }

    static std::string GetFileName(const std::string& path) {
        size_t pos = path.find_last_of("\\/");
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }

    static std::string GetRelativePath(const std::string& fullPath) {
        // Look for "res\" in the path
        size_t resPos = fullPath.find("res\\");
        if (resPos != std::string::npos) {
            return fullPath.substr(resPos + 4);
        }
        return GetFileName(fullPath);
    }

    static std::string FindReplacement(const std::string& originalPath) {
        if (!Config::enableAssetRedirect) return "";

        std::string relativePath = GetRelativePath(originalPath);
        std::string assetsPath = Config::tlAssetsPath;

        // Try 1: Exact path (tl/assets/g/ev/xxx.gyu)
        std::string tryPath = assetsPath + relativePath;
        if (FileExists(tryPath.c_str())) {
            return tryPath;
        }

        // Try 2: PNG version of exact path
        size_t extPos = tryPath.rfind(".gyu");
        if (extPos != std::string::npos) {
            std::string pngPath = tryPath.substr(0, extPos) + ".png";
            if (FileExists(pngPath.c_str())) {
                return pngPath;
            }
        }

        // Try 3: Flat structure (tl/assets/xxx.gyu)
        std::string filename = GetFileName(originalPath);
        tryPath = assetsPath + filename;
        if (FileExists(tryPath.c_str())) {
            return tryPath;
        }

        // Try 4: Flat PNG (tl/assets/xxx.png)
        extPos = filename.rfind(".gyu");
        if (extPos != std::string::npos) {
            std::string pngName = filename.substr(0, extPos) + ".png";
            tryPath = assetsPath + pngName;
            if (FileExists(tryPath.c_str())) {
                return tryPath;
            }
        }

        return "";
    }
}

//=============================================================================
// Shitty Text Fix
//=============================================================================
namespace TextFix {
    // Replace UTF-8 chars that don't exist in SJIS with ones that do
    std::string NormalizeUtf8(const std::string& utf8) {
        std::string result;
        result.reserve(utf8.size());

        for (size_t i = 0; i < utf8.size(); ) {
            // Check for em-dash "?" (UTF-8: E2 80 94)
            if (i + 2 < utf8.size() &&
                (unsigned char)utf8[i] == 0xE2 &&
                (unsigned char)utf8[i+1] == 0x80 &&
                (unsigned char)utf8[i+2] == 0x94) {
                // Replace with horizontal bar "―" (UTF-8: E2 80 95) - exists in SJIS
                result += "\xE2\x80\x95";
                i += 3;
                continue;
            }

            // Check for en-dash "?" (UTF-8: E2 80 93)
            if (i + 2 < utf8.size() &&
                (unsigned char)utf8[i] == 0xE2 &&
                (unsigned char)utf8[i+1] == 0x80 &&
                (unsigned char)utf8[i+2] == 0x93) {
                result += "\xE2\x80\x95";
                i += 3;
                continue;
            }

            // Check for curly quotes and replace with straight
            if (i + 2 < utf8.size() &&
                (unsigned char)utf8[i] == 0xE2 &&
                (unsigned char)utf8[i+1] == 0x80) {
                unsigned char c3 = (unsigned char)utf8[i+2];
                if (c3 == 0x98|| c3 == 0x99) {  // ' or '
                    result += "'";
                    i += 3;
                    continue;
                }if (c3 == 0x9C || c3 == 0x9D) {  // " or "
                    result += "\"";
                    i += 3;
                    continue;
                }
            }

            result += utf8[i];
            i++;
        }

        return result;
    }
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
        m_labels.clear();

        int globalCount = 0;
        int contextualCount = 0;
        int textCount = 0;
        int choiceCount = 0;
        int labelCount = 0;

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
            } else if (type == "LABEL") {
                m_labels[original] = translated;
                labelCount++;
            } else if (type.rfind("CHOICE_", 0) == 0) {
                m_messages[original] = translated;
                choiceCount++;
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
        Log("[TL]   %d choices\n", choiceCount);
        Log("[TL]   %d labels\n", labelCount);

        return true;
    }

    void Reload() {
        Log("[TL] Reloading...\n");
        Load(Config::translationFile, Config::namesFile);
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

    // Label
    // Label lookup - with [X] suffix fallback
    const std::string* FindLabelTranslation(const char* sjisLabel) {
        if (!sjisLabel || !*sjisLabel) return nullptr;

        std::string utf8Key = Encoding::SjisToUtf8(sjisLabel);
        if (utf8Key.empty()) return nullptr;

        std::lock_guard<std::mutex> lock(m_dataMutex);

        // Try exact match first
        auto it = m_labels.find(utf8Key);
        if (it != m_labels.end()) {
            return &it->second;
        }

        // Save files don't include [X] suffix, but TSV does
        // Try appending " [1]", " [2]", etc.
        for (int i = 1; i <= 30; i++) { // just 30 to be safe.
            std::string withSuffix = utf8Key + " [" + std::to_string(i) + "]";
            it = m_labels.find(withSuffix);
            if (it != m_labels.end()) {
                return &it->second;
            }
        }

        LogMissing(utf8Key.c_str(), "LABEL");
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

        std::ofstream file(Config::untranslatedLog, std::ios::app | std::ios::binary);
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
    std::unordered_map<std::string, std::string> m_labels;           // label -> translated
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
// Word Wrapping
//=============================================================================
namespace WordWrap {
    bool IsSjisLead(unsigned char c) {
        return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
    }

    std::string Wrap(const std::string& text, int maxWidth) {
        if (text.empty() || maxWidth <= 0) return text;

        std::string result;
        result.reserve(text.size() + 64);

        int lineLen = 0;
        size_t lineStart = 0;
        size_t lastSpace = std::string::npos;

        for (size_t i = 0; i < text.size(); ) {
            unsigned char c = (unsigned char)text[i];

            // Existing newline - reset
            if (c == '\n') {
                result += c;
                lineLen = 0;
                lineStart = result.size();
                lastSpace = std::string::npos;
                i++;
                continue;
            }

            // SJIS double-byte (Japanese) - don't count for word wrap
            if (IsSjisLead(c) && i + 1 < text.size()) {
                result += text[i];
                result += text[i + 1];
                lineLen += 2;
                i += 2;
                continue;
            }

            // Remember last space position for word wrap
            if (c == ' ') {
                lastSpace = result.size();
            }

            result += c;
            lineLen++;

            // Need to wrap?
            if (lineLen >= maxWidth) {
                if (lastSpace != std::string::npos && lastSpace > lineStart) {
                    // Replace space with newline
                    result[lastSpace] = '\n';
                    lineLen = (int)(result.size() - lastSpace - 1);
                    lineStart = lastSpace + 1;
                    lastSpace = std::string::npos;
                }// else: no good break point, let it overflow (game will handle)
            }

            i++;
        }

        return result;
    }
}

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
// Character ID → Original Name Lookup (from char_table.tsv)
//=============================================================================
static std::unordered_map<int, std::string> g_charIdToName;  // ID → original JP name

static void LoadCharIdTable(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        Log("[TL] No char_table.tsv found\n");
        return;
    }

    size_t size = (size_t)file.tellg();
    file.seekg(0);
    std::string content(size, 0);
    file.read(&content[0], size);

    Encoding::Type enc = Encoding::Detect(content.c_str(), size);
    std::string utf8 = Encoding::ToUtf8(content, enc);

    std::istringstream iss(utf8);
    std::string line;
    int count = 0;

    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.pop_back();
        if (line.find("ID") == 0) continue;  // Skip header

        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;

        int id = std::atoi(line.substr(0, tab).c_str());
        std::string name = line.substr(tab + 1);

        if (id > 0 && !name.empty()) {
            g_charIdToName[id] = name;
            count++;
        }
    }

    Log("[TL] Loaded %d CharID mappings\n", count);
}

//=============================================================================
// Hook: RetouchAdvCharacter::say()
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

    // If name is NULL, look up by CharID
    if ((!name || !*name) && pThis) {
        int charId = *((int*)((uintptr_t)pThis + 4));

        if (charId > 0) {
            // Step 1: CharID → original name
            auto it = g_charIdToName.find(charId);
            if (it != g_charIdToName.end()) {
                const std::string& origName = it->second;

                // Step 2: original name → translation (via unique_names.tsv)
                std::string sjisName = Encoding::Utf8ToSjis(origName.c_str());
                const std::string* tlUtf8 = g_translationDB.FindNameTranslation(sjisName.c_str(), nullptr);

                if (tlUtf8) {
                    std::string sjis = Encoding::Utf8ToSjis(tlUtf8->c_str());
                    if (!sjis.empty()) {
                        finalName = g_stringPool.Store(sjis);
                        Log("[SAY] CharID %d (%s) -> %s\n", charId, origName.c_str(), tlUtf8->c_str());
                    }
                } else {
                    // No translation, use original name
                    finalName = g_stringPool.Store(sjisName);
                }
            }
        }
    }
    // Translate inline name
    else if (name && *name) {
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
            std::string normalized = TextFix::NormalizeUtf8(*tlUtf8);
            std::string sjis = Encoding::Utf8ToSjis(normalized.c_str());
            if (!sjis.empty()) {
                std::string wrapped = WordWrap::Wrap(sjis, Config::wordWrapWidth); // Apply word wrapping
                finalMsg = g_stringPool.Store(wrapped);
            }
        }
    }

    // Log
    if (Config::enableTextLogging) {
        std::string nameUtf8 = name ? Encoding::SjisToUtf8(name) : "(null)";
        std::string msgUtf8 = message ? Encoding::SjisToUtf8(message) : "(null)";

        Log("[SAY] voiceId=%d flags=0x%08X\n", voiceId, flags);
        Log("      name=\"%s\"\n", nameUtf8.c_str());
        Log("      msg=\"%s\"\n", msgUtf8.c_str());

        if (finalName != name || finalMsg != message) {
            std::string tlNameUtf8 = finalName ? Encoding::SjisToUtf8(finalName) : "";
            std::string tlMsgUtf8 = finalMsg ? Encoding::SjisToUtf8(finalMsg) : "";
            Log("  --> name=\"%s\"\n", tlNameUtf8.c_str());
            Log("  --> msg=\"%s\"\n", tlMsgUtf8.c_str());
        }
    }

    g_origAdvCharSay(pThis, voiceId, finalName, finalMsg, flag, flags, p1, p2, p3, printParam);
}

//=============================================================================
// Hook: RetouchPrintManager::printEx()
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
            std::string normalized = TextFix::NormalizeUtf8(*tlUtf8);
            std::string sjis = Encoding::Utf8ToSjis(normalized.c_str());
            if (!sjis.empty()) {
                std::string wrapped = WordWrap::Wrap(sjis, Config::wordWrapWidth); // Apply word wrapping
                finalMsg = g_stringPool.Store(wrapped);
            }
        }
    }

    g_origPrintEx(pThis, charId, msgId, name, finalMsg, flags, linkData);
}

//=============================================================================
// Hook: SaveDataTitle (LABEL translation)
//=============================================================================
typedef bool(__thiscall* Fn_SaveDataIsValid)(void* pThis, int slotType, int slotIndex);
typedef void*(__thiscall* Fn_SaveDataGetItem)(void* pThis, int slotType, int slotIndex);

static Fn_SaveDataIsValid g_SaveDataIsValid = nullptr;
static Fn_SaveDataGetItem g_SaveDataGetItem = nullptr;

typedef int(__thiscall* Fn_SaveDataTitle)(
    void* pThis, void* fcString, int slotType, int slotIndex, bool useTemplate, unsigned int* outTime
);
static Fn_SaveDataTitle g_origSaveDataTitle = nullptr;

static int __fastcall SaveDataTitle_Hook(
    void* pThis, void* edx,
    void* fcString, int slotType, int slotIndex, bool useTemplate, unsigned int* outTime)
{
    // Debug
    Log("[SAVE] title() called: type=%d index=%d\n", slotType, slotIndex);

    // Check valid
    if (!g_SaveDataIsValid(pThis, slotType, slotIndex)) {
        Log("[SAVE] Invalid slot\n");
        return g_origSaveDataTitle(pThis, fcString, slotType, slotIndex, useTemplate, outTime);
    }

    DWORD* item = (DWORD*)g_SaveDataGetItem(pThis, slotType, slotIndex);

    // Empty slot - call original
    if (item[0] == 0) {
        Log("[SAVE] Empty slot\n");
        return g_origSaveDataTitle(pThis, fcString, slotType, slotIndex, useTemplate, outTime);
    }

    // Get label
    DWORD labelFCString = item[2];
    const char* labelSjis = *(const char**)(labelFCString + 0x14);
    Log("[SAVE] Label raw: %p -> \"%s\"\n", labelSjis, labelSjis ? Encoding::SjisToUtf8(labelSjis).c_str() : "(null)");

    // Try translate
    const char* finalLabel = labelSjis;
    std::string translatedSjis;

    if (labelSjis && *labelSjis) {
        const std::string* translated = g_translationDB.FindLabelTranslation(labelSjis);
        if (translated) {
            translatedSjis = Encoding::Utf8ToSjis(translated->c_str());
            finalLabel = translatedSjis.c_str();
            Log("[SAVE] Found translation: \"%s\"\n", translated->c_str());
        } else {
            Log("[SAVE] No translation found!\n");
        }
    }

    // Temporarily replace the label in the item
    const char* originalPtr = *(const char**)(labelFCString + 0x14);
    *(const char**)(labelFCString + 0x14) = finalLabel;

    // Call original
    int result = g_origSaveDataTitle(pThis, fcString, slotType, slotIndex, useTemplate, outTime);

    // Restore original
    *(const char**)(labelFCString + 0x14) = originalPtr;

    return result;
}

//=============================================================================
// Hook: RetouchSystem::prepareQuestion() (CHOICE translation)
//=============================================================================
typedef void(__thiscall* Fn_PrepareQuestion)(void* pThis, int choiceId, const char* text);
static Fn_PrepareQuestion g_origPrepareQuestion = nullptr;

static void __fastcall PrepareQuestion_Hook(void* pThis, void* edx, int choiceId, const char* text)
{
    const char* finalText = text;

    if (text && *text) {
        const std::string* translated = g_translationDB.FindMessageTranslation(text);
        if (translated) {
            std::string sjis = Encoding::Utf8ToSjis(translated->c_str());
            if (!sjis.empty()) {
                finalText = g_stringPool.Store(sjis);

                if (Config::enableTextLogging) {
                    Log("[CHOICE] %d: \"%s\" -> \"%s\"\n",
                        choiceId,
                        Encoding::SjisToUtf8(text).c_str(),
                        translated->c_str());
                }
            }
        }
    }

    g_origPrepareQuestion(pThis, choiceId, finalText);
}

//=============================================================================
// GetGlyphOutlineA Font Fix
//=============================================================================
typedef DWORD(WINAPI* Fn_GetGlyphOutlineA)(
    HDC hdc, UINT uChar, UINT fuFormat,
    LPGLYPHMETRICS lpgm, DWORD cjBuffer,
    LPVOID pvBuffer, const MAT2* lpmat2
);
static Fn_GetGlyphOutlineA g_origGetGlyphOutlineA = nullptr;

static DWORD WINAPI GetGlyphOutlineA_Hook(
    HDC hdc, UINT uChar, UINT fuFormat,
    LPGLYPHMETRICS lpgm, DWORD cjBuffer,
    LPVOID pvBuffer, const MAT2* lpmat2)
{
    DWORD result = g_origGetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);

    // Fix negative origin.x which breaks the renderer's rect calculations
    // This affects 'j' and potentially other characters in proportional fonts
    if (lpgm && pvBuffer && result != GDI_ERROR) {
        if (lpgm->gmptGlyphOrigin.x < 0) {
            int offset = -lpgm->gmptGlyphOrigin.x;
            lpgm->gmptGlyphOrigin.x = 0;
            lpgm->gmCellIncX += offset;  // Maintain proper spacing
        }
    }

    return result;
}

//=============================================================================
// Font Replacement Hook
//=============================================================================
typedef HFONT(WINAPI* Fn_CreateFontIndirectA)(const LOGFONTA*);
static Fn_CreateFontIndirectA g_origCreateFontIndirectA = nullptr;

static HFONT WINAPI CreateFontIndirectA_Hook(const LOGFONTA* lf) {
    if (lf) {
        LOGFONTA modified = *lf;

        std::string origName = Encoding::SjisToUtf8(lf->lfFaceName);
        const char* newFont = nullptr;

        // Check for proportional (Ｐ in SJIS =0x820x6F)
        bool isProportional = (strstr(lf->lfFaceName, "\x82\x6F") != nullptr);

        if (isProportional) {
            // Proportional font
            if (Config::fontNameProportional[0] != '\0') {
                newFont = Config::fontNameProportional;
            } else if (Config::fontName[0] != '\0') {
                newFont = Config::fontName;  // Fallback to main
            } else {
                newFont = "MS PGothic";  // Default
            }
        } else {
            // Non-proportional font
            if (Config::fontName[0] != '\0') {
                newFont = Config::fontName;
            } else {
                newFont = "MS Gothic";  // Default
            }
        }

        Log("[FONT] %s (h=%d) -> %s\n", origName.c_str(), lf->lfHeight, newFont);

        strcpy_s(modified.lfFaceName, newFont);
        modified.lfCharSet = DEFAULT_CHARSET;
        return g_origCreateFontIndirectA(&modified);
    }
    return g_origCreateFontIndirectA(lf);
}

//=============================================================================
// Hook: CreateFileA - Asset Redirection
//=============================================================================
typedef HANDLE(WINAPI* Fn_CreateFileA)(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurity, DWORD dwCreation,
    DWORD dwFlags, HANDLE hTemplate
);
static Fn_CreateFileA g_origCreateFileA = nullptr;

static HANDLE WINAPI CreateFileA_Hook(
    LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurity, DWORD dwCreation,
    DWORD dwFlags, HANDLE hTemplate)
{
    if (lpFileName && (dwDesiredAccess & GENERIC_READ)) {
        const char* ext = strrchr(lpFileName, '.');
        if (ext && _stricmp(ext, ".gyu") == 0) {
            std::string replacement = AssetRedirect::FindReplacement(lpFileName);
            if (!replacement.empty()) {
                if (Config::logAssetRedirects) {
                    Log("[ASSET] %s -> %s\n", lpFileName, replacement.c_str());
                }
                return g_origCreateFileA(
                    replacement.c_str(), dwDesiredAccess, dwShareMode,
                    lpSecurity, dwCreation, dwFlags, hTemplate
                );
            }
        }
    }

    return g_origCreateFileA(
        lpFileName, dwDesiredAccess, dwShareMode,
        lpSecurity, dwCreation, dwFlags, hTemplate
    );
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

    // Hook RetouchAdvCharacter::say()
    {
        uintptr_t addr = base + Offsets::AdvCharSay;
        Log("[*] Trying to hook RetouchAdvCharacter::say() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&AdvCharSay_Hook, (void**)&g_origAdvCharSay) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchAdvCharacter::say() hooked\n");
        }
    }

    // Hook RetouchPrintManager::printEx()
    {
        uintptr_t addr = base + Offsets::PrintEx;
        Log("[*] Trying to hook RetouchPrintManager::printEx() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&PrintEx_Hook, (void**)&g_origPrintEx) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchPrintManager::printEx() hooked\n");
        }
    }

    // Hook SaveDataTitle() for LABEL translation
    g_SaveDataIsValid = (Fn_SaveDataIsValid)(base + Offsets::SaveDataIsValid);
    g_SaveDataGetItem = (Fn_SaveDataGetItem)(base + Offsets::SaveDataGetItem);
    {
        uintptr_t addr = base + Offsets::SaveDataTitle;
        Log("[*] Trying to hook SaveDataTitle() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&SaveDataTitle_Hook, (void**)&g_origSaveDataTitle) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] SaveDataTitle() hooked\n");
        }
    }

    // Hook RetouchSystem::prepareQuestion() for CHOICE translation
    {
        uintptr_t addr = base + Offsets::PrepareQuestion;
        Log("[*] Trying to hook RetouchSystem::prepareQuestion() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&PrepareQuestion_Hook, (void**)&g_origPrepareQuestion) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchSystem::prepareQuestion() hooked\n");
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
    // Load config FIRST (before console init, so we know if console is enabled)
    LoadConfig();

    if (Config::enableConsole) {
        InitConsole();
    }

    Log("==================================================\n");
    Log("%s\n", Encoding::SjisToUtf8("よついろ★パッショナート！ - Translation Hook").c_str());
    Log("==================================================\n\n");

    // Load translations using config paths
    g_translationDB.Load(Config::translationFile, Config::namesFile);
    LoadCharIdTable(Config::charIdFile);

    if (MH_Initialize() != MH_OK) {
        Log("[!] MinHook init failed\n");
        return false;
    }

    // Hook GetGlyphOutlineA
    if (MH_CreateHookApi(L"gdi32", "GetGlyphOutlineA",
        (void*)&GetGlyphOutlineA_Hook, (void**)&g_origGetGlyphOutlineA) == MH_OK) {
        MH_EnableHook(MH_ALL_HOOKS);
        Log("[+] GetGlyphOutlineA hooked\n");
    }

    // Hook CreateFontIndirect
    if (MH_CreateHookApi(L"gdi32", "CreateFontIndirectA",
        (void*)&CreateFontIndirectA_Hook,
        (void**)&g_origCreateFontIndirectA) == MH_OK) {
        MH_EnableHook(MH_ALL_HOOKS);
        Log("[+] Font hook installed\n");
    }

    // Hook CreateFileA for asset redirection
    if (Config::enableAssetRedirect) {
        // Create assets directory if needed
        CreateDirectoryA(".\\tl", nullptr);
        CreateDirectoryA(Config::tlAssetsPath, nullptr);

        if (MH_CreateHookApi(L"kernel32", "CreateFileA",
            (void*)&CreateFileA_Hook, (void**)&g_origCreateFileA) == MH_OK) {
            MH_EnableHook(MH_ALL_HOOKS);
            Log("[+] Asset redirection hooked (%s)\n", Config::tlAssetsPath);
        }
    }

    // Start file watcher
    std::string watchDir = ".\\tl"; // Default
    std::string transFile = Config::translationFile;
    size_t lastSlash = transFile.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        watchDir = transFile.substr(0, lastSlash);
    }

    g_fileWatcher.Start(watchDir.c_str(), { 
        AssetRedirect::GetFileName(Config::translationFile), 
        AssetRedirect::GetFileName(Config::namesFile) 
    }, []() {
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
