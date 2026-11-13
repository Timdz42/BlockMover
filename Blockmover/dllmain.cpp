#include "pch.h"
#undef min
#undef max

#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx9.h"
#include "imgui/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d9.lib")

// ============================================================================
// 1. Sichere Speicher- & Hilfsfunktionen
// ============================================================================

struct Int3 { int32_t x, y, z; };

static int32_t MinI32(int32_t a, int32_t b) { return (a < b) ? a : b; }
static int32_t MaxI32(int32_t a, int32_t b) { return (a > b) ? a : b; }
static int32_t ClampI32(int32_t v, int32_t lo, int32_t hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static uintptr_t g_exeStart = 0;
static uintptr_t g_exeEnd = 0;

constexpr uintptr_t RVA_APP_SINGLETON_1 = 0x00968C44;
constexpr uintptr_t RVA_APP_SINGLETON_2 = 0x00972EB8;

constexpr int BLOCK_OFF_SKIN = 0x20; // CGameCtnBlockSkin* (Schilder / Custom-Links)
constexpr int BLOCK_OFF_BLOCKINFO = 0x24; // CGameCtnBlockInfo*
constexpr int BLOCK_OFF_MOBIL_1 = 0x30; // CSceneMobil*
constexpr int BLOCK_OFF_NOD_40 = 0x40;
constexpr int BLOCK_OFF_MOBIL_2 = 0x44;
constexpr int BLOCK_OFF_COORD_X = 0x48;
constexpr int BLOCK_OFF_COORD_Y = 0x4C;
constexpr int BLOCK_OFF_COORD_Z = 0x50;
constexpr int BLOCK_OFF_DIR = 0x54; // 0 = North, 1 = East, 2 = South, 3 = West
constexpr int BLOCK_OFF_FLAGS60 = 0x60; // Bits 0..5 = Variant, 0x1000 = IsGround, 0x2000 = Clip, 0x8000 = HasSkin

static CRITICAL_SECTION g_stateCs;
static bool             g_csInitialized = false;

struct ScopedStateLock {
    ScopedStateLock() { if (g_csInitialized) EnterCriticalSection(&g_stateCs); }
    ~ScopedStateLock() { if (g_csInitialized) LeaveCriticalSection(&g_stateCs); }
};

static bool SafeReadU32(uintptr_t addr, uint32_t* outVal) {
    if (addr < 0x10000) return false;
    __try { *outVal = *reinterpret_cast<const uint32_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeWriteU32(uintptr_t addr, uint32_t val) {
    if (addr < 0x10000) return false;
    __try { *reinterpret_cast<uint32_t*>(addr) = val; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeReadFloat(uintptr_t addr, float* outVal) {
    if (addr < 0x10000) return false;
    __try {
        float v = *reinterpret_cast<const float*>(addr);
        if (std::isnan(v) || std::isinf(v)) return false;
        *outVal = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeWriteFloat(uintptr_t addr, float val) {
    if (addr < 0x10000) return false;
    __try { *reinterpret_cast<float*>(addr) = val; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeReadByte(uintptr_t addr, uint8_t* outVal) {
    if (addr < 0x10000) return false;
    __try { *outVal = *reinterpret_cast<const uint8_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void InitExeBounds() {
    if (g_exeStart != 0) return;
    HMODULE hExe = GetModuleHandleA(nullptr);
    if (!hExe) return;
    g_exeStart = reinterpret_cast<uintptr_t>(hExe);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeStart);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_exeStart + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
            g_exeEnd = g_exeStart + static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
            return;
        }
    }
    g_exeEnd = g_exeStart + 0x00A62000;
}

static bool IsSafeWritableHeapAddr(uintptr_t addr) {
    InitExeBounds();
    if (addr < 0x10000 || (addr & 3) != 0) return false;
    if (addr >= g_exeStart && addr < g_exeEnd) return false;
    uint32_t dummy = 0;
    return SafeReadU32(addr, &dummy);
}

static const char* SafeGetCleanClassName(uintptr_t strAddr) {
    if (strAddr < 0x10000) return nullptr;
    __try {
        const char* s = reinterpret_cast<const char*>(strAddr);
        if (s[0] == '.' && s[1] == '?' && s[2] == 'A' && (s[3] == 'V' || s[3] == 'U')) s += 4;
        if (s[0] != 'C' && s[0] != 'I' && s[0] != 'S' && s[0] != 'N') return nullptr;
        if (s[1] < 'A' || s[1] > 'Z') return nullptr;
        int len = 0;
        for (; len < 60; ++len) {
            char c = s[len];
            if (c == '\0' || c == '@') break;
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) return nullptr;
        }
        return (len >= 3 && len < 60) ? s : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool ClassStartsWith(const char* cn, const char* prefix) {
    if (!cn || !prefix) return false;
    return std::strncmp(cn, prefix, std::strlen(prefix)) == 0;
}

static bool ClassIsExact(const char* cn, const char* target) {
    if (!cn || !target) return false;
    size_t tlen = std::strlen(target);
    if (std::strncmp(cn, target, tlen) != 0) return false;
    return (cn[tlen] == '\0' || cn[tlen] == '@');
}

static bool ClassContainsWord(const char* cn, const char* sub) {
    if (!cn || !sub || ClassStartsWith(cn, "CMwClassInfo")) return false;
    return std::strstr(cn, sub) != nullptr;
}

static bool IsAnyHeapObject(uintptr_t objAddr, uint32_t* outVTable = nullptr) {
    InitExeBounds();
    if (objAddr < 0x10000 || (objAddr & 3) != 0) return false;
    if (objAddr >= g_exeStart && objAddr < g_exeEnd) return false;
    uint32_t vt = 0, fn0 = 0;
    if (!SafeReadU32(objAddr, &vt) || vt < 0x10000 || (vt & 3) != 0) return false;
    if (!SafeReadU32(vt, &fn0) || fn0 < 0x10000) return false;
    if (outVTable) *outVTable = vt;
    return true;
}

static const char* GetHeapObjectClassName(uintptr_t objAddr, uint32_t* outVTable = nullptr) {
    uint32_t vt = 0;
    if (!IsAnyHeapObject(objAddr, &vt)) return nullptr;
    if (outVTable) *outVTable = vt;

    uint32_t colAddr = 0;
    if (SafeReadU32(vt - 4, &colAddr) && colAddr >= 0x10000) {
        uint32_t tdAddr = 0;
        if (SafeReadU32(colAddr + 0x0C, &tdAddr) && tdAddr >= 0x10000) {
            uint32_t magic = 0;
            if (SafeReadU32(tdAddr + 0x08, &magic) && (magic == 0x56413F2E || magic == 0x55413F2E)) {
                const char* rttiName = SafeGetCleanClassName(tdAddr + 0x0C);
                if (rttiName && !ClassStartsWith(rttiName, "CMwClassInfo")) return rttiName;
            }
        }
    }

    for (int i = 0; i < 8; ++i) {
        uint32_t fnAddr = 0;
        if (!SafeReadU32(vt + i * 4, &fnAddr) || fnAddr < 0x10000) continue;
        uint8_t b0 = 0, b5 = 0;
        if (!SafeReadByte(fnAddr, &b0)) continue;
        if (b0 == 0xE9) {
            uint32_t rel = 0;
            if (SafeReadU32(fnAddr + 1, &rel)) fnAddr = fnAddr + 5 + rel;
            SafeReadByte(fnAddr, &b0);
        }
        if (b0 == 0xB8 && SafeReadByte(fnAddr + 5, &b5) && (b5 == 0xC3 || b5 == 0xC2)) {
            uint32_t ciAddr = 0;
            if (SafeReadU32(fnAddr + 1, &ciAddr) && ciAddr >= 0x10000) {
                const int nameOffsets[] = { 0x1C, 0x14, 0x18 };
                for (int nOff : nameOffsets) {
                    uint32_t namePtr = 0;
                    if (SafeReadU32(ciAddr + nOff, &namePtr)) {
                        const char* ciName = SafeGetCleanClassName(namePtr);
                        if (ciName && !ClassStartsWith(ciName, "CMwClassInfo")) return ciName;
                    }
                }
            }
        }
    }
    return nullptr;
}

static int DetectHeapChunkEndOffset(uintptr_t baseAddr, int maxScanBytes) {
    for (int off = 0x08; off <= maxScanBytes; off += 8) {
        uint32_t w0 = 0, w1 = 0;
        if (!SafeReadU32(baseAddr + off, &w0) || !SafeReadU32(baseAddr + off + 4, &w1)) return off;
        if (w0 < 0x00100000u) continue;
        uint32_t topByte = w1 & 0xFF000000u;
        if ((topByte == 0x88000000u || topByte == 0x8C000000u || topByte == 0x8A000000u || topByte == 0x8E000000u) &&
            (w1 & 0x00FFFF00u) != 0 && (w1 & 0x000000FFu) == 0) {
            return off;
        }
    }
    return maxScanBytes;
}

// ============================================================================
// 2. Konfiguration, Sprache (DE/EN) & Globale Statusvariablen
// ============================================================================

constexpr int32_t VANILLA_MIN_X = 0, VANILLA_MAX_X = 31;
constexpr int32_t VANILLA_MIN_Y = 1, VANILLA_MAX_Y = 31;
constexpr int32_t VANILLA_MIN_Z = 0, VANILLA_MAX_Z = 31;
constexpr int32_t UNLIMITER_MAX = 255;

enum class BuildRuleMode : int { Vanilla = 0, TMUnlimiter = 1 };
enum class ModState { Inactive, Selecting, Moving };

static ModState g_state = ModState::Selecting;

struct ModConfig {
    bool modEnabled = true;
    int  language = 1; // 1 = English, 0 = Deutsch
    int  ruleMode = static_cast<int>(BuildRuleMode::TMUnlimiter);
    bool cameraRelativeMove = true;
    bool cursorFollowsMove = true;
    bool rotateAsGroup = true;
    bool soundFeedback = true;
    bool visualHighlight = true;
    bool showBlueBoxes = true;
    bool onlyPlayerBlocks = true;
    int  stepMultiplier = 1;

    // Standard-Keybinds
    int keyToggleMenu = VK_F3;      // 114
    int keyToggleModMode = VK_F6;      // 117
    int keySelectBlock = 'E';        // 69
    int keyConfirmSelection = VK_NUMPAD0; // 96
    int keyBackToSelect = VK_BACK;    // 8

    int keyMoveNorth = 'W';        // 87
    int keyMoveSouth = 'V';        // 86
    int keyMoveWest = 'A';        // 65
    int keyMoveEast = 'D';        // 68
    int keyMoveUp = VK_PRIOR;   // 33 (Page Up / Bild Hoch)
    int keyMoveDown = VK_NEXT;    // 34 (Page Down / Bild Runter)
    int keyRotateCW = 'R';        // 82
    int keyRotateCCW = 'Q';        // 81

    // Direkt-Werkzeuge (0 = Nicht belegt)
    int keyTeleportToCursor = VK_NUMPAD5; // 101
    int keyCloneAtCursor = 'L';        // 76
    int keySelectLastBuilt = 0;
    int keySelectAllBlocks = 0;
    int keySelectSameHeight = 0;
    int keySelectSameType = 0;
    int keyBoxCorner1 = 0;
    int keyBoxSelect2 = 0;
    int keyStepSizeCycle = 0;
    int keyCopySelection = 0;
    int keyPasteBlueprint = 'I';        // 73
};

static ModConfig g_cfg;

static const char* TR(const char* de, const char* en) {
    return (g_cfg.language == 1) ? en : de;
}

// ============================================================================
// 3. Exakter Zugriff auf CSceneMobils, CHmsItem & CHmsCorpus
// ============================================================================

struct DynamicEditorContext {
    uintptr_t pApp = 0;
    uintptr_t pEditorCat = 0;   // pApp+0x414 (CGameCtnEditorFree)
    uintptr_t pCursor = 0;      // pEditorCat+0xBC (CGameCtnCursor)
    uintptr_t pChallenge = 0;   // pApp+0x198 (CGameCtnChallenge)
    uintptr_t pBlocksArray = 0; // pChallenge+0x58
    uint32_t  blockCount = 0;   // pChallenge+0x54
};

struct SelectedBlockEntry {
    uintptr_t blockAddr;
    float     appliedPulseY;
};

static DynamicEditorContext            g_ctx;
static std::vector<SelectedBlockEntry> g_selectedBlocks;
static std::vector<uintptr_t>          g_dirtyBlocks;
static uintptr_t                       g_cachedAppPtrAddr = 0;
static int                             g_lastMovedFloats = 0;
static uintptr_t                       g_lastMovedBlockAddr = 0;

static bool IsBlockSelected(uintptr_t blockAddr) {
    for (const auto& e : g_selectedBlocks) {
        if (e.blockAddr == blockAddr) return true;
    }
    return false;
}

static void MarkBlockDirty(uintptr_t blockAddr) {
    if (!blockAddr) return;
    if (std::find(g_dirtyBlocks.begin(), g_dirtyBlocks.end(), blockAddr) == g_dirtyBlocks.end()) {
        g_dirtyBlocks.push_back(blockAddr);
    }
}

static Int3 ReadBlockCoord(uintptr_t blockAddr) {
    if (!IsAnyHeapObject(blockAddr)) return { 0, 1, 0 };
    uint32_t ux = 0, uy = 0, uz = 0;
    SafeReadU32(blockAddr + BLOCK_OFF_COORD_X, &ux);
    SafeReadU32(blockAddr + BLOCK_OFF_COORD_Y, &uy);
    SafeReadU32(blockAddr + BLOCK_OFF_COORD_Z, &uz);
    return { static_cast<int32_t>(ux), static_cast<int32_t>(uy), static_cast<int32_t>(uz) };
}

static uint32_t ReadBlockDir(uintptr_t blockAddr) {
    if (!IsAnyHeapObject(blockAddr)) return 0;
    uint32_t d = 0;
    SafeReadU32(blockAddr + BLOCK_OFF_DIR, &d);
    return d & 3u;
}

static void WriteBlockDir(uintptr_t blockAddr, uint32_t dir) {
    if (!IsAnyHeapObject(blockAddr)) return;
    SafeWriteU32(blockAddr + BLOCK_OFF_DIR, dir & 3u);
}

static void AddUniqueMobilPtr(std::vector<uintptr_t>& outMobils, uintptr_t mob, uintptr_t blockAddr, bool isChildMobil = false) {
    if (!IsAnyHeapObject(mob)) return;
    const char* cn = GetHeapObjectClassName(mob);
    if (!cn || !ClassStartsWith(cn, "CSceneMobil")) return;

    if (!isChildMobil) {
        uint32_t ownerBlock = 0;
        SafeReadU32(mob + 0x1C, &ownerBlock);
        if (ownerBlock != blockAddr && ownerBlock != 0) return;

        uint32_t refCnt = 0;
        if (SafeReadU32(mob + 0x04, &refCnt) && refCnt > 16) return;
    }

    if (std::find(outMobils.begin(), outMobils.end(), mob) == outMobils.end()) {
        outMobils.push_back(mob);
    }
}

static void GetBlockSceneMobils(uintptr_t blockAddr, std::vector<uintptr_t>& outMobils) {
    outMobils.clear();
    if (!IsAnyHeapObject(blockAddr)) return;

    for (int off = 0x28; off <= 0x64; off += 4) {
        if (off == BLOCK_OFF_COORD_X || off == BLOCK_OFF_COORD_Y || off == BLOCK_OFF_COORD_Z ||
            off == BLOCK_OFF_DIR || off == BLOCK_OFF_NOD_40 || off == BLOCK_OFF_FLAGS60) continue;
        uint32_t ptr = 0;
        if (!SafeReadU32(blockAddr + off, &ptr)) continue;
        bool isDirectSlot = (off == BLOCK_OFF_MOBIL_1 || off == BLOCK_OFF_MOBIL_2);
        AddUniqueMobilPtr(outMobils, ptr, blockAddr, isDirectSlot);
    }

    for (size_t i = 0; i < outMobils.size() && outMobils.size() < 32; ++i) {
        uintptr_t mob = outMobils[i];
        uint32_t subCnt = 0, subArr = 0;
        if (SafeReadU32(mob + 0x38, &subCnt) && SafeReadU32(mob + 0x3C, &subArr) &&
            subCnt > 0 && subCnt <= 16 && IsSafeWritableHeapAddr(subArr)) {
            for (uint32_t k = 0; k < subCnt; ++k) {
                uint32_t childCand = 0;
                if (SafeReadU32(subArr + k * 4, &childCand)) {
                    AddUniqueMobilPtr(outMobils, childCand, blockAddr, true);
                }
            }
        }
    }
}

static bool IsStadiumPerimeterBlockInfo(uintptr_t blockInfo) {
    if (!IsAnyHeapObject(blockInfo)) return false;
    for (int off = 0x14; off <= 0x40; off += 4) {
        uint32_t strPtr = 0;
        if (SafeReadU32(blockInfo + off, &strPtr) && strPtr >= 0x10000) {
            __try {
                const char* s = reinterpret_cast<const char*>(strPtr);
                if (s[0] >= 'A' && s[0] <= 'Z') {
                    if (std::strncmp(s, "Stadium", 7) == 0 &&
                        (std::strstr(s, "Warp") != nullptr || std::strstr(s, "Tribune") != nullptr ||
                            std::strstr(s, "Sculpt") != nullptr || std::strstr(s, "Screen") != nullptr ||
                            std::strstr(s, "Light") != nullptr)) {
                        return true;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    return false;
}

static bool IsMovableTrackBlock(uintptr_t blockAddr) {
    if (!IsAnyHeapObject(blockAddr)) return false;

    uint32_t blockInfo = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_BLOCKINFO, &blockInfo) && IsAnyHeapObject(blockInfo)) {
        const char* biName = GetHeapObjectClassName(blockInfo);
        // Nur reine Gras-Bodenkacheln (Flat), Auto-Clips und Stützpfeiler ausschließen:
        if (biName && (ClassContainsWord(biName, "BlockInfoClip") ||
            ClassContainsWord(biName, "BlockInfoFlat") ||
            ClassContainsWord(biName, "BlockInfoPylon"))) {
            return false;
        }
    }

    uint32_t flags60 = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_FLAGS60, &flags60)) {
        // WICHTIG: 0x1000 NICHT ausschließen! 0x1000 = IsGround (normale Blöcke, die auf dem Boden stehen).
        // Nur 0x2000 (Auto-Clip), 0x3F (System-Variante) und 0x10000000/0x20000000 (feste Stadion-Umgebung) ausschließen:
        if ((flags60 & 0x2000u) != 0 || (flags60 & 0x3Fu) == 0x3Fu || (flags60 & 0x30000000u) != 0) {
            return false;
        }
    }

    if (g_cfg.onlyPlayerBlocks && !IsBlockSelected(blockAddr)) {
        Int3 c = ReadBlockCoord(blockAddr);
        if (c.y <= 0 || IsStadiumPerimeterBlockInfo(blockInfo)) return false;
    }

    std::vector<uintptr_t> mobils;
    GetBlockSceneMobils(blockAddr, mobils);
    return !mobils.empty();
}

static void WriteBlockCoord(uintptr_t blockAddr, const Int3& newC) {
    if (!IsAnyHeapObject(blockAddr)) return;
    SafeWriteU32(blockAddr + BLOCK_OFF_COORD_X, static_cast<uint32_t>(newC.x));
    SafeWriteU32(blockAddr + BLOCK_OFF_COORD_Y, static_cast<uint32_t>(newC.y));
    SafeWriteU32(blockAddr + BLOCK_OFF_COORD_Z, static_cast<uint32_t>(newC.z));
}

static bool IsOrthonormalRotationMatrixAt(uintptr_t objAddr, int off) {
    float m[9] = { 0 };
    for (int i = 0; i < 9; ++i) {
        if (!SafeReadFloat(objAddr + off + i * 4, &m[i])) return false;
    }
    if (std::fabs(m[4] - 1.0f) > 0.06f) return false;
    if (std::fabs(m[1]) > 0.06f || std::fabs(m[3]) > 0.06f || std::fabs(m[5]) > 0.06f || std::fabs(m[7]) > 0.06f) return false;
    float detXZ = m[0] * m[8] - m[2] * m[6];
    return std::fabs(std::fabs(detXZ) - 1.0f) < 0.08f;
}

static int MoveHmsCorpus(uintptr_t corpusAddr, float dxW, float dyW, float dzW) {
    if (!IsAnyHeapObject(corpusAddr)) return 0;
    const char* cn = GetHeapObjectClassName(corpusAddr);
    if (cn && !ClassStartsWith(cn, "CHmsCorpus")) return 0;

    float oldCx = 0, oldCy = 0, oldCz = 0;
    if (IsOrthonormalRotationMatrixAt(corpusAddr, 0x18) &&
        SafeReadFloat(corpusAddr + 0x3C, &oldCx) &&
        SafeReadFloat(corpusAddr + 0x40, &oldCy) &&
        SafeReadFloat(corpusAddr + 0x44, &oldCz)) {
        SafeWriteFloat(corpusAddr + 0x3C, oldCx + dxW);
        SafeWriteFloat(corpusAddr + 0x40, oldCy + dyW);
        SafeWriteFloat(corpusAddr + 0x44, oldCz + dzW);
        return 1;
    }
    return 0;
}

static int MoveSingleSceneMobil(uintptr_t mobilAddr, float dxW, float dyW, float dzW, std::vector<uintptr_t>& visited) {
    if (!IsAnyHeapObject(mobilAddr)) return 0;
    uint32_t hmsItem = 0;
    if (!SafeReadU32(mobilAddr + 0x28, &hmsItem) || !IsAnyHeapObject(hmsItem)) return 0;

    int hits = 0;
    uint32_t pairCount = 0, pairArray = 0;
    if (SafeReadU32(hmsItem + 0x34, &pairCount) &&
        SafeReadU32(hmsItem + 0x38, &pairArray) &&
        pairCount > 0 && pairCount <= 32 && IsSafeWritableHeapAddr(pairArray)) {
        for (uint32_t i = 0; i < pairCount; ++i) {
            uint32_t corpusPtr = 0;
            if (SafeReadU32(pairArray + i * 4, &corpusPtr) && IsAnyHeapObject(corpusPtr)) {
                if (std::find(visited.begin(), visited.end(), corpusPtr) == visited.end()) {
                    visited.push_back(corpusPtr);
                    hits += MoveHmsCorpus(corpusPtr, dxW, dyW, dzW);
                }
            }
        }
    }
    return hits;
}

static int MoveBlockVisualsTargeted(uintptr_t blockAddr, float dxWorld, float dyWorld, float dzWorld) {
    if (!IsAnyHeapObject(blockAddr)) return 0;
    if (std::fabs(dxWorld) >= 4.0f || std::fabs(dyWorld) >= 4.0f || std::fabs(dzWorld) >= 4.0f) {
        if (g_selectedBlocks.empty() || blockAddr == g_selectedBlocks[0].blockAddr) {
            g_lastMovedBlockAddr = blockAddr;
        }
    }

    std::vector<uintptr_t> mobils;
    GetBlockSceneMobils(blockAddr, mobils);

    std::vector<uintptr_t> visited;
    int totalHits = 0;
    for (uintptr_t mob : mobils) {
        totalHits += MoveSingleSceneMobil(mob, dxWorld, dyWorld, dzWorld, visited);
    }
    if (totalHits > 0) MarkBlockDirty(blockAddr);
    return totalHits;
}

static void RotateBlockVisualsTargeted(uintptr_t blockAddr, uint32_t dir) {
    std::vector<uintptr_t> mobils;
    GetBlockSceneMobils(blockAddr, mobils);
    if (mobils.empty()) return;

    uint32_t d = dir & 3u;
    float r[9] = { 0 };
    r[4] = 1.0f;
    switch (d) {
    case 0: r[0] = 1.0f; r[2] = 0.0f; r[6] = 0.0f; r[8] = 1.0f; break;
    case 1: r[0] = 0.0f; r[2] = -1.0f; r[6] = 1.0f; r[8] = 0.0f; break;
    case 2: r[0] = -1.0f; r[2] = 0.0f; r[6] = 0.0f; r[8] = -1.0f; break;
    case 3: r[0] = 0.0f; r[2] = 1.0f; r[6] = -1.0f; r[8] = 0.0f; break;
    }

    float newCornerX = (d == 1u || d == 2u) ? 32.0f : 0.0f;
    float newCornerZ = (d == 2u || d == 3u) ? 32.0f : 0.0f;

    std::vector<uintptr_t> visited;
    for (uintptr_t mob : mobils) {
        uint32_t hmsItem = 0;
        if (!SafeReadU32(mob + 0x28, &hmsItem) || !IsAnyHeapObject(hmsItem)) continue;

        uint32_t pairCount = 0, pairArray = 0;
        if (SafeReadU32(hmsItem + 0x34, &pairCount) &&
            SafeReadU32(hmsItem + 0x38, &pairArray) &&
            pairCount > 0 && pairCount <= 32 && IsSafeWritableHeapAddr(pairArray)) {
            for (uint32_t i = 0; i < pairCount; ++i) {
                uint32_t corpusPtr = 0;
                if (SafeReadU32(pairArray + i * 4, &corpusPtr) && IsAnyHeapObject(corpusPtr)) {
                    if (std::find(visited.begin(), visited.end(), corpusPtr) != visited.end()) continue;
                    visited.push_back(corpusPtr);

                    if (IsOrthonormalRotationMatrixAt(corpusPtr, 0x18)) {
                        float m0 = 1.0f, m2 = 0.0f, cx = 0.0f, cz = 0.0f;
                        SafeReadFloat(corpusPtr + 0x18, &m0);
                        SafeReadFloat(corpusPtr + 0x20, &m2);
                        uint32_t curDir = (m0 > 0.5f) ? 0u : ((m2 < -0.5f) ? 1u : ((m0 < -0.5f) ? 2u : 3u));

                        float oldCornerX = (curDir == 1u || curDir == 2u) ? 32.0f : 0.0f;
                        float oldCornerZ = (curDir == 2u || curDir == 3u) ? 32.0f : 0.0f;

                        if (SafeReadFloat(corpusPtr + 0x3C, &cx) && SafeReadFloat(corpusPtr + 0x44, &cz)) {
                            SafeWriteFloat(corpusPtr + 0x3C, cx + (newCornerX - oldCornerX));
                            SafeWriteFloat(corpusPtr + 0x44, cz + (newCornerZ - oldCornerZ));
                        }
                        for (int k = 0; k < 9; ++k) {
                            SafeWriteFloat(corpusPtr + 0x18 + k * 4, r[k]);
                        }
                    }
                }
            }
        }
    }
    MarkBlockDirty(blockAddr);
}

// ============================================================================
// 4. Cursor-Zugriff, Main-Thread Scene-Rebuild & Optische Hervorhebung
// ============================================================================

typedef void(__thiscall* FnEditorRebuildThiscall)(void* pEditor);
typedef void(__thiscall* FnMobilRemoveFromSceneThiscall)(void* pMobil);
typedef void(__thiscall* FnScene3dAddMobilThiscall)(void* pScene3d, void* pMobil, const void* pPlacement);
typedef uintptr_t(__thiscall* FnEditorPlaceBlockVt68Thiscall)(
    void* pEditor, uintptr_t pBlockInfo, uint32_t arg1_zero,
    int32_t coordX, int32_t coordY, int32_t coordZ, uint32_t dir,
    uint32_t isGroundOrVariant, uint32_t arg7_zero, uint32_t arg8_zero,
    uint32_t arg9_zero, uint32_t arg10_zero);

struct SceneMobilPlacement {
    float    iso4[12];
    uint32_t pZoneOverride;
};

static uintptr_t ReadBlockPlugTree(uintptr_t blockAddr) {
    uint32_t mob = 0, hmsItem = 0, plugSolid = 0, plugTree = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_1, &mob) && IsAnyHeapObject(mob) &&
        SafeReadU32(mob + 0x28, &hmsItem) && IsAnyHeapObject(hmsItem) &&
        SafeReadU32(hmsItem + 0x14, &plugSolid) && IsAnyHeapObject(plugSolid) &&
        SafeReadU32(plugSolid + 0x5C, &plugTree) && IsAnyHeapObject(plugTree)) {
        return plugTree;
    }
    return 0;
}

static uint32_t ReadBlockCorpus4C(uintptr_t blockAddr) {
    uint32_t mob = 0, hmsItem = 0, cCnt = 0, cArr = 0, corp0 = 0, val4C = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_1, &mob) && IsAnyHeapObject(mob) &&
        SafeReadU32(mob + 0x28, &hmsItem) && IsAnyHeapObject(hmsItem) &&
        SafeReadU32(hmsItem + 0x34, &cCnt) && SafeReadU32(hmsItem + 0x38, &cArr) &&
        cCnt > 0 && IsSafeWritableHeapAddr(cArr) &&
        SafeReadU32(cArr, &corp0) && IsAnyHeapObject(corp0) &&
        SafeReadU32(corp0 + 0x4C, &val4C)) {
        return val4C;
    }
    return 0;
}

struct PendingBlockSpawn {
    uintptr_t pBlockInfo;
    Int3      pos;
    uint32_t  dir;
    uint32_t  flags60;
    uintptr_t pSkin;
    uintptr_t pPlugTree;
};

struct ClonedSignVisualOverride {
    uintptr_t blockAddr;
    uintptr_t pSkin;
    uintptr_t pPlugTree;
    uint32_t  corpus4C;
    uint32_t  flags60;
};

static std::vector<ClonedSignVisualOverride> g_clonedSignOverrides;

static void ApplyClonedSignVisualOverrideToBlock(const ClonedSignVisualOverride& ov) {
    if (!IsAnyHeapObject(ov.blockAddr)) return;

    // 1. Flags60 (inkl. 0x8000 Skin-Bit) sicherstellen
    if (ov.flags60 != 0u) {
        uint32_t curFl60 = 0;
        if (SafeReadU32(ov.blockAddr + BLOCK_OFF_FLAGS60, &curFl60)) {
            SafeWriteU32(ov.blockAddr + BLOCK_OFF_FLAGS60, (curFl60 & ~0x8FFFu) | (ov.flags60 & 0x8FFFu));
        }
    }

    // 2. Exakten CGameCtnBlockSkin (+0x20) des Quell-Schildes setzen
    if (IsAnyHeapObject(ov.pSkin)) {
        uint32_t curSkin = 0;
        if (SafeReadU32(ov.blockAddr + BLOCK_OFF_SKIN, &curSkin) && curSkin != ov.pSkin) {
            uint32_t refCnt = 0;
            if (SafeReadU32(ov.pSkin + 0x04, &refCnt)) {
                SafeWriteU32(ov.pSkin + 0x04, refCnt + 1);
            }
            SafeWriteU32(ov.blockAddr + BLOCK_OFF_SKIN, static_cast<uint32_t>(ov.pSkin));
        }
    }

    // 3. CPlugTree (CPlugSolid + 0x5C) UND gecachten Render-Pointer (CHmsCorpus + 0x4C) aktualisieren
    uint32_t dstMob = 0, dstHmsItem = 0, dstPlugSolid = 0;
    if (SafeReadU32(ov.blockAddr + BLOCK_OFF_MOBIL_1, &dstMob) && IsAnyHeapObject(dstMob) &&
        SafeReadU32(dstMob + 0x28, &dstHmsItem) && IsAnyHeapObject(dstHmsItem)) {

        if (IsAnyHeapObject(ov.pPlugTree) &&
            SafeReadU32(dstHmsItem + 0x14, &dstPlugSolid) && IsAnyHeapObject(dstPlugSolid)) {
            uint32_t oldTree = 0;
            if (SafeReadU32(dstPlugSolid + 0x5C, &oldTree) && oldTree != ov.pPlugTree) {
                uint32_t treeRef = 0;
                if (SafeReadU32(ov.pPlugTree + 0x04, &treeRef)) {
                    SafeWriteU32(ov.pPlugTree + 0x04, treeRef + 1);
                }
                SafeWriteU32(dstPlugSolid + 0x5C, static_cast<uint32_t>(ov.pPlugTree));
            }
        }

        if (ov.corpus4C != 0) {
            uint32_t cCnt = 0, cArr = 0, corp0 = 0;
            if (SafeReadU32(dstHmsItem + 0x34, &cCnt) && SafeReadU32(dstHmsItem + 0x38, &cArr) &&
                cCnt > 0 && IsSafeWritableHeapAddr(cArr) &&
                SafeReadU32(cArr, &corp0) && IsAnyHeapObject(corp0)) {
                SafeWriteU32(corp0 + 0x4C, ov.corpus4C);
            }
        }
    }
}

static bool                           g_menuVisible = false;
static WNDPROC                        g_origGameWndProc = nullptr;
static HWND                           g_hookedGameWnd = nullptr;
static volatile bool                  g_rebuildPending = false;
static uint32_t                       g_rebuildExecCount = 0;
static uint32_t                       g_lastRebuildException = 0;
static uintptr_t                      g_mobilBeforeRebuild = 0;
static uintptr_t                      g_mobilAfterRebuild = 0;
static uint32_t                       g_mobilSceneUpdateCount = 0;
static std::vector<PendingBlockSpawn> g_pendingSpawns;
static uint32_t                       g_lastSpawnedCount = 0;

static Int3                     g_lastWorldCursorPos = { 15, 2, 15 };
static bool                     g_centerBlueprintAtCursor = true;
static std::vector<std::string> g_blueprintFiles;

static void RequestLiveEditorRebuild();

static void ApplyPulseToEntry(SelectedBlockEntry& entry) {
    if (!g_cfg.visualHighlight || entry.appliedPulseY != 0.0f || !IsAnyHeapObject(entry.blockAddr)) return;
    if (MoveBlockVisualsTargeted(entry.blockAddr, 0.0f, 2.0f, 0.0f) > 0) {
        entry.appliedPulseY = 2.0f;
    }
}

static void ApplyAllSelectionPulses() {
    if (!g_cfg.visualHighlight) return;
    for (auto& entry : g_selectedBlocks) ApplyPulseToEntry(entry);
}

static void RemovePulseFromEntry(SelectedBlockEntry& entry) {
    if (entry.appliedPulseY == 0.0f) return;
    if (IsAnyHeapObject(entry.blockAddr)) {
        MoveBlockVisualsTargeted(entry.blockAddr, 0.0f, -entry.appliedPulseY, 0.0f);
    }
    entry.appliedPulseY = 0.0f;
}

static void RemoveAllSelectionPulses() {
    for (auto& entry : g_selectedBlocks) RemovePulseFromEntry(entry);
}

static void ClearSelectionWithRestore() {
    bool hadSelection = !g_selectedBlocks.empty();
    RemoveAllSelectionPulses();
    g_selectedBlocks.clear();
    if (hadSelection) RequestLiveEditorRebuild();
}

static Int3 ReadCursorCoord() {
    if (!IsAnyHeapObject(g_ctx.pCursor)) return { 0, 1, 0 };
    uint32_t ux = 0, uy = 0, uz = 0;
    if (SafeReadU32(g_ctx.pCursor + 0x14, &ux) &&
        SafeReadU32(g_ctx.pCursor + 0x18, &uy) &&
        SafeReadU32(g_ctx.pCursor + 0x1C, &uz)) {
        return { static_cast<int32_t>(ux), static_cast<int32_t>(uy), static_cast<int32_t>(uz) };
    }
    return { 0, 1, 0 };
}

static void WriteCursorCoord(const Int3& c) {
    if (!IsAnyHeapObject(g_ctx.pCursor)) return;
    Int3 oldC = ReadCursorCoord();
    SafeWriteU32(g_ctx.pCursor + 0x14, static_cast<uint32_t>(c.x));
    SafeWriteU32(g_ctx.pCursor + 0x18, static_cast<uint32_t>(c.y));
    SafeWriteU32(g_ctx.pCursor + 0x1C, static_cast<uint32_t>(c.z));

    float dxW = (c.x - oldC.x) * 32.0f;
    float dyW = (c.y - oldC.y) * 8.0f;
    float dzW = (c.z - oldC.z) * 32.0f;
    if (std::fabs(dxW) > 0.1f || std::fabs(dyW) > 0.1f || std::fabs(dzW) > 0.1f) {
        std::vector<uintptr_t> visited;
        const int curMobilOffsets[] = { 0x40, 0x44, 0x48 };
        for (int mOff : curMobilOffsets) {
            uint32_t cMob = 0;
            if (SafeReadU32(g_ctx.pCursor + mOff, &cMob) && IsAnyHeapObject(cMob)) {
                MoveSingleSceneMobil(cMob, dxW, dyW, dzW, visited);
            }
        }
    }
}

static void UpdateCursorBoxHighlightColor(bool hasSelection) {
    if (!IsAnyHeapObject(g_ctx.pCursor)) return;
    if (hasSelection) {
        SafeWriteFloat(g_ctx.pCursor + 0x2C, 0.0f);
        SafeWriteFloat(g_ctx.pCursor + 0x30, 0.85f);
        SafeWriteFloat(g_ctx.pCursor + 0x34, 1.0f);
        SafeWriteFloat(g_ctx.pCursor + 0x58, 0.0f);
        SafeWriteFloat(g_ctx.pCursor + 0x5C, 0.85f);
        SafeWriteFloat(g_ctx.pCursor + 0x60, 1.0f);
    }
    else {
        SafeWriteFloat(g_ctx.pCursor + 0x2C, 0.17f);
        SafeWriteFloat(g_ctx.pCursor + 0x30, 0.05f);
        SafeWriteFloat(g_ctx.pCursor + 0x34, 0.05f);
        SafeWriteFloat(g_ctx.pCursor + 0x58, 0.05f);
        SafeWriteFloat(g_ctx.pCursor + 0x5C, 0.17f);
        SafeWriteFloat(g_ctx.pCursor + 0x60, 0.05f);
    }
}

static bool RefreshEditorContext() {
    InitExeBounds();
    g_ctx.pEditorCat = 0;
    g_ctx.pCursor = 0;
    g_ctx.pChallenge = 0;
    g_ctx.pBlocksArray = 0;
    g_ctx.blockCount = 0;

    const uintptr_t knownGlobals[] = {
        g_exeStart + RVA_APP_SINGLETON_1,
        g_exeStart + RVA_APP_SINGLETON_2,
        g_cachedAppPtrAddr
    };
    for (uintptr_t gAddr : knownGlobals) {
        if (!gAddr) continue;
        uint32_t appCand = 0;
        if (SafeReadU32(gAddr, &appCand) && IsAnyHeapObject(appCand)) {
            const char* cn = GetHeapObjectClassName(appCand);
            if (cn && (ClassIsExact(cn, "CTrackMania") || ClassStartsWith(cn, "CGameCtnApp"))) {
                g_ctx.pApp = appCand;
                g_cachedAppPtrAddr = gAddr;
                break;
            }
        }
    }
    if (!g_ctx.pApp) return false;

    uint32_t ch198 = 0;
    if (SafeReadU32(g_ctx.pApp + 0x198, &ch198) && IsAnyHeapObject(ch198)) {
        g_ctx.pChallenge = ch198;
    }

    if (g_ctx.pChallenge) {
        uint32_t cnt54 = 0, arr58 = 0;
        if (SafeReadU32(g_ctx.pChallenge + 0x54, &cnt54) &&
            SafeReadU32(g_ctx.pChallenge + 0x58, &arr58) &&
            cnt54 > 0 && cnt54 <= 50000 && arr58 >= 0x10000 && (arr58 & 3) == 0) {
            g_ctx.blockCount = cnt54;
            g_ctx.pBlocksArray = arr58;
        }
    }

    uint32_t ed414 = 0;
    if (SafeReadU32(g_ctx.pApp + 0x414, &ed414) && IsAnyHeapObject(ed414)) {
        g_ctx.pEditorCat = ed414;
        uint32_t curBC = 0;
        if (SafeReadU32(ed414 + 0xBC, &curBC) && IsAnyHeapObject(curBC)) {
            g_ctx.pCursor = curBC;
        }
        else {
            for (int off = 0x10; off <= 0x200; off += 4) {
                uint32_t field = 0;
                if (!SafeReadU32(ed414 + off, &field) || !IsAnyHeapObject(field)) continue;
                const char* cn = GetHeapObjectClassName(field);
                if (cn && ClassContainsWord(cn, "Cursor")) {
                    g_ctx.pCursor = field;
                    break;
                }
            }
        }
    }
    return (g_ctx.pChallenge != 0 && g_ctx.pBlocksArray != 0);
}

static uintptr_t CallEditorPlaceBlockVt68Seh(
    uintptr_t editorAddr, uintptr_t fnVt68Addr, uintptr_t pBlockInfo,
    int32_t x, int32_t y, int32_t z, uint32_t dir, uint32_t isGroundOrVariant)
{
    __try {
        auto fnPlace = reinterpret_cast<FnEditorPlaceBlockVt68Thiscall>(fnVt68Addr);
        return fnPlace(reinterpret_cast<void*>(editorAddr), pBlockInfo, 0u,
            x, y, z, dir & 3u, isGroundOrVariant, 0u, 0u, 0u, 0u);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastRebuildException = GetExceptionCode();
        return 0;
    }
}

static bool UpdateSingleMobilInScene3dSeh(uintptr_t mobilAddr, uintptr_t scene3dAddr, uintptr_t fnRemoveAddr, uintptr_t fnAddAddr, const SceneMobilPlacement* pPlacement) {
    __try {
        auto fnRemove = reinterpret_cast<FnMobilRemoveFromSceneThiscall>(fnRemoveAddr);
        auto fnAdd = reinterpret_cast<FnScene3dAddMobilThiscall>(fnAddAddr);

        uint32_t curScene = 0;
        SafeReadU32(mobilAddr + 0x14, &curScene);
        if (curScene != 0) fnRemove(reinterpret_cast<void*>(mobilAddr));
        fnAdd(reinterpret_cast<void*>(scene3dAddr), reinterpret_cast<void*>(mobilAddr), pPlacement);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastRebuildException = GetExceptionCode();
        return false;
    }
}

static void RefreshBlockMobilsInScene3dMainThread(uintptr_t blockAddr) {
    if (!IsAnyHeapObject(blockAddr) || g_exeStart == 0) return;

    uintptr_t fnRemoveAddr = g_exeStart + 0x3B5720;
    std::vector<uintptr_t> mobils;
    GetBlockSceneMobils(blockAddr, mobils);

    for (uintptr_t mob : mobils) {
        if (!IsAnyHeapObject(mob)) continue;

        uint32_t scene3d = 0, sceneVt = 0, fnAddAddr = 0, hmsItem = 0;
        if (!SafeReadU32(mob + 0x14, &scene3d) || !IsAnyHeapObject(scene3d, &sceneVt)) continue;
        if (!SafeReadU32(sceneVt + 30 * 4, &fnAddAddr) || fnAddAddr < 0x10000) continue;
        if (!SafeReadU32(mob + 0x28, &hmsItem) || !IsAnyHeapObject(hmsItem)) continue;

        uint32_t cCnt = 0, cArr = 0, corpus0 = 0;
        if (!SafeReadU32(hmsItem + 0x34, &cCnt) || !SafeReadU32(hmsItem + 0x38, &cArr) ||
            cCnt == 0 || cCnt > 32 || !IsSafeWritableHeapAddr(cArr) ||
            !SafeReadU32(cArr, &corpus0) || !IsAnyHeapObject(corpus0)) {
            continue;
        }

        SceneMobilPlacement placement{};
        bool readOk = true;
        for (int i = 0; i < 12; ++i) {
            if (!SafeReadFloat(corpus0 + 0x18 + i * 4, &placement.iso4[i])) {
                readOk = false;
                break;
            }
        }
        if (!readOk) continue;
        placement.pZoneOverride = 0;

        uint32_t origRef = 0;
        if (SafeReadU32(mob + 0x04, &origRef)) SafeWriteU32(mob + 0x04, origRef + 1);

        if (UpdateSingleMobilInScene3dSeh(mob, scene3d, fnRemoveAddr, fnAddAddr, &placement)) {
            ++g_mobilSceneUpdateCount;
        }

        uint32_t afterRef = 0;
        if (origRef > 0 && SafeReadU32(mob + 0x04, &afterRef) && afterRef > origRef) {
            SafeWriteU32(mob + 0x04, afterRef - 1);
        }
    }
}

static bool CallEditorVTableRebuildSeh(uintptr_t editorAddr, uint32_t fnAddr) {
    __try {
        reinterpret_cast<FnEditorRebuildThiscall>(fnAddr)(reinterpret_cast<void*>(editorAddr));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastRebuildException = GetExceptionCode();
        return false;
    }
}

static void ExecuteEditorSceneRebuildNow() {
    static bool s_inRebuild = false;
    if (s_inRebuild) return;
    s_inRebuild = true;
    g_rebuildPending = false;

    ScopedStateLock lock;

    if (RefreshEditorContext() && IsAnyHeapObject(g_ctx.pEditorCat)) {
        uint32_t edVt = 0;
        if (SafeReadU32(g_ctx.pEditorCat, &edVt) && edVt >= 0x10000) {
            Int3 savedCursor = ReadCursorCoord();

            if (!g_pendingSpawns.empty()) {
                std::vector<PendingBlockSpawn> toSpawn;
                toSpawn.swap(g_pendingSpawns);

                uint32_t fnVt68 = 0;
                if (SafeReadU32(edVt + 68 * 4, &fnVt68) && fnVt68 >= 0x10000) {
                    RemoveAllSelectionPulses();
                    g_selectedBlocks.clear();
                    uint32_t spawned = 0;

                    for (size_t sIdx = 0; sIdx < toSpawn.size(); ++sIdx) {
                        const auto& sp = toSpawn[sIdx];
                        if (!IsAnyHeapObject(sp.pBlockInfo)) continue;
                        RefreshEditorContext();
                        uint32_t countBefore = g_ctx.blockCount;

                        uint32_t varArg = sp.flags60 & 0x3Fu;
                        uintptr_t res = 0;

                        if (sp.pos.y >= 2) {
                            res = CallEditorPlaceBlockVt68Seh(
                                g_ctx.pEditorCat, fnVt68, sp.pBlockInfo,
                                sp.pos.x, sp.pos.y, sp.pos.z, sp.dir & 3u, varArg);
                            if (!res && varArg != 0u) {
                                res = CallEditorPlaceBlockVt68Seh(
                                    g_ctx.pEditorCat, fnVt68, sp.pBlockInfo,
                                    sp.pos.x, sp.pos.y, sp.pos.z, sp.dir & 3u, 0u);
                            }
                        }

                        if (!res) {
                            int32_t airY = MaxI32(sp.pos.y + 1, 2);
                            res = CallEditorPlaceBlockVt68Seh(
                                g_ctx.pEditorCat, fnVt68, sp.pBlockInfo,
                                sp.pos.x, airY, sp.pos.z, sp.dir & 3u, varArg);
                            if (!res && varArg != 0u) {
                                res = CallEditorPlaceBlockVt68Seh(
                                    g_ctx.pEditorCat, fnVt68, sp.pBlockInfo,
                                    sp.pos.x, airY, sp.pos.z, sp.dir & 3u, 0u);
                            }
                        }

                        static uint32_t s_spawnSlotCounter = 0;
                        for (int attempt = 0; attempt < 24 && !res; ++attempt) {
                            uint32_t slot = (s_spawnSlotCounter++) % 812u;
                            int32_t tempX = (slot & 1u) ? 30 : 0;
                            int32_t tempZ = 1 + static_cast<int32_t>((slot / 2u) % 29u);
                            int32_t tempY = 28 - static_cast<int32_t>((slot / 58u) % 14u);
                            res = CallEditorPlaceBlockVt68Seh(
                                g_ctx.pEditorCat, fnVt68, sp.pBlockInfo,
                                tempX, tempY, tempZ, sp.dir & 3u, varArg);
                        }

                        if (res && RefreshEditorContext() && g_ctx.blockCount > countBefore) {
                            for (int32_t idx = static_cast<int32_t>(g_ctx.blockCount) - 1;
                                idx >= static_cast<int32_t>(countBefore); --idx) {
                                uint32_t newBlk = 0, bInfo = 0;
                                if (!SafeReadU32(g_ctx.pBlocksArray + idx * 4, &newBlk) || !IsAnyHeapObject(newBlk)) continue;
                                SafeReadU32(newBlk + BLOCK_OFF_BLOCKINFO, &bInfo);
                                if (bInfo != sp.pBlockInfo) continue;

                                Int3 curBPos = ReadBlockCoord(newBlk);
                                float dxW = (sp.pos.x - curBPos.x) * 32.0f;
                                float dyW = (sp.pos.y - curBPos.y) * 8.0f;
                                float dzW = (sp.pos.z - curBPos.z) * 32.0f;

                                WriteBlockCoord(newBlk, sp.pos);
                                WriteBlockDir(newBlk, sp.dir & 3u);

                                // Falls das Quell-Schild einen Skin hatte, Override-Eintrag anlegen
                                if (IsAnyHeapObject(sp.pSkin) || IsAnyHeapObject(sp.pPlugTree)) {
                                    uint32_t srcCorpus4C = 0;
                                    for (uint32_t bIdx = 0; bIdx < countBefore; ++bIdx) {
                                        uint32_t candB = 0, candSkin = 0;
                                        if (SafeReadU32(g_ctx.pBlocksArray + bIdx * 4, &candB) && candB != newBlk &&
                                            SafeReadU32(candB + BLOCK_OFF_SKIN, &candSkin) && candSkin == sp.pSkin) {
                                            srcCorpus4C = ReadBlockCorpus4C(candB);
                                            break;
                                        }
                                    }
                                    ClonedSignVisualOverride ov = { newBlk, sp.pSkin, sp.pPlugTree, srcCorpus4C, sp.flags60 };
                                    g_clonedSignOverrides.push_back(ov);
                                    ApplyClonedSignVisualOverrideToBlock(ov);
                                }
                                else {
                                    if (sp.flags60 != 0u) {
                                        uint32_t curFl60 = 0;
                                        SafeReadU32(newBlk + BLOCK_OFF_FLAGS60, &curFl60);
                                        SafeWriteU32(newBlk + BLOCK_OFF_FLAGS60, (curFl60 & ~0x8FFFu) | (sp.flags60 & 0x8FFFu));
                                    }
                                    uint32_t unwantedSkin = 0;
                                    if (SafeReadU32(newBlk + BLOCK_OFF_SKIN, &unwantedSkin) && unwantedSkin != 0) {
                                        SafeWriteU32(newBlk + BLOCK_OFF_SKIN, 0);
                                    }
                                }

                                if (std::fabs(dxW) > 0.01f || std::fabs(dyW) > 0.01f || std::fabs(dzW) > 0.01f) {
                                    MoveBlockVisualsTargeted(newBlk, dxW, dyW, dzW);
                                }
                                RotateBlockVisualsTargeted(newBlk, sp.dir & 3u);

                                MarkBlockDirty(newBlk);
                                g_selectedBlocks.push_back({ newBlk, 0.0f });
                                ++spawned;
                                break;
                            }
                        }
                    }

                    g_lastSpawnedCount = spawned;
                    if (!g_selectedBlocks.empty()) {
                        g_state = ModState::Moving;
                        ApplyAllSelectionPulses();
                    }
                }
            }

            uintptr_t sampleBlock = (!g_selectedBlocks.empty() && IsAnyHeapObject(g_selectedBlocks[0].blockAddr))
                ? g_selectedBlocks[0].blockAddr : g_lastMovedBlockAddr;

            if (IsAnyHeapObject(sampleBlock)) {
                uint32_t m0 = 0;
                SafeReadU32(sampleBlock + BLOCK_OFF_MOBIL_1, &m0);
                g_mobilBeforeRebuild = m0;
            }

            std::vector<uintptr_t> blocksToUpdate;
            blocksToUpdate.swap(g_dirtyBlocks);
            for (const auto& item : g_selectedBlocks) {
                if (IsAnyHeapObject(item.blockAddr) &&
                    std::find(blocksToUpdate.begin(), blocksToUpdate.end(), item.blockAddr) == blocksToUpdate.end()) {
                    blocksToUpdate.push_back(item.blockAddr);
                }
            }
            for (uintptr_t bAddr : blocksToUpdate) {
                if (IsAnyHeapObject(bAddr)) RefreshBlockMobilsInScene3dMainThread(bAddr);
            }
            ++g_rebuildExecCount;

            uint32_t fnVt31 = 0;
            if (SafeReadU32(edVt + 31 * 4, &fnVt31) && fnVt31 >= 0x10000) {
                CallEditorVTableRebuildSeh(g_ctx.pEditorCat, fnVt31);
            }

            // Einmalig nach VT[31] sicherstellen, dass geklonte Schilder ihren Skin (+0x20) & CPlugTree (+0x5C) behalten
            for (const auto& ov : g_clonedSignOverrides) {
                ApplyClonedSignVisualOverrideToBlock(ov);
            }

            if (IsAnyHeapObject(sampleBlock)) {
                uint32_t m1 = 0;
                SafeReadU32(sampleBlock + BLOCK_OFF_MOBIL_1, &m1);
                g_mobilAfterRebuild = m1;
            }

            WriteCursorCoord(savedCursor);
            UpdateCursorBoxHighlightColor(!g_selectedBlocks.empty());
        }
    }

    s_inRebuild = false;
}

static HWND FindTrackManiaMainWindow() {
    HWND hCur = GetTopWindow(nullptr);
    DWORD myPid = GetCurrentProcessId();
    while (hCur) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hCur, &pid);
        if (pid == myPid && IsWindowVisible(hCur) && GetParent(hCur) == nullptr) return hCur;
        hCur = GetNextWindow(hCur, GW_HWNDNEXT);
    }
    return nullptr;
}

static bool HandleD3D9OverlayWndProcMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK HookedGameWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == (WM_APP + 0x555)) {
        if (g_rebuildPending) ExecuteEditorSceneRebuildNow();
        return 0;
    }
    if (HandleD3D9OverlayWndProcMessage(hWnd, msg, wParam, lParam)) return 0;
    return CallWindowProcA(g_origGameWndProc, hWnd, msg, wParam, lParam);
}

static void EnsureGameWindowSubclassed() {
    HWND hGame = FindTrackManiaMainWindow();
    if (!hGame) return;
    if (g_hookedGameWnd != hGame || !g_origGameWndProc) {
        WNDPROC prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(hGame, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedGameWndProc)));
        if (prev && prev != HookedGameWndProc) {
            g_origGameWndProc = prev;
            g_hookedGameWnd = hGame;
        }
    }
}

static void RequestLiveEditorRebuild() {
    EnsureGameWindowSubclassed();
    if (g_hookedGameWnd && g_origGameWndProc) {
        g_rebuildPending = true;
        PostMessageA(g_hookedGameWnd, WM_APP + 0x555, 0, 0);
    }
}

// ============================================================================
// 5. Auswahl-Verwaltung, Drag-Select, Blaupausen & Block-Operationen
// ============================================================================

// 20-Byte Layout für 100% Binärkompatibilität mit bestehenden .tmgroup Dateien
struct ClipboardBlockData {
    Int3      relativeCoord;
    uint32_t  dir;
    uint32_t  blockInfoId;      // Block + 0x14 (MwId des Blocktyps)
    uintptr_t blockInfoPtr;     // Block + 0x24 (CGameCtnBlockInfo*)
    uint32_t  reservedFlags60;  // Block + 0x60 (VariantIndex + Flags)
};

static std::vector<ClipboardBlockData> g_clipboard;
static std::vector<uintptr_t>          g_clipboardSkins;     // Parallel-Array für CGameCtnBlockSkin* (+0x20)
static std::vector<uintptr_t>          g_clipboardPlugTrees; // Parallel-Array für CPlugTree* (CPlugSolid+0x5C)
static std::string g_baseDir = ".\\";
static std::string g_iniPath = ".\\BlockMover.ini";
static bool        g_running = true;

static bool      g_dragActive = false;
static bool      g_dragAddMode = true;
static uintptr_t g_lastDragBlock = 0;
static bool      g_hasBoxCorner1 = false;
static Int3      g_boxCorner1 = { 0, 1, 0 };

static D3DMATRIX g_candView{};
static bool      g_hasCandView = false;
static D3DMATRIX g_d3dView{};
static D3DMATRIX g_d3dProj{};
static bool      g_d3dHas3DMatrices = false;

static void PlayBeepSound(DWORD freq, DWORD dur) {
    if (g_cfg.soundFeedback) Beep(freq, dur);
}

static void RefreshBlueprintFileList() {
    g_blueprintFiles.clear();
    std::string searchPattern = g_baseDir + "Blueprints\\*.tmgroup";
    WIN32_FIND_DATAA fd{};
    HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string fn(fd.cFileName);
                if (fn.size() > 8 && fn.substr(fn.size() - 8) == ".tmgroup") fn.resize(fn.size() - 8);
                g_blueprintFiles.push_back(fn);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    std::sort(g_blueprintFiles.begin(), g_blueprintFiles.end());
}

static void InitPaths(HMODULE hMod) {
    char path[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(hMod, path, MAX_PATH) > 0) {
        std::string fullPath(path);
        size_t pos = fullPath.find_last_of("\\/");
        if (pos != std::string::npos) {
            g_baseDir = fullPath.substr(0, pos + 1);
            g_iniPath = g_baseDir + "BlockMover.ini";
        }
    }
    RefreshBlueprintFileList();
}

static void ApplyKeybindPreset(int presetId) {
    g_cfg.keyToggleMenu = VK_F3;
    g_cfg.keyToggleModMode = VK_F6;
    g_cfg.keyBackToSelect = VK_BACK;
    if (presetId == 0) { // Standard-Preset
        g_cfg.keySelectBlock = 'E';
        g_cfg.keyConfirmSelection = VK_NUMPAD0;
        g_cfg.keyMoveNorth = 'W';
        g_cfg.keyMoveSouth = 'V';
        g_cfg.keyMoveWest = 'A';
        g_cfg.keyMoveEast = 'D';
        g_cfg.keyMoveUp = VK_PRIOR;
        g_cfg.keyMoveDown = VK_NEXT;
        g_cfg.keyRotateCW = 'R';
        g_cfg.keyRotateCCW = 'Q';
        g_cfg.keyTeleportToCursor = VK_NUMPAD5;
        g_cfg.keyCloneAtCursor = 'L';
        g_cfg.keyPasteBlueprint = 'I';
    }
    else { // Numpad-Alternative
        g_cfg.keySelectBlock = 'F';
        g_cfg.keyConfirmSelection = VK_NUMPAD0;
        g_cfg.keyMoveNorth = VK_NUMPAD8;
        g_cfg.keyMoveSouth = VK_NUMPAD2;
        g_cfg.keyMoveWest = VK_NUMPAD4;
        g_cfg.keyMoveEast = VK_NUMPAD6;
        g_cfg.keyMoveUp = VK_NUMPAD9;
        g_cfg.keyMoveDown = VK_NUMPAD3;
        g_cfg.keyRotateCW = VK_NUMPAD7;
        g_cfg.keyRotateCCW = VK_NUMPAD1;
        g_cfg.keyTeleportToCursor = VK_NUMPAD5;
        g_cfg.keyCloneAtCursor = 0;
        g_cfg.keyPasteBlueprint = 0;
    }
}

static void CycleStepMultiplier() {
    const int presets[5] = { 1, 2, 4, 8, 16 };
    int next = 1;
    for (int i = 0; i < 5; ++i) {
        if (g_cfg.stepMultiplier == presets[i]) {
            next = presets[(i + 1) % 5];
            break;
        }
    }
    g_cfg.stepMultiplier = next;
    PlayBeepSound(700 + next * 30, 35);
}

static void SaveConfig() {
    auto WriteInt = [](const char* k, int v) {
        char buf[32]; sprintf_s(buf, "%d", v);
        WritePrivateProfileStringA("BlockMover", k, buf, g_iniPath.c_str());
        };
    WriteInt("ModEnabled", g_cfg.modEnabled);
    WriteInt("Language", g_cfg.language);
    WriteInt("RuleMode", g_cfg.ruleMode);
    WriteInt("CameraRelativeMove", g_cfg.cameraRelativeMove);
    WriteInt("CursorFollowsMove", g_cfg.cursorFollowsMove);
    WriteInt("RotateAsGroup", g_cfg.rotateAsGroup);
    WriteInt("SoundFeedback", g_cfg.soundFeedback);
    WriteInt("VisualHighlight", g_cfg.visualHighlight);
    WriteInt("ShowBlueBoxes", g_cfg.showBlueBoxes);
    WriteInt("OnlyPlayerBlocks", g_cfg.onlyPlayerBlocks);
    WriteInt("StepMultiplier", g_cfg.stepMultiplier);
    WriteInt("CenterBlueprintAtCursor", g_centerBlueprintAtCursor);

    WriteInt("KeyToggleMenu", g_cfg.keyToggleMenu);
    WriteInt("KeyToggleModMode", g_cfg.keyToggleModMode);
    WriteInt("KeySelectBlock", g_cfg.keySelectBlock);
    WriteInt("KeyConfirmSelection", g_cfg.keyConfirmSelection);
    WriteInt("KeyBackToSelect", g_cfg.keyBackToSelect);
    WriteInt("KeyMoveNorth", g_cfg.keyMoveNorth);
    WriteInt("KeyMoveSouth", g_cfg.keyMoveSouth);
    WriteInt("KeyMoveWest", g_cfg.keyMoveWest);
    WriteInt("KeyMoveEast", g_cfg.keyMoveEast);
    WriteInt("KeyMoveUp", g_cfg.keyMoveUp);
    WriteInt("KeyMoveDown", g_cfg.keyMoveDown);
    WriteInt("KeyRotateCW", g_cfg.keyRotateCW);
    WriteInt("KeyRotateCCW", g_cfg.keyRotateCCW);

    WriteInt("KeyTeleportToCursor", g_cfg.keyTeleportToCursor);
    WriteInt("KeyCloneAtCursor", g_cfg.keyCloneAtCursor);
    WriteInt("KeySelectLastBuilt", g_cfg.keySelectLastBuilt);
    WriteInt("KeySelectAllBlocks", g_cfg.keySelectAllBlocks);
    WriteInt("KeySelectSameHeight", g_cfg.keySelectSameHeight);
    WriteInt("KeySelectSameType", g_cfg.keySelectSameType);
    WriteInt("KeyBoxCorner1", g_cfg.keyBoxCorner1);
    WriteInt("KeyBoxSelect2", g_cfg.keyBoxSelect2);
    WriteInt("KeyStepSizeCycle", g_cfg.keyStepSizeCycle);
    WriteInt("KeyCopySelection", g_cfg.keyCopySelection);
    WriteInt("KeyPasteBlueprint", g_cfg.keyPasteBlueprint);
}

static void LoadConfig() {
    const char* ini = g_iniPath.c_str();
    g_cfg.modEnabled = GetPrivateProfileIntA("BlockMover", "ModEnabled", 1, ini) != 0;
    g_cfg.language = ClampI32(GetPrivateProfileIntA("BlockMover", "Language", 1, ini), 0, 1);
    g_cfg.ruleMode = GetPrivateProfileIntA("BlockMover", "RuleMode", 1, ini);
    if (GetModuleHandleA("TMUnlimiter.dll") != nullptr) {
        g_cfg.ruleMode = static_cast<int>(BuildRuleMode::TMUnlimiter);
    }
    g_cfg.cameraRelativeMove = GetPrivateProfileIntA("BlockMover", "CameraRelativeMove", 1, ini) != 0;
    g_cfg.cursorFollowsMove = GetPrivateProfileIntA("BlockMover", "CursorFollowsMove", 1, ini) != 0;
    g_cfg.rotateAsGroup = GetPrivateProfileIntA("BlockMover", "RotateAsGroup", 1, ini) != 0;
    g_cfg.soundFeedback = GetPrivateProfileIntA("BlockMover", "SoundFeedback", 1, ini) != 0;
    g_cfg.visualHighlight = GetPrivateProfileIntA("BlockMover", "VisualHighlight", 1, ini) != 0;
    g_cfg.showBlueBoxes = GetPrivateProfileIntA("BlockMover", "ShowBlueBoxes", 1, ini) != 0;
    g_cfg.onlyPlayerBlocks = GetPrivateProfileIntA("BlockMover", "OnlyPlayerBlocks", 1, ini) != 0;
    g_cfg.stepMultiplier = ClampI32(GetPrivateProfileIntA("BlockMover", "StepMultiplier", 1, ini), 1, 32);
    g_centerBlueprintAtCursor = GetPrivateProfileIntA("BlockMover", "CenterBlueprintAtCursor", 1, ini) != 0;

    g_cfg.keyToggleMenu = GetPrivateProfileIntA("BlockMover", "KeyToggleMenu", VK_F3, ini);
    g_cfg.keyToggleModMode = GetPrivateProfileIntA("BlockMover", "KeyToggleModMode", VK_F6, ini);
    g_cfg.keySelectBlock = GetPrivateProfileIntA("BlockMover", "KeySelectBlock", 'E', ini);
    g_cfg.keyConfirmSelection = GetPrivateProfileIntA("BlockMover", "KeyConfirmSelection", VK_NUMPAD0, ini);
    g_cfg.keyBackToSelect = GetPrivateProfileIntA("BlockMover", "KeyBackToSelect", VK_BACK, ini);
    g_cfg.keyMoveNorth = GetPrivateProfileIntA("BlockMover", "KeyMoveNorth", 'W', ini);
    g_cfg.keyMoveSouth = GetPrivateProfileIntA("BlockMover", "KeyMoveSouth", 'V', ini);
    g_cfg.keyMoveWest = GetPrivateProfileIntA("BlockMover", "KeyMoveWest", 'A', ini);
    g_cfg.keyMoveEast = GetPrivateProfileIntA("BlockMover", "KeyMoveEast", 'D', ini);
    g_cfg.keyMoveUp = GetPrivateProfileIntA("BlockMover", "KeyMoveUp", VK_PRIOR, ini);
    g_cfg.keyMoveDown = GetPrivateProfileIntA("BlockMover", "KeyMoveDown", VK_NEXT, ini);
    g_cfg.keyRotateCW = GetPrivateProfileIntA("BlockMover", "KeyRotateCW", 'R', ini);
    g_cfg.keyRotateCCW = GetPrivateProfileIntA("BlockMover", "KeyRotateCCW", 'Q', ini);

    g_cfg.keyTeleportToCursor = GetPrivateProfileIntA("BlockMover", "KeyTeleportToCursor", VK_NUMPAD5, ini);
    g_cfg.keyCloneAtCursor = GetPrivateProfileIntA("BlockMover", "KeyCloneAtCursor", 'L', ini);
    g_cfg.keySelectLastBuilt = GetPrivateProfileIntA("BlockMover", "KeySelectLastBuilt", 0, ini);
    g_cfg.keySelectAllBlocks = GetPrivateProfileIntA("BlockMover", "KeySelectAllBlocks", 0, ini);
    g_cfg.keySelectSameHeight = GetPrivateProfileIntA("BlockMover", "KeySelectSameHeight", 0, ini);
    g_cfg.keySelectSameType = GetPrivateProfileIntA("BlockMover", "KeySelectSameType", 0, ini);
    g_cfg.keyBoxCorner1 = GetPrivateProfileIntA("BlockMover", "KeyBoxCorner1", 0, ini);
    g_cfg.keyBoxSelect2 = GetPrivateProfileIntA("BlockMover", "KeyBoxSelect2", 0, ini);
    g_cfg.keyStepSizeCycle = GetPrivateProfileIntA("BlockMover", "KeyStepSizeCycle", 0, ini);
    g_cfg.keyCopySelection = GetPrivateProfileIntA("BlockMover", "KeyCopySelection", 0, ini);
    g_cfg.keyPasteBlueprint = GetPrivateProfileIntA("BlockMover", "KeyPasteBlueprint", 'I', ini);
}

static void SanitizeSelection() {
    if (!g_ctx.pBlocksArray || g_ctx.blockCount == 0 || g_ctx.blockCount > 50000) {
        g_selectedBlocks.clear();
        return;
    }
    g_selectedBlocks.erase(
        std::remove_if(g_selectedBlocks.begin(), g_selectedBlocks.end(), [](const SelectedBlockEntry& sel) {
            if (!IsAnyHeapObject(sel.blockAddr)) return true;
            for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
                uint32_t b = 0;
                if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && b == sel.blockAddr) return false;
            }
            return true;
            }),
        g_selectedBlocks.end()
    );
}

static uintptr_t FindBlockAtOrNearCursor(const Int3& target) {
    if (!g_ctx.pBlocksArray || g_ctx.blockCount == 0) return 0;

    uintptr_t exact3DMatch = 0;
    uintptr_t sameColumnNear = 0;
    int32_t   bestColNearDist = 999999;
    uintptr_t nearColumnBest = 0;
    int32_t   bestNearScore = 999999;
    uintptr_t sameColumnAnyY = 0;
    int32_t   bestColAnyDist = 999999;

    for (int32_t i = static_cast<int32_t>(g_ctx.blockCount) - 1; i >= 0; --i) {
        uint32_t block = 0;
        if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &block) || !IsMovableTrackBlock(block)) continue;

        Int3 pos = ReadBlockCoord(block);
        int32_t distXZ = std::abs(pos.x - target.x) + std::abs(pos.z - target.z);
        int32_t distY = std::abs(pos.y - target.y);

        if (distXZ == 0 && distY == 0) {
            exact3DMatch = block;
            break;
        }
        if (distXZ == 0 && distY <= 4 && distY < bestColNearDist) {
            bestColNearDist = distY;
            sameColumnNear = block;
        }
        if (distXZ <= 1 && distY <= 2) {
            int32_t score = distXZ * 12 + distY;
            if (score < bestNearScore) {
                bestNearScore = score;
                nearColumnBest = block;
            }
        }
        if (distXZ == 0 && distY < bestColAnyDist) {
            bestColAnyDist = distY;
            sameColumnAnyY = block;
        }
    }

    if (exact3DMatch)   return exact3DMatch;
    if (sameColumnNear) return sameColumnNear;
    if (nearColumnBest) return nearColumnBest;
    return sameColumnAnyY;
}

static void SelectLastPlacedBlock() {
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) {
        PlayBeepSound(240, 80);
        return;
    }
    RemoveAllSelectionPulses();
    g_selectedBlocks.clear();
    for (int32_t i = static_cast<int32_t>(g_ctx.blockCount) - 1; i >= 0; --i) {
        uint32_t b = 0;
        if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && IsMovableTrackBlock(b)) {
            g_selectedBlocks.push_back({ b, 0.0f });
            if (g_cfg.cursorFollowsMove) {
                Int3 bPos = ReadBlockCoord(b);
                WriteCursorCoord(bPos);
                g_lastWorldCursorPos = bPos;
            }
            break;
        }
    }
    if (!g_selectedBlocks.empty()) {
        g_state = ModState::Moving;
        ApplyAllSelectionPulses();
        RequestLiveEditorRebuild();
        PlayBeepSound(1000, 70);
    }
}

static void SelectAllMapBlocks() {
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) {
        PlayBeepSound(240, 80);
        return;
    }
    RemoveAllSelectionPulses();
    g_selectedBlocks.clear();
    for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
        uint32_t b = 0;
        if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && IsMovableTrackBlock(b)) {
            g_selectedBlocks.push_back({ b, 0.0f });
        }
    }
    if (!g_selectedBlocks.empty()) {
        g_state = ModState::Moving;
        ApplyAllSelectionPulses();
        RequestLiveEditorRebuild();
        PlayBeepSound(1150, 80);
    }
}

static void SelectBlocksOnSameHeightY() {
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) {
        PlayBeepSound(240, 80);
        return;
    }
    SanitizeSelection();
    int32_t targetY = (!g_selectedBlocks.empty())
        ? ReadBlockCoord(g_selectedBlocks[0].blockAddr).y
        : ReadCursorCoord().y;

    RemoveAllSelectionPulses();
    g_selectedBlocks.clear();
    for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
        uint32_t b = 0;
        if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && IsMovableTrackBlock(b)) {
            if (ReadBlockCoord(b).y == targetY) {
                g_selectedBlocks.push_back({ b, 0.0f });
            }
        }
    }
    if (!g_selectedBlocks.empty()) {
        g_state = ModState::Moving;
        ApplyAllSelectionPulses();
        RequestLiveEditorRebuild();
        PlayBeepSound(1100, 70);
    }
}

static void SelectBlocksOfSameType() {
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) {
        PlayBeepSound(240, 80);
        return;
    }
    SanitizeSelection();
    uintptr_t refBlock = (!g_selectedBlocks.empty())
        ? g_selectedBlocks[0].blockAddr
        : FindBlockAtOrNearCursor(ReadCursorCoord());
    if (!IsAnyHeapObject(refBlock)) {
        PlayBeepSound(240, 80);
        return;
    }
    uint32_t targetInfo = 0;
    SafeReadU32(refBlock + BLOCK_OFF_BLOCKINFO, &targetInfo);
    if (!IsAnyHeapObject(targetInfo)) return;

    RemoveAllSelectionPulses();
    g_selectedBlocks.clear();
    for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
        uint32_t b = 0, bInfo = 0;
        if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && IsMovableTrackBlock(b) &&
            SafeReadU32(b + BLOCK_OFF_BLOCKINFO, &bInfo) && bInfo == targetInfo) {
            g_selectedBlocks.push_back({ b, 0.0f });
        }
    }
    if (!g_selectedBlocks.empty()) {
        g_state = ModState::Moving;
        ApplyAllSelectionPulses();
        RequestLiveEditorRebuild();
        PlayBeepSound(1150, 70);
    }
}

static void SetBoxCorner1AtCursor() {
    ScopedStateLock lock;
    if (!RefreshEditorContext()) { PlayBeepSound(240, 80); return; }
    Int3 cur = ReadCursorCoord();
    if (IsAnyHeapObject(g_ctx.pCursor)) g_lastWorldCursorPos = cur;
    g_boxCorner1 = g_lastWorldCursorPos;
    g_hasBoxCorner1 = true;
    if (g_state == ModState::Inactive) g_state = ModState::Selecting;
    PlayBeepSound(850, 60);
}

static void SelectBlocksInBoxToCursor() {
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) {
        PlayBeepSound(240, 80);
        return;
    }
    if (!g_hasBoxCorner1) {
        SetBoxCorner1AtCursor();
        return;
    }
    RemoveAllSelectionPulses();
    SanitizeSelection();

    Int3 c2 = ReadCursorCoord();
    if (IsAnyHeapObject(g_ctx.pCursor)) g_lastWorldCursorPos = c2;
    int32_t minX = MinI32(g_boxCorner1.x, c2.x), maxX = MaxI32(g_boxCorner1.x, c2.x);
    int32_t minZ = MinI32(g_boxCorner1.z, c2.z), maxZ = MaxI32(g_boxCorner1.z, c2.z);

    uint32_t added = 0;
    for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
        uint32_t block = 0;
        if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &block) || !IsMovableTrackBlock(block)) continue;
        Int3 pos = ReadBlockCoord(block);
        if (pos.x >= minX && pos.x <= maxX && pos.z >= minZ && pos.z <= maxZ) {
            if (!IsBlockSelected(block)) {
                g_selectedBlocks.push_back({ block, 0.0f });
                ++added;
            }
        }
    }
    if (!g_selectedBlocks.empty()) {
        g_state = ModState::Moving;
        ApplyAllSelectionPulses();
        RequestLiveEditorRebuild();
    }
    PlayBeepSound(added > 0 ? 1150 : 350, 80);
}

static void ToggleBlockAtCursor() {
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) {
        PlayBeepSound(240, 80);
        return;
    }
    SanitizeSelection();

    Int3 target = ReadCursorCoord();
    if (IsAnyHeapObject(g_ctx.pCursor)) g_lastWorldCursorPos = target;
    uintptr_t chosen = FindBlockAtOrNearCursor(target);
    if (!chosen) {
        PlayBeepSound(300, 60);
        return;
    }

    for (auto it = g_selectedBlocks.begin(); it != g_selectedBlocks.end(); ++it) {
        if (it->blockAddr == chosen) {
            RemovePulseFromEntry(*it);
            g_selectedBlocks.erase(it);
            RequestLiveEditorRebuild();
            PlayBeepSound(500, 50);
            return;
        }
    }

    g_selectedBlocks.push_back({ chosen, 0.0f });
    ApplyPulseToEntry(g_selectedBlocks.back());
    g_state = ModState::Moving;
    if (g_cfg.cursorFollowsMove) {
        Int3 bPos = ReadBlockCoord(chosen);
        WriteCursorCoord(bPos);
        g_lastWorldCursorPos = bPos;
    }
    RequestLiveEditorRebuild();
    PlayBeepSound(950, 60);
}

static void HandleContinuousDragSelect(bool selectKeyDown) {
    if (!selectKeyDown) {
        if (g_dragActive && !g_selectedBlocks.empty()) g_state = ModState::Moving;
        g_dragActive = false;
        g_lastDragBlock = 0;
        return;
    }
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) return;

    Int3 cur = ReadCursorCoord();
    uintptr_t block = FindBlockAtOrNearCursor(cur);
    if (!block || block == g_lastDragBlock) return;
    g_lastDragBlock = block;

    SanitizeSelection();

    bool alreadySelected = IsBlockSelected(block);
    if (!g_dragActive) {
        g_dragActive = true;
        g_dragAddMode = !alreadySelected;
    }

    if (g_dragAddMode && !alreadySelected) {
        g_selectedBlocks.push_back({ block, 0.0f });
        ApplyPulseToEntry(g_selectedBlocks.back());
        g_state = ModState::Moving;
        if (g_cfg.cursorFollowsMove && g_selectedBlocks.size() == 1) {
            Int3 bPos = ReadBlockCoord(block);
            WriteCursorCoord(bPos);
            g_lastWorldCursorPos = bPos;
        }
        RequestLiveEditorRebuild();
        PlayBeepSound(950, 25);
    }
    else if (!g_dragAddMode && alreadySelected) {
        for (auto it = g_selectedBlocks.begin(); it != g_selectedBlocks.end(); ++it) {
            if (it->blockAddr == block) {
                RemovePulseFromEntry(*it);
                g_selectedBlocks.erase(it);
                RequestLiveEditorRebuild();
                PlayBeepSound(500, 25);
                break;
            }
        }
    }
}

static uintptr_t ResolveBlockInfoForClipboardEntry(const ClipboardBlockData& item) {
    if (IsAnyHeapObject(item.blockInfoPtr)) {
        const char* cn = GetHeapObjectClassName(item.blockInfoPtr);
        if (cn && ClassContainsWord(cn, "BlockInfo") &&
            !ClassContainsWord(cn, "BlockInfoFlat") && !ClassContainsWord(cn, "BlockInfoClip")) {
            return item.blockInfoPtr;
        }
    }
    if (item.blockInfoId != 0 && g_ctx.pBlocksArray && g_ctx.blockCount > 0) {
        for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
            uint32_t b = 0, bId = 0, bInfo = 0;
            if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && IsAnyHeapObject(b) &&
                SafeReadU32(b + 0x14, &bId) && bId == item.blockInfoId &&
                SafeReadU32(b + BLOCK_OFF_BLOCKINFO, &bInfo) && IsAnyHeapObject(bInfo)) {
                return bInfo;
            }
        }
    }
    if (!g_selectedBlocks.empty() && IsAnyHeapObject(g_selectedBlocks[0].blockAddr)) {
        uint32_t bInfo = 0;
        if (SafeReadU32(g_selectedBlocks[0].blockAddr + BLOCK_OFF_BLOCKINFO, &bInfo) && IsAnyHeapObject(bInfo)) {
            return bInfo;
        }
    }
    if (g_ctx.pBlocksArray && g_ctx.blockCount > 0) {
        for (int32_t i = static_cast<int32_t>(g_ctx.blockCount) - 1; i >= 0; --i) {
            uint32_t b = 0, bInfo = 0;
            if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && IsMovableTrackBlock(b) &&
                SafeReadU32(b + BLOCK_OFF_BLOCKINFO, &bInfo) && IsAnyHeapObject(bInfo)) {
                return bInfo;
            }
        }
    }
    return 0;
}

static void SaveBlueprintToFile(const std::string& groupName) {
    if (g_clipboard.empty() || groupName.empty()) return;
    std::string bpDir = g_baseDir + "Blueprints";
    CreateDirectoryA(bpDir.c_str(), nullptr);
    std::string path = bpDir + "\\" + groupName + ".tmgroup";
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return;
    size_t count = g_clipboard.size();
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    file.write(reinterpret_cast<const char*>(g_clipboard.data()), count * sizeof(ClipboardBlockData));
    file.close();
    RefreshBlueprintFileList();
}

static void LoadBlueprintFromFile(const std::string& groupName) {
    if (groupName.empty()) return;
    std::string path = g_baseDir + "Blueprints\\" + groupName + ".tmgroup";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) { PlayBeepSound(240, 80); return; }
    std::streamsize totalSize = file.tellg();
    file.seekg(0, std::ios::beg);
    if (totalSize <= static_cast<std::streamsize>(sizeof(size_t))) { PlayBeepSound(240, 80); return; }

    size_t count = 0;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    std::streamsize rem = totalSize - static_cast<std::streamsize>(sizeof(size_t));

    if (count > 0 && count < 100000) {
        g_clipboard.clear();
        g_clipboard.resize(count);
        g_clipboardSkins.assign(count, 0);
        g_clipboardPlugTrees.assign(count, 0);
        if (rem == static_cast<std::streamsize>(count * sizeof(ClipboardBlockData))) {
            file.read(reinterpret_cast<char*>(g_clipboard.data()), count * sizeof(ClipboardBlockData));
        }
        else if (rem == static_cast<std::streamsize>(count * 16)) {
            struct LegacyClip { Int3 rel; uint32_t dir; };
            for (size_t i = 0; i < count; ++i) {
                LegacyClip lc{};
                file.read(reinterpret_cast<char*>(&lc), sizeof(lc));
                g_clipboard[i] = { lc.rel, lc.dir, 0u, 0u, 0u };
            }
        }
        PlayBeepSound(1000, 80);
    }
}

static void CopySelectedBlocks() {
    ScopedStateLock lock;
    if (!RefreshEditorContext()) return;
    SanitizeSelection();
    if (g_selectedBlocks.empty()) { PlayBeepSound(240, 80); return; }
    g_clipboard.clear();
    g_clipboardSkins.clear();
    g_clipboardPlugTrees.clear();

    Int3 anchor = ReadBlockCoord(g_selectedBlocks[0].blockAddr);
    for (const auto& entry : g_selectedBlocks) {
        Int3 p = ReadBlockCoord(entry.blockAddr);
        if (p.y < anchor.y) anchor.y = p.y;
    }

    for (const auto& entry : g_selectedBlocks) {
        Int3 pos = ReadBlockCoord(entry.blockAddr);
        uint32_t bId = 0, bInfo = 0, fl60 = 0, bSkin = 0;
        SafeReadU32(entry.blockAddr + 0x14, &bId);
        SafeReadU32(entry.blockAddr + BLOCK_OFF_SKIN, &bSkin);
        SafeReadU32(entry.blockAddr + BLOCK_OFF_BLOCKINFO, &bInfo);
        SafeReadU32(entry.blockAddr + BLOCK_OFF_FLAGS60, &fl60);
        g_clipboard.push_back({
            { pos.x - anchor.x, pos.y - anchor.y, pos.z - anchor.z },
            ReadBlockDir(entry.blockAddr), bId, bInfo, fl60
            });
        g_clipboardSkins.push_back(bSkin);
        g_clipboardPlugTrees.push_back(ReadBlockPlugTree(entry.blockAddr));
    }
    PlayBeepSound(1100, 90);
}

static void PasteFormationToSelection() {
    ScopedStateLock lock;
    if (!RefreshEditorContext()) return;
    SanitizeSelection();
    if (g_clipboard.empty()) { PlayBeepSound(240, 80); return; }

    // Immer live den echten Spiel-Cursor lesen (und g_lastWorldCursorPos aktualisieren)
    Int3 liveCur = ReadCursorCoord();
    if (IsAnyHeapObject(g_ctx.pCursor)) {
        g_lastWorldCursorPos = liveCur;
    }
    Int3 targetOrigin = g_lastWorldCursorPos;
    if (targetOrigin.x > 31 || targetOrigin.y > 31 || targetOrigin.z > 31) {
        g_cfg.ruleMode = static_cast<int>(BuildRuleMode::TMUnlimiter);
    }

    bool isVanilla = (g_cfg.ruleMode == static_cast<int>(BuildRuleMode::Vanilla));
    int32_t minX = isVanilla ? VANILLA_MIN_X : 0, maxX = isVanilla ? VANILLA_MAX_X : UNLIMITER_MAX;
    int32_t minY = 1, maxY = isVanilla ? VANILLA_MAX_Y : UNLIMITER_MAX;
    int32_t minZ = isVanilla ? VANILLA_MIN_Z : 0, maxZ = isVanilla ? VANILLA_MAX_Z : UNLIMITER_MAX;

    int32_t minRelX = g_clipboard[0].relativeCoord.x, maxRelX = minRelX;
    int32_t minRelY = g_clipboard[0].relativeCoord.y;
    int32_t minRelZ = g_clipboard[0].relativeCoord.z, maxRelZ = minRelZ;

    for (size_t i = 1; i < g_clipboard.size(); ++i) {
        const Int3& r = g_clipboard[i].relativeCoord;
        if (r.x < minRelX) minRelX = r.x;
        if (r.x > maxRelX) maxRelX = r.x;
        if (r.y < minRelY) minRelY = r.y;
        if (r.z < minRelZ) minRelZ = r.z;
        if (r.z > maxRelZ) maxRelZ = r.z;
    }

    int32_t baseOffsetX = g_centerBlueprintAtCursor ? ((minRelX + maxRelX) / 2) : g_clipboard[0].relativeCoord.x;
    int32_t baseOffsetZ = g_centerBlueprintAtCursor ? ((minRelZ + maxRelZ) / 2) : g_clipboard[0].relativeCoord.z;

    RemoveAllSelectionPulses();
    g_pendingSpawns.clear();

    for (size_t i = 0; i < g_clipboard.size(); ++i) {
        uintptr_t bInfo = ResolveBlockInfoForClipboardEntry(g_clipboard[i]);
        if (!IsAnyHeapObject(bInfo)) continue;

        int32_t relX = g_clipboard[i].relativeCoord.x - baseOffsetX;
        int32_t relY = g_clipboard[i].relativeCoord.y - minRelY;
        int32_t relZ = g_clipboard[i].relativeCoord.z - baseOffsetZ;

        Int3 newPos = {
            ClampI32(targetOrigin.x + relX, minX, maxX),
            ClampI32(targetOrigin.y + relY, minY, maxY),
            ClampI32(targetOrigin.z + relZ, minZ, maxZ)
        };
        uintptr_t skinPtr = (i < g_clipboardSkins.size()) ? g_clipboardSkins[i] : 0;
        uintptr_t treePtr = (i < g_clipboardPlugTrees.size()) ? g_clipboardPlugTrees[i] : 0;
        g_pendingSpawns.push_back({ bInfo, newPos, g_clipboard[i].dir & 3u, g_clipboard[i].reservedFlags60, skinPtr, treePtr });
    }

    g_selectedBlocks.clear();
    if (g_pendingSpawns.empty()) { PlayBeepSound(240, 80); return; }
    RequestLiveEditorRebuild();
    PlayBeepSound(1300, 90);
}

static void MoveSelectedBlocks(int32_t dirX, int32_t dirY, int32_t dirZ) {
    ScopedStateLock lock;
    if (!RefreshEditorContext()) return;
    RemoveAllSelectionPulses();
    SanitizeSelection();
    if (g_selectedBlocks.empty()) { PlayBeepSound(240, 60); return; }

    // Falls Cursor oder ein Block bereits außerhalb von 0..31 liegt, automatisch TMUnlimiter-Grenzen (0..255) nutzen
    Int3 curCheck = ReadCursorCoord();
    if (curCheck.x > 31 || curCheck.y > 31 || curCheck.z > 31) {
        g_cfg.ruleMode = static_cast<int>(BuildRuleMode::TMUnlimiter);
    }

    bool isVanilla = (g_cfg.ruleMode == static_cast<int>(BuildRuleMode::Vanilla));
    int32_t minX = isVanilla ? VANILLA_MIN_X : 0, maxX = isVanilla ? VANILLA_MAX_X : UNLIMITER_MAX;
    int32_t minY = isVanilla ? VANILLA_MIN_Y : 1, maxY = isVanilla ? VANILLA_MAX_Y : UNLIMITER_MAX;
    int32_t minZ = isVanilla ? VANILLA_MIN_Z : 0, maxZ = isVanilla ? VANILLA_MAX_Z : UNLIMITER_MAX;

    int32_t dx = dirX * g_cfg.stepMultiplier;
    int32_t dy = dirY * g_cfg.stepMultiplier;
    int32_t dz = dirZ * g_cfg.stepMultiplier;

    // 1. Zuerst prüfen, ob ALLE ausgewählten Blöcke innerhalb der Grenzen bleiben,
    //    damit eine Formation am Rand niemals auseinandergerissen wird!
    for (const auto& entry : g_selectedBlocks) {
        Int3 oldPos = ReadBlockCoord(entry.blockAddr);
        if (oldPos.x > 31 || oldPos.y > 31 || oldPos.z > 31) {
            minX = 0; maxX = UNLIMITER_MAX;
            minY = 1; maxY = UNLIMITER_MAX;
            minZ = 0; maxZ = UNLIMITER_MAX;
        }
        if (oldPos.x + dx < minX || oldPos.x + dx > maxX ||
            oldPos.y + dy < minY || oldPos.y + dy > maxY ||
            oldPos.z + dz < minZ || oldPos.z + dz > maxZ) {
            ApplyAllSelectionPulses();
            PlayBeepSound(240, 80);
            return;
        }
    }

    int totalVisualHits = 0;
    int movedBlocksCount = 0;

    // 2. Alle Blöcke synchron bewegen
    for (auto& entry : g_selectedBlocks) {
        uintptr_t block = entry.blockAddr;
        Int3 oldPos = ReadBlockCoord(block);
        Int3 newPos = { oldPos.x + dx, oldPos.y + dy, oldPos.z + dz };
        WriteBlockCoord(block, newPos);
        totalVisualHits += MoveBlockVisualsTargeted(block, dx * 32.0f, dy * 8.0f, dz * 32.0f);
        ++movedBlocksCount;
    }

    g_lastMovedFloats = totalVisualHits;

    if (g_cfg.cursorFollowsMove) {
        Int3 c = ReadBlockCoord(g_selectedBlocks[0].blockAddr);
        WriteCursorCoord(c);
        g_lastWorldCursorPos = c;
    }

    ApplyAllSelectionPulses();
    RequestLiveEditorRebuild();
    PlayBeepSound(880, 30);
}

// Berechnet Vorwärts/Rückwärts & Links/Rechts direkt aus der 3D-Bildschirmprojektion (cx / cw)
static void MoveSelectedBlocksCameraRelative(int32_t forwardStep, int32_t rightStep, int32_t upStep) {
    int32_t dx = 0, dz = 0;
    if (g_cfg.cameraRelativeMove && g_d3dHas3DMatrices) {
        // 1. Rechts-Vektor auf dem Monitor (Richtung, in der clip.x = vx * proj._11 wächst)
        float sRight = (g_d3dProj._11 >= 0.0f) ? 1.0f : -1.0f;
        float rx = g_d3dView._11 * sRight;
        float rz = g_d3dView._31 * sRight;

        // 2. Vorwärts-Vektor in den Monitor hinein (Richtung, in der Tiefe clip.w = vz * proj._34 wächst)
        float sFwd = (g_d3dProj._34 >= 0.0f) ? 1.0f : -1.0f;
        float fx = g_d3dView._13 * sFwd;
        float fz = g_d3dView._33 * sFwd;

        int32_t fwdDx = 0, fwdDz = 0;
        if (std::fabs(fx) >= std::fabs(fz)) fwdDx = (fx >= 0.0f) ? 1 : -1;
        else                                fwdDz = (fz >= 0.0f) ? 1 : -1;

        int32_t rightDx = 0, rightDz = 0;
        if (std::fabs(rx) >= std::fabs(rz)) rightDx = (rx >= 0.0f) ? 1 : -1;
        else                                rightDz = (rz >= 0.0f) ? 1 : -1;

        dx = forwardStep * fwdDx + rightStep * rightDx;
        dz = forwardStep * fwdDz + rightStep * rightDz;
    }
    else {
        // Feste TMNF-Weltachsen: Vorwärts = +Z, Rechts = -X, Links = +X
        dx = -rightStep;
        dz = forwardStep;
    }
    MoveSelectedBlocks(dx, upStep, dz);
}

static void TeleportSelectedBlocksToTarget(const Int3& targetPos) {
    int32_t deltaX = 0, deltaY = 0, deltaZ = 0, savedStep = 1;
    {
        ScopedStateLock lock;
        if (!RefreshEditorContext()) return;
        SanitizeSelection();
        if (g_selectedBlocks.empty()) { PlayBeepSound(240, 60); return; }

        Int3 anchor = ReadBlockCoord(g_selectedBlocks[0].blockAddr);
        if (g_centerBlueprintAtCursor && g_selectedBlocks.size() > 1) {
            int32_t minX = anchor.x, maxX = anchor.x, minY = anchor.y, minZ = anchor.z, maxZ = anchor.z;
            for (const auto& entry : g_selectedBlocks) {
                Int3 p = ReadBlockCoord(entry.blockAddr);
                if (p.x < minX) minX = p.x;
                if (p.x > maxX) maxX = p.x;
                if (p.y < minY) minY = p.y;
                if (p.z < minZ) minZ = p.z;
                if (p.z > maxZ) maxZ = p.z;
            }
            anchor = { (minX + maxX) / 2, minY, (minZ + maxZ) / 2 };
        }

        deltaX = targetPos.x - anchor.x;
        deltaY = targetPos.y - anchor.y;
        deltaZ = targetPos.z - anchor.z;
        savedStep = g_cfg.stepMultiplier;
        g_cfg.stepMultiplier = 1;
    }
    MoveSelectedBlocks(deltaX, deltaY, deltaZ);
    {
        ScopedStateLock lock;
        g_cfg.stepMultiplier = savedStep;
    }
}

static void RotateSelectedBlocks(bool clockwise) {
    ScopedStateLock lock;
    if (!RefreshEditorContext()) return;
    RemoveAllSelectionPulses();
    SanitizeSelection();
    if (g_selectedBlocks.empty()) { PlayBeepSound(240, 60); return; }

    bool isVanilla = (g_cfg.ruleMode == static_cast<int>(BuildRuleMode::Vanilla));
    int32_t minX = isVanilla ? VANILLA_MIN_X : 0, maxX = isVanilla ? VANILLA_MAX_X : UNLIMITER_MAX;
    int32_t minZ = isVanilla ? VANILLA_MIN_Z : 0, maxZ = isVanilla ? VANILLA_MAX_Z : UNLIMITER_MAX;
    Int3 pivot = ReadBlockCoord(g_selectedBlocks[0].blockAddr);

    uint32_t dirStep = clockwise ? 1u : 3u;
    for (auto& entry : g_selectedBlocks) {
        uintptr_t block = entry.blockAddr;
        uint32_t newDir = (ReadBlockDir(block) + dirStep) & 3u;
        Int3 oldPos = ReadBlockCoord(block), newPos = oldPos;

        if (g_cfg.rotateAsGroup && g_selectedBlocks.size() > 1) {
            int32_t relX = oldPos.x - pivot.x, relZ = oldPos.z - pivot.z;
            // Synchron zur Block-Rotation (Dir 0 -> 1 -> 2 -> 3 entspricht +Z -> -X -> -Z -> +X)
            int32_t nextX = clockwise ? (pivot.x - relZ) : (pivot.x + relZ);
            int32_t nextZ = clockwise ? (pivot.z + relX) : (pivot.z - relX);
            if (nextX >= minX && nextX <= maxX && nextZ >= minZ && nextZ <= maxZ) {
                newPos.x = nextX;
                newPos.z = nextZ;
                WriteBlockCoord(block, newPos);
                MoveBlockVisualsTargeted(block, (newPos.x - oldPos.x) * 32.0f, 0.0f, (newPos.z - oldPos.z) * 32.0f);
            }
        }
        WriteBlockDir(block, newDir);
        RotateBlockVisualsTargeted(block, newDir);
    }

    ApplyAllSelectionPulses();
    RequestLiveEditorRebuild();
    PlayBeepSound(clockwise ? 1050 : 850, 45);
}

static void DumpMemoryRangeToStream(std::ofstream& out, const char* title, uintptr_t baseAddr, int maxBytes) {
    if (!IsSafeWritableHeapAddr(baseAddr)) return;
    char buf[256];
    int chunkEnd = DetectHeapChunkEndOffset(baseAddr, maxBytes);
    sprintf_s(buf, "\n--- %s [0x%08X] (ChunkLen=0x%X) ---\n", title, (uint32_t)baseAddr, chunkEnd);
    out << buf;
    for (int off = 0; off < chunkEnd; off += 4) {
        uint32_t v = 0; SafeReadU32(baseAddr + off, &v);
        float f = 0; SafeReadFloat(baseAddr + off, &f);
        const char* cn = GetHeapObjectClassName(v);
        sprintf_s(buf, "  +0x%03X: 0x%08X (int=%d, flt=%.2f) %s\n", off, v, (int)v, f, cn ? cn : "");
        out << buf;
    }
}

static uint32_t  g_endSceneCallCount = 0;
static uintptr_t g_foundD3DDeviceAddr = 0;
static uintptr_t g_foundViewportAddr = 0;
static int       g_hookedVTableCount = 0;

static void DumpDeepBlockVisualHierarchy(std::ofstream& out, uintptr_t blockAddr, const char* label) {
    if (!IsAnyHeapObject(blockAddr)) return;
    char buf[256];
    Int3 bc = ReadBlockCoord(blockAddr);
    uint32_t bd = ReadBlockDir(blockAddr);
    uint32_t fl60 = 0, skinPtr = 0, mob0 = 0;
    SafeReadU32(blockAddr + BLOCK_OFF_FLAGS60, &fl60);
    SafeReadU32(blockAddr + BLOCK_OFF_SKIN, &skinPtr);
    SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_1, &mob0);

    sprintf_s(buf, "\n================ %s [0x%08X] Coord=(%d,%d,%d) Dir=%u Flags60=0x%08X Skin=0x%08X ================\n",
        label, (uint32_t)blockAddr, bc.x, bc.y, bc.z, bd, fl60, skinPtr);
    out << buf;

    DumpMemoryRangeToStream(out, "CGameCtnBlock", blockAddr, 0x68);

    if (IsAnyHeapObject(skinPtr)) {
        DumpMemoryRangeToStream(out, "CGameCtnBlockSkin (+0x20)", skinPtr, 0x40);
    }

    if (IsAnyHeapObject(mob0)) {
        DumpMemoryRangeToStream(out, "CSceneMobil (+0x30)", mob0, 0x60);

        uint32_t item20 = 0, hmsItem28 = 0;
        if (SafeReadU32(mob0 + 0x20, &item20) && IsAnyHeapObject(item20)) {
            DumpMemoryRangeToStream(out, "Mobil+0x20 Object", item20, 0x80);
        }
        if (SafeReadU32(mob0 + 0x28, &hmsItem28) && IsAnyHeapObject(hmsItem28)) {
            DumpMemoryRangeToStream(out, "CHmsItem (Mobil+0x28)", hmsItem28, 0x80);

            for (int off = 0x04; off < 0x60; off += 4) {
                uint32_t subPtr = 0;
                if (SafeReadU32(hmsItem28 + off, &subPtr) && IsAnyHeapObject(subPtr)) {
                    char subTitle[64];
                    sprintf_s(subTitle, "CHmsItem+0x%02X (%s)", off,
                        GetHeapObjectClassName(subPtr) ? GetHeapObjectClassName(subPtr) : "Obj");
                    DumpMemoryRangeToStream(out, subTitle, subPtr, 0x60);
                }
            }

            uint32_t cCnt = 0, cArr = 0;
            if (SafeReadU32(hmsItem28 + 0x34, &cCnt) && SafeReadU32(hmsItem28 + 0x38, &cArr) &&
                cCnt > 0 && cCnt <= 8 && IsSafeWritableHeapAddr(cArr)) {
                for (uint32_t i = 0; i < cCnt; ++i) {
                    uint32_t corp = 0;
                    if (SafeReadU32(cArr + i * 4, &corp) && IsAnyHeapObject(corp)) {
                        char cTitle[64];
                        sprintf_s(cTitle, "CHmsCorpus[%u]", i);
                        DumpMemoryRangeToStream(out, cTitle, corp, 0x80);
                    }
                }
            }
        }
    }
}

static void WriteDiagnosticFile() {
    RemoveAllSelectionPulses();
    RefreshEditorContext();
    SanitizeSelection();
    std::string path = g_baseDir + "BlockMover_Diagnose.txt";
    std::ofstream out(path);
    if (!out.is_open()) return;

    char buf[256];
    Int3 curPos = ReadCursorCoord();
    sprintf_s(buf, "ExeBounds:            0x%08X - 0x%08X\n", (uint32_t)g_exeStart, (uint32_t)g_exeEnd); out << buf;
    sprintf_s(buf, "pApp:                 0x%08X (%s)\n", (uint32_t)g_ctx.pApp, GetHeapObjectClassName(g_ctx.pApp) ? GetHeapObjectClassName(g_ctx.pApp) : "null"); out << buf;
    sprintf_s(buf, "pEditor414:           0x%08X (%s)\n", (uint32_t)g_ctx.pEditorCat, GetHeapObjectClassName(g_ctx.pEditorCat) ? GetHeapObjectClassName(g_ctx.pEditorCat) : "null"); out << buf;
    sprintf_s(buf, "pCursor:              0x%08X (%s) | ReadCoord=(%d,%d,%d)\n",
        (uint32_t)g_ctx.pCursor, GetHeapObjectClassName(g_ctx.pCursor) ? GetHeapObjectClassName(g_ctx.pCursor) : "null",
        curPos.x, curPos.y, curPos.z); out << buf;
    sprintf_s(buf, "pChallenge:           0x%08X | TotalBlocks=%u (Array=0x%08X)\n",
        (uint32_t)g_ctx.pChallenge, g_ctx.blockCount, (uint32_t)g_ctx.pBlocksArray); out << buf;
    sprintf_s(buf, "SelectedBlocks:       %u | LastMoved3DFloats: %d | LastSpawned: %u\n",
        (uint32_t)g_selectedBlocks.size(), g_lastMovedFloats, g_lastSpawnedCount); out << buf;

    // 1. Dumpe den ausgewählten Block IMMER zuerst!
    if (!g_selectedBlocks.empty() && IsAnyHeapObject(g_selectedBlocks[0].blockAddr)) {
        DumpDeepBlockVisualHierarchy(out, g_selectedBlocks[0].blockAddr, "SELECTED-BLOCK [0]");
    }

    // 2. Dumpe die neuesten Skin-Blöcke RÜCKWÄRTS (vom Ende des Arrays abwärts, damit der Klon ganz oben steht!)
    if (g_ctx.pBlocksArray && g_ctx.blockCount > 0) {
        int skinBlockIdx = 0;
        for (int32_t i = static_cast<int32_t>(g_ctx.blockCount) - 1; i >= 0 && skinBlockIdx < 4; --i) {
            uint32_t b = 0, sk = 0;
            if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && IsAnyHeapObject(b) &&
                SafeReadU32(b + BLOCK_OFF_SKIN, &sk) && IsAnyHeapObject(sk)) {
                char lbl[64];
                sprintf_s(lbl, "LATEST-SKIN-BLOCK #%d (ArrayIdx=%d)", skinBlockIdx, i);
                DumpDeepBlockVisualHierarchy(out, b, lbl);
                ++skinBlockIdx;
            }
        }
    }

    // 3. Dumpe die CPlugTree-Struktur des neuesten Skin-Blocks und die SetSkin-Engine-Funktionen:
    if (g_exeStart != 0) {
        const uint32_t rvas[] = { 0x2FD400, 0x2FD4E0, 0x4A2FD0, 0x4DE290 };
        for (uint32_t rva : rvas) {
            uintptr_t fnStart = g_exeStart + rva;
            sprintf_s(buf, "\n--- Exe Code @ RVA 0x%06X (0x%08X) ---\n  ", rva, (uint32_t)fnStart);
            out << buf;
            for (int k = 0; k < 128; ++k) {
                uint8_t b = 0;
                SafeReadByte(fnStart + k, &b);
                sprintf_s(buf, "%02X ", b);
                out << buf;
                if ((k & 15) == 15) out << "\n  ";
            }
        }
    }

    out.close();
    PlayBeepSound(1250, 100);
}

// ============================================================================
// 6. Dear ImGui 1.93.0 Direct3D 9 In-Game Overlay & 3D Blaue Boxen
// ============================================================================

static int* g_bindingTarget = nullptr;

static std::string GetKeyName(int vk) {
    if (vk <= 0) return TR("Keine", "None");
    switch (vk) {
    case VK_RETURN:  return "Enter";
    case VK_BACK:    return "Backspace";
    case VK_SPACE:   return TR("Leertaste", "Space");
    case VK_PRIOR:   return TR("Bild Hoch", "Page Up");
    case VK_NEXT:    return TR("Bild Runter", "Page Down");
    case VK_UP:      return TR("Pfeil Hoch", "Up Arrow");
    case VK_DOWN:    return TR("Pfeil Runter", "Down Arrow");
    case VK_LEFT:    return TR("Pfeil Links", "Left Arrow");
    case VK_RIGHT:   return TR("Pfeil Rechts", "Right Arrow");
    case VK_NUMPAD0: return "Numpad 0";
    case VK_NUMPAD1: return "Numpad 1";
    case VK_NUMPAD2: return "Numpad 2";
    case VK_NUMPAD3: return "Numpad 3";
    case VK_NUMPAD4: return "Numpad 4";
    case VK_NUMPAD5: return "Numpad 5";
    case VK_NUMPAD6: return "Numpad 6";
    case VK_NUMPAD7: return "Numpad 7";
    case VK_NUMPAD8: return "Numpad 8";
    case VK_NUMPAD9: return "Numpad 9";
    }
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    char name[64] = { 0 };
    if (GetKeyNameTextA(sc << 16, name, sizeof(name)) > 0) return std::string(name);
    char fb[16]; sprintf_s(fb, "VK_%02X", vk); return std::string(fb);
}

static bool IsTrackManiaProcessFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return (fgPid == GetCurrentProcessId());
}

typedef HRESULT(STDMETHODCALLTYPE* FnD3D9EndScene)(IDirect3DDevice9* pDevice);
typedef HRESULT(STDMETHODCALLTYPE* FnD3D9Reset)(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);
typedef HRESULT(STDMETHODCALLTYPE* FnD3D9SetTransform)(IDirect3DDevice9* pDevice, D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix);

struct HookedD3DVTableEntry {
    uintptr_t* vtable;
    FnD3D9Reset        origReset;
    FnD3D9EndScene     origEndScene;
    FnD3D9SetTransform origSetTransform;
};

static HookedD3DVTableEntry g_hookedVTables[16] = {};
static bool                 g_imguiInitialized = false;
static IDirect3DDevice9* g_imguiDevice = nullptr;
static HWND                 g_imguiHwnd = nullptr;
static char                 g_bpNameBuf[64] = "MeineDeko";
static bool                 g_imguiWantKeyboard = false;

static bool IsTypingInBlueprintBox() {
    return (g_menuVisible && g_imguiInitialized && g_imguiWantKeyboard);
}

static bool IsModActiveMoveKey(WPARAM vk) {
    if (!g_cfg.modEnabled || g_state == ModState::Inactive) return false;
    int k = static_cast<int>(vk);
    if (k <= 0) return false;
    if (k == g_cfg.keySelectLastBuilt || k == g_cfg.keySelectAllBlocks ||
        k == g_cfg.keySelectSameHeight || k == g_cfg.keySelectSameType ||
        k == g_cfg.keyBoxCorner1 || k == g_cfg.keyBoxSelect2 ||
        k == g_cfg.keyStepSizeCycle || k == g_cfg.keyPasteBlueprint) {
        return true;
    }
    if (g_selectedBlocks.empty()) return false;
    return (k == g_cfg.keyMoveNorth || k == g_cfg.keyMoveSouth ||
        k == g_cfg.keyMoveWest || k == g_cfg.keyMoveEast ||
        k == g_cfg.keyMoveUp || k == g_cfg.keyMoveDown ||
        k == g_cfg.keyRotateCW || k == g_cfg.keyRotateCCW ||
        k == g_cfg.keyConfirmSelection || k == g_cfg.keyTeleportToCursor ||
        k == g_cfg.keyCloneAtCursor || k == g_cfg.keyCopySelection);
}

static bool HandleD3D9OverlayWndProcMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
        !g_menuVisible && IsModActiveMoveKey(wParam)) {
        return true;
    }

    if (!g_imguiInitialized) return false;

    LRESULT imguiRes = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    if (!g_menuVisible) return false;
    if (imguiRes) return true;

    ImGuiIO& io = ImGui::GetIO();
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (io.WantCaptureMouse) return true;
        break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
        if (io.WantCaptureKeyboard || io.WantTextInput) return true;
        break;
    default:
        break;
    }
    return false;
}

static void ApplyTrackManiaImGuiTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.5f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.10f, 0.14f, 0.95f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.08f, 0.11f, 0.90f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.11f, 0.16f, 0.96f);
    colors[ImGuiCol_Border] = ImVec4(0.00f, 0.78f, 1.00f, 0.65f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.16f, 0.23f, 0.90f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.25f, 0.36f, 0.95f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.32f, 0.46f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.14f, 0.22f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.20f, 0.32f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.00f, 0.90f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.00f, 0.78f, 1.00f, 0.85f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.00f, 0.92f, 1.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.13f, 0.21f, 0.31f, 0.95f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.34f, 0.49f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.00f, 0.60f, 0.82f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.13f, 0.24f, 0.36f, 0.90f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.35f, 0.52f, 0.95f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.00f, 0.62f, 0.85f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.32f, 0.45f, 0.75f);
    colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.18f, 0.27f, 0.95f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.18f, 0.36f, 0.54f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.00f, 0.52f, 0.74f, 1.00f);
}

static bool IsMainStadiumCameraView(IDirect3DDevice9* pDevice, const D3DMATRIX* v) {
    if (!v) return false;

    // 1. Muss eine gültige affine 3D-Matrix sein (_44 == 1, letzte Spalte _14.._34 == 0)
    if (std::fabs(v->_44 - 1.0f) > 0.01f ||
        std::fabs(v->_14) > 0.01f || std::fabs(v->_24) > 0.01f || std::fabs(v->_34) > 0.01f) {
        return false;
    }

    // 2. Obere 3x3-Matrix muss orthonormal sein (Einheitslänge der Zeilen, keine Skalierung)
    float r0 = v->_11 * v->_11 + v->_12 * v->_12 + v->_13 * v->_13;
    float r1 = v->_21 * v->_21 + v->_22 * v->_22 + v->_23 * v->_23;
    float r2 = v->_31 * v->_31 + v->_32 * v->_32 + v->_33 * v->_33;
    if (std::fabs(r0 - 1.0f) > 0.1f || std::fabs(r1 - 1.0f) > 0.1f || std::fabs(r2 - 1.0f) > 0.1f) {
        return false;
    }

    // 3. 2D-UI-Einheitsmatrix (_11=1, _22=1, _33=1 ohne 3D-Neigung) strikt ausschließen!
    bool isIdentityRot = (std::fabs(v->_11 - 1.0f) < 1e-3f &&
        std::fabs(v->_22 - 1.0f) < 1e-3f &&
        std::fabs(v->_33 - 1.0f) < 1e-3f);
    if (isIdentityRot) return false;

    // 4. Weltposition der Kamera aus View-Matrix berechnen: CamPos = -T * R^T
    //    Im Stadion liegt die Kamera im Bereich X/Z (distXZ > 60m), bei 3D-Katalog-Icons nahe (0,0,0).
    float camX = -(v->_41 * v->_11 + v->_42 * v->_12 + v->_43 * v->_13);
    float camY = -(v->_41 * v->_21 + v->_42 * v->_22 + v->_43 * v->_23);
    float camZ = -(v->_41 * v->_31 + v->_42 * v->_32 + v->_43 * v->_33);
    if (camY < 1.0f || (camX * camX + camZ * camZ) < (60.0f * 60.0f)) {
        return false;
    }

    // 5. Viewport muss das Hauptfenster sein (keine kleinen Icon-Viewports)
    if (pDevice) {
        D3DVIEWPORT9 vp{};
        if (SUCCEEDED(pDevice->GetViewport(&vp)) && (vp.Width < 400 || vp.Height < 300)) {
            return false;
        }
    }
    return true;
}

static bool ProjectWorldToScreen(float wx, float wy, float wz, const D3DMATRIX& view, const D3DMATRIX& proj, const ImVec2& dispSize, ImVec2& outScreen) {
    float vx = wx * view._11 + wy * view._21 + wz * view._31 + view._41;
    float vy = wx * view._12 + wy * view._22 + wz * view._32 + view._42;
    float vz = wx * view._13 + wy * view._23 + wz * view._33 + view._43;
    float vw = wx * view._14 + wy * view._24 + wz * view._34 + view._44;

    float cx = vx * proj._11 + vy * proj._21 + vz * proj._31 + vw * proj._41;
    float cy = vx * proj._12 + vy * proj._22 + vz * proj._32 + vw * proj._42;
    float cw = vx * proj._14 + vy * proj._24 + vz * proj._34 + vw * proj._44;

    if (cw < 0.1f) return false;
    float invW = 1.0f / cw;
    float ndcX = cx * invW;
    float ndcY = cy * invW;
    if (ndcX < -4.0f || ndcX > 4.0f || ndcY < -4.0f || ndcY > 4.0f) return false;

    outScreen.x = (ndcX * 0.5f + 0.5f) * dispSize.x;
    outScreen.y = (0.5f - ndcY * 0.5f) * dispSize.y;
    return true;
}

static void DrawProjected3DBoxOnImGui(ImDrawList* dl, const ImVec2& dispSize, float x0, float y0, float z0, float x1, float y1, float z1, ImU32 lineCol, ImU32 fillCol) {
    const float corners3D[8][3] = {
        { x0, y0, z0 },{ x1, y0, z0 },{ x1, y0, z1 },{ x0, y0, z1 },
        { x0, y1, z0 },{ x1, y1, z0 },{ x1, y1, z1 },{ x0, y1, z1 }
    };
    ImVec2 pts[8];
    bool ok[8];
    int validCnt = 0;
    for (int i = 0; i < 8; ++i) {
        ok[i] = ProjectWorldToScreen(corners3D[i][0], corners3D[i][1], corners3D[i][2], g_d3dView, g_d3dProj, dispSize, pts[i]);
        if (ok[i]) ++validCnt;
    }
    if (validCnt < 2) return;

    if ((fillCol >> 24) != 0 && validCnt == 8) {
        const int faces[6][4] = {
            { 0, 1, 2, 3 },{ 4, 5, 6, 7 },
            { 0, 1, 5, 4 },{ 1, 2, 6, 5 },
            { 2, 3, 7, 6 },{ 3, 0, 4, 7 }
        };
        for (int f = 0; f < 6; ++f) {
            dl->AddQuadFilled(pts[faces[f][0]], pts[faces[f][1]], pts[faces[f][2]], pts[faces[f][3]], fillCol);
        }
    }

    const int edges[12][2] = {
        { 0, 1 },{ 1, 2 },{ 2, 3 },{ 3, 0 },
        { 4, 5 },{ 5, 6 },{ 6, 7 },{ 7, 4 },
        { 0, 4 },{ 1, 5 },{ 2, 6 },{ 3, 7 }
    };
    for (int e = 0; e < 12; ++e) {
        int a = edges[e][0], b = edges[e][1];
        if (ok[a] && ok[b]) dl->AddLine(pts[a], pts[b], lineCol, 2.0f);
    }
}

static void RenderImGuiSelectionBoxes3D(IDirect3DDevice9* pDevice) {
    if (!g_cfg.modEnabled || !g_cfg.showBlueBoxes) return;
    if (g_selectedBlocks.empty() && !g_hasBoxCorner1) return;

    if (!g_d3dHas3DMatrices && pDevice) {
        D3DMATRIX v{}, p{};
        if (SUCCEEDED(pDevice->GetTransform(D3DTS_VIEW, &v)) &&
            SUCCEEDED(pDevice->GetTransform(D3DTS_PROJECTION, &p))) {
            if (IsMainStadiumCameraView(pDevice, &v) && std::fabs(p._44) < 0.001f && std::fabs(p._34) > 0.1f) {
                g_d3dView = v;
                g_d3dProj = p;
                g_d3dHas3DMatrices = true;
            }
        }
    }
    if (!g_d3dHas3DMatrices) return;

    ImDrawList* bgList = ImGui::GetBackgroundDrawList();
    ImVec2 dispSize = ImGui::GetIO().DisplaySize;
    if (!bgList || dispSize.x <= 0.0f || dispSize.y <= 0.0f) return;

    size_t maxBoxes = g_selectedBlocks.size();
    if (maxBoxes > 256) maxBoxes = 256;
    for (size_t i = 0; i < maxBoxes; ++i) {
        const auto& entry = g_selectedBlocks[i];
        if (!IsAnyHeapObject(entry.blockAddr)) continue;
        Int3 pos = ReadBlockCoord(entry.blockAddr);
        float x0 = pos.x * 32.0f - 0.10f;
        float y0 = pos.y * 8.0f + entry.appliedPulseY - 0.10f;
        float z0 = pos.z * 32.0f - 0.10f;
        float x1 = (pos.x + 1) * 32.0f + 0.10f;
        float y1 = (pos.y + 1) * 8.0f + entry.appliedPulseY + 0.10f;
        float z1 = (pos.z + 1) * 32.0f + 0.10f;
        DrawProjected3DBoxOnImGui(bgList, dispSize, x0, y0, z0, x1, y1, z1,
            IM_COL32(0, 229, 255, 245), IM_COL32(0, 180, 255, 36));
    }

    if (g_hasBoxCorner1) {
        float x0 = g_boxCorner1.x * 32.0f;
        float y0 = g_boxCorner1.y * 8.0f;
        float z0 = g_boxCorner1.z * 32.0f;
        DrawProjected3DBoxOnImGui(bgList, dispSize, x0, y0, z0, x0 + 32.0f, y0 + 8.0f, z0 + 32.0f,
            IM_COL32(0, 255, 110, 245), IM_COL32(0, 255, 110, 36));
    }
}

static void RenderDearImGuiFrame(IDirect3DDevice9* pDevice) {
    bool inEditor = (g_ctx.pEditorCat != 0 && g_ctx.pChallenge != 0);
    if (!inEditor && !g_menuVisible) return;

    HWND hGame = g_hookedGameWnd;
    if (!hGame && pDevice) {
        D3DDEVICE_CREATION_PARAMETERS dcp{};
        if (SUCCEEDED(pDevice->GetCreationParameters(&dcp)) && dcp.hFocusWindow && IsWindow(dcp.hFocusWindow)) {
            hGame = dcp.hFocusWindow;
        }
    }
    if (!hGame) hGame = FindTrackManiaMainWindow();
    if (!hGame) return;

    if (!g_hookedGameWnd && IsWindow(hGame)) {
        WNDPROC prev = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hGame, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedGameWndProc)));
        if (prev && prev != HookedGameWndProc) {
            g_origGameWndProc = prev;
            g_hookedGameWnd = hGame;
        }
    }

    if (!g_imguiInitialized || g_imguiDevice != pDevice || g_imguiHwnd != hGame) {
        if (g_imguiInitialized) {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_imguiInitialized = false;
        }
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ApplyTrackManiaImGuiTheme();
        ImGui_ImplWin32_Init(hGame);
        ImGui_ImplDX9_Init(pDevice);
        g_imguiDevice = pDevice;
        g_imguiHwnd = hGame;
        g_imguiInitialized = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_menuVisible;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        IDirect3DSurface9* pBB = nullptr;
        if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB)) && pBB) {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(pBB->GetDesc(&desc)) && desc.Width > 0 && desc.Height > 0) {
                io.DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
            }
            pBB->Release();
        }
    }
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;

    ImGui::NewFrame();
    g_imguiWantKeyboard = g_menuVisible && (io.WantCaptureKeyboard || io.WantTextInput);

    // Immer live mit dem echten Editor-Cursor synchronisieren, sobald dieser im Spiel bewegt wird
    if (inEditor) {
        static Int3 s_prevHwCursor = { -1, -1, -1 };
        Int3 hwCur = ReadCursorCoord();
        if (hwCur.x != s_prevHwCursor.x || hwCur.y != s_prevHwCursor.y || hwCur.z != s_prevHwCursor.z) {
            g_lastWorldCursorPos = hwCur;
            s_prevHwCursor = hwCur;
        }
    }

    RenderImGuiSelectionBoxes3D(pDevice);

    Int3 curPos = g_lastWorldCursorPos;
    const char* modeShort = !g_cfg.modEnabled ? TR("DEAKTIVIERT", "DISABLED") :
        (g_state == ModState::Inactive) ? TR("PAUSIERT", "PAUSED") :
        (!g_selectedBlocks.empty()) ? TR("BEWEGEN AKTIV", "MOVING ACTIVE") : TR("AUSWAHL BEREIT", "SELECT READY");

    if (g_menuVisible && inEditor) {
        ImGui::SetNextWindowPos(ImVec2(12.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##BlockMoverHUD", nullptr, hudFlags)) {
            ImGui::TextColored(ImVec4(0.00f, 0.92f, 1.00f, 1.0f), "TMNF BlockMover Studio");
            ImGui::SameLine();
            ImGui::Text("| %s | %s: %u | Cursor: (%d, %d, %d) | %s: %dx | [%s] %s",
                modeShort, TR("Auswahl", "Selected"), (uint32_t)g_selectedBlocks.size(),
                curPos.x, curPos.y, curPos.z,
                TR("Schritt", "Step"), g_cfg.stepMultiplier, GetKeyName(g_cfg.keyToggleMenu).c_str(), TR("Menü", "Menu"));
        }
        ImGui::End();
    }

    if (g_menuVisible) {
        ImGui::SetNextWindowPos(ImVec2(20.0f, 46.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(610.0f, 580.0f), ImGuiCond_FirstUseEver);

        bool open = g_menuVisible;
        if (ImGui::Begin("TMNF BlockMover Studio - Live 3D Editor & Blueprint Suite (F3)###MainStudio", &open)) {
            float topW = ImGui::GetContentRegionAvail().x;
            if (inEditor) {
                ImGui::TextColored(ImVec4(0.00f, 0.90f, 1.00f, 1.0f),
                    TR("Map (%u Blöcke) | Ziel-Cursor: (%d, %d, %d) | Clipboard: %u",
                        "Map (%u Blocks) | Target Cursor: (%d, %d, %d) | Clipboard: %u"),
                    g_ctx.blockCount, curPos.x, curPos.y, curPos.z, (uint32_t)g_clipboard.size());
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.25f, 1.0f),
                    TR("Bitte zuerst eine Map im Streckeneditor öffnen!", "Please open a map in the track editor first!"));
            }
            ImGui::SameLine(topW - 88.0f);
            if (ImGui::Button(g_cfg.language == 0 ? "DE -> EN" : "EN -> DE", ImVec2(88.0f, 22.0f))) {
                g_cfg.language = (g_cfg.language == 0) ? 1 : 0;
                SaveConfig();
            }

            if (inEditor) {
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::InputInt3(TR("Ziel-Cursor (X,Y,Z)", "Target Cursor (X,Y,Z)"), &g_lastWorldCursorPos.x)) {
                    g_lastWorldCursorPos.x = ClampI32(g_lastWorldCursorPos.x, 0, UNLIMITER_MAX);
                    g_lastWorldCursorPos.y = ClampI32(g_lastWorldCursorPos.y, 1, UNLIMITER_MAX);
                    g_lastWorldCursorPos.z = ClampI32(g_lastWorldCursorPos.z, 0, UNLIMITER_MAX);
                    WriteCursorCoord(g_lastWorldCursorPos);
                }
                ImGui::SameLine();
                if (ImGui::Button(TR("Auf Auswahl[0]", "To Selection[0]")) && !g_selectedBlocks.empty()) {
                    g_lastWorldCursorPos = ReadBlockCoord(g_selectedBlocks[0].blockAddr);
                    WriteCursorCoord(g_lastWorldCursorPos);
                }
            }

            ImGui::Separator();

            if (ImGui::BeginTabBar("BlockMoverMainTabs")) {
                if (ImGui::BeginTabItem(TR("1. Steuerung & Auswahl", "1. Controls & Selection"))) {
                    float availW = ImGui::GetContentRegionAvail().x;
                    float bw3 = (availW - 14.0f) / 3.0f;

                    ImGui::TextColored(ImVec4(0.00f, 0.88f, 1.00f, 1.0f), TR("Smart-Auswahl Werkzeuge", "Smart Selection Tools"));
                    if (ImGui::Button(TR("Cursor-Block (+/-)", "Cursor Block (+/-)"), ImVec2(bw3, 26.0f))) ToggleBlockAtCursor();
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Letzter gebauter Block", "Last Placed Block"), ImVec2(bw3, 26.0f))) SelectLastPlacedBlock();
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Alle Streckenblöcke", "All Track Blocks"), ImVec2(bw3, 26.0f))) SelectAllMapBlocks();

                    if (ImGui::Button(TR("Gleiche Y-Höhe wählen", "Same Y-Height"), ImVec2(bw3, 26.0f))) SelectBlocksOnSameHeightY();
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Gleicher Blocktyp", "Same Block Type"), ImVec2(bw3, 26.0f))) SelectBlocksOfSameType();
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Auswahl absetzen", "Deselect All"), ImVec2(bw3, 26.0f))) {
                        ClearSelectionWithRestore();
                        g_state = ModState::Selecting;
                        PlayBeepSound(400, 60);
                    }

                    char c1Label[64];
                    if (g_hasBoxCorner1) sprintf_s(c1Label, TR("Ecke 1: (%d,%d,%d)", "Corner 1: (%d,%d,%d)"), g_boxCorner1.x, g_boxCorner1.y, g_boxCorner1.z);
                    else sprintf_s(c1Label, "%s", TR("1. Box-Ecke 1 am Cursor", "1. Set Box Corner 1"));
                    if (ImGui::Button(c1Label, ImVec2(bw3 * 1.5f, 25.0f))) SetBoxCorner1AtCursor();
                    ImGui::SameLine();
                    if (ImGui::Button(TR("2. Bereich bis Cursor wählen", "2. Select Box to Cursor"), ImVec2(availW - bw3 * 1.5f - 7.0f, 25.0f))) {
                        SelectBlocksInBoxToCursor();
                    }

                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.00f, 0.88f, 1.00f, 1.0f), TR("Schrittweite:", "Step Size:"));
                    ImGui::SameLine();
                    const int stepPresets[5] = { 1, 2, 4, 8, 16 };
                    for (int sp : stepPresets) {
                        bool active = (g_cfg.stepMultiplier == sp);
                        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.62f, 0.86f, 1.0f));
                        char spLbl[16]; sprintf_s(spLbl, "%dx", sp);
                        if (ImGui::Button(spLbl, ImVec2(38.0f, 22.0f))) g_cfg.stepMultiplier = sp;
                        if (active) ImGui::PopStyleColor();
                        ImGui::SameLine();
                    }
                    ImGui::NewLine();

                    if (ImGui::BeginTable("DPadTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled(g_cfg.cameraRelativeMove
                            ? TR("D-Pad (Relativ zur Kamera)", "D-Pad (Camera-Relative)")
                            : TR("D-Pad (Feste Weltachsen)", "D-Pad (Fixed World Axes)"));
                        ImGui::Dummy(ImVec2(76.0f, 0.0f)); ImGui::SameLine();
                        if (ImGui::Button(TR("Vor##dp", "Forward##dp"), ImVec2(84.0f, 28.0f))) MoveSelectedBlocksCameraRelative(1, 0, 0);

                        if (ImGui::Button(TR("Links##dp", "Left##dp"), ImVec2(80.0f, 28.0f))) MoveSelectedBlocksCameraRelative(0, -1, 0);
                        ImGui::SameLine();
                        if (ImGui::Button(TR("Zum Cursor!", "To Cursor!"), ImVec2(84.0f, 28.0f))) {
                            Int3 liveTarget = ReadCursorCoord();
                            if (IsAnyHeapObject(g_ctx.pCursor)) g_lastWorldCursorPos = liveTarget;
                            TeleportSelectedBlocksToTarget(g_lastWorldCursorPos);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(TR("Rechts##dp", "Right##dp"), ImVec2(80.0f, 28.0f))) MoveSelectedBlocksCameraRelative(0, 1, 0);

                        ImGui::Dummy(ImVec2(76.0f, 0.0f)); ImGui::SameLine();
                        if (ImGui::Button(TR("Zurück##dp", "Back##dp"), ImVec2(84.0f, 28.0f))) MoveSelectedBlocksCameraRelative(-1, 0, 0);

                        ImGui::TableNextColumn();
                        ImGui::TextDisabled(TR("Höhe (Y) & 90° Drehung", "Height (Y) & 90° Rotation"));
                        float colW = ImGui::GetContentRegionAvail().x;
                        float halfCol = (colW - 7.0f) * 0.5f;
                        if (ImGui::Button(TR("Hoch (+Y)", "Up (+Y)"), ImVec2(halfCol, 28.0f))) MoveSelectedBlocksCameraRelative(0, 0, 1);
                        ImGui::SameLine();
                        if (ImGui::Button(TR("Runter (-Y)", "Down (-Y)"), ImVec2(halfCol, 28.0f))) MoveSelectedBlocksCameraRelative(0, 0, -1);

                        if (ImGui::Button(TR("90° Rechts", "90° CW"), ImVec2(halfCol, 28.0f))) RotateSelectedBlocks(true);
                        ImGui::SameLine();
                        if (ImGui::Button(TR("90° Links", "90° CCW"), ImVec2(halfCol, 28.0f))) RotateSelectedBlocks(false);

                        if (ImGui::Button(TR("Auswahl am Cursor klonen", "Clone Selection at Cursor"), ImVec2(colW, 28.0f))) {
                            CopySelectedBlocks();
                            PasteFormationToSelection();
                        }
                        ImGui::EndTable();
                    }

                    ImGui::Separator();
                    ImGui::Checkbox(TR("Bewegung relativ zur Kamera-Perspektive (Vor/Zurück/Links/Rechts)",
                        "Move relative to Camera Perspective (Forward/Back/Left/Right)"), &g_cfg.cameraRelativeMove);
                    if (ImGui::Checkbox(TR("Auswahl +2m schweben lassen", "Float Selection +2m"), &g_cfg.visualHighlight)) {
                        if (g_cfg.visualHighlight) ApplyAllSelectionPulses();
                        else RemoveAllSelectionPulses();
                        RequestLiveEditorRebuild();
                    }
                    ImGui::SameLine(295.0f);
                    ImGui::Checkbox(TR("Cursor folgt beim Bewegen", "Cursor Follows Move"), &g_cfg.cursorFollowsMove);
                    ImGui::Checkbox(TR("Als Gruppe drehen (90°)", "Rotate as Group (90°)"), &g_cfg.rotateAsGroup);
                    ImGui::SameLine(295.0f);
                    ImGui::Checkbox(TR("3D-Auswahlboxen (Overlay)", "3D Selection Boxes"), &g_cfg.showBlueBoxes);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(TR("2. Blaupausen (.tmgroup)", "2. Blueprints (.tmgroup)"))) {
                    float availW = ImGui::GetContentRegionAvail().x;
                    ImGui::TextColored(ImVec4(0.00f, 0.88f, 1.00f, 1.0f),
                        TR("Spawnt exakt am Ziel-Cursor (%d, %d, %d) inkl. Block-Variante & Schild-Skin",
                            "Spawns strictly at Target Cursor (%d, %d, %d) preserving Variant & Sign Skin"),
                        curPos.x, curPos.y, curPos.z);

                    ImGui::Checkbox(TR("Horizontal (X/Z) am Cursor zentrieren (Aus = Exakt ab Block[0]-Anker)",
                        "Center horizontally (X/Z) at Cursor (Off = Exact Block[0] Anchor)"), &g_centerBlueprintAtCursor);

                    ImGui::SetNextItemWidth(240.0f);
                    ImGui::InputText(TR("Name (.tmgroup)", "File (.tmgroup)"), g_bpNameBuf, sizeof(g_bpNameBuf));
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Ordner scannen", "Scan Folder"), ImVec2(120.0f, 0.0f))) RefreshBlueprintFileList();

                    float bwBp = (availW - 14.0f) / 3.0f;
                    if (ImGui::Button(TR("1. Auswahl speichern (Strg+C)", "1. Save Selection (Ctrl+C)"), ImVec2(bwBp, 28.0f))) {
                        CopySelectedBlocks();
                        SaveBlueprintToFile(g_bpNameBuf);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(TR("2. Datei laden", "2. Load File"), ImVec2(bwBp, 28.0f))) LoadBlueprintFromFile(g_bpNameBuf);
                    ImGui::SameLine();
                    if (ImGui::Button(TR("3. Am Cursor spawnen (Strg+V)", "3. Spawn at Cursor (Ctrl+V)"), ImVec2(bwBp, 28.0f))) {
                        PasteFormationToSelection();
                    }

                    ImGui::Separator();
                    ImGui::BeginChild("BlueprintListChild", ImVec2(0, 210.0f), ImGuiChildFlags_Borders);
                    if (g_blueprintFiles.empty()) {
                        ImGui::TextDisabled(TR("Noch keine .tmgroup Dateien im Ordner Blueprints\\ gefunden.",
                            "No .tmgroup files found in Blueprints\\ folder yet."));
                    }
                    else {
                        for (size_t i = 0; i < g_blueprintFiles.size(); ++i) {
                            const std::string& fn = g_blueprintFiles[i];
                            bool isCurrent = (_stricmp(fn.c_str(), g_bpNameBuf) == 0);
                            if (ImGui::Selectable((fn + ".tmgroup##bp_" + std::to_string(i)).c_str(), isCurrent, ImGuiSelectableFlags_AllowDoubleClick)) {
                                strncpy_s(g_bpNameBuf, fn.c_str(), sizeof(g_bpNameBuf) - 1);
                                LoadBlueprintFromFile(g_bpNameBuf);
                                if (ImGui::IsMouseDoubleClicked(0)) PasteFormationToSelection();
                            }
                        }
                    }
                    ImGui::EndChild();
                    ImGui::TextDisabled(TR("Tipp: Klick lädt die Datei, Doppelklick spawnt sie direkt am Ziel-Cursor!",
                        "Tip: Single-click loads file, double-click spawns it right at the target cursor!"));
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(TR("3. Streckenblöcke-Liste", "3. Track Blocks List"))) {
                    ImGui::Checkbox(TR("Stadion-Außenring ausblenden", "Hide Stadium Perimeter"), &g_cfg.onlyPlayerBlocks);
                    ImGui::SameLine(360.0f);
                    if (ImGui::Button(TR("Auswahl leeren", "Clear Selection"), ImVec2(-1.0f, 0.0f))) {
                        ClearSelectionWithRestore();
                        g_state = ModState::Selecting;
                    }
                    ImGui::BeginChild("BlockListChildTab", ImVec2(0, 330.0f), ImGuiChildFlags_Borders);
                    if (inEditor && g_ctx.pBlocksArray && g_ctx.blockCount > 0) {
                        const char* dirNames[4] = { "N(0)", "E(1)", "S(2)", "W(3)" };
                        int shown = 0;
                        for (int32_t i = static_cast<int32_t>(g_ctx.blockCount) - 1; i >= 0 && shown < 300; --i) {
                            uint32_t b = 0;
                            if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) || !IsMovableTrackBlock(b)) continue;
                            Int3 pos = ReadBlockCoord(b);
                            uint32_t dir = ReadBlockDir(b);
                            bool sel = IsBlockSelected(b);
                            char rowBuf[192];
                            sprintf_s(rowBuf, "%s#%d  |  Pos: (%d, %d, %d)  |  Dir: %s  |  [0x%08X]##blk_%u",
                                sel ? "[*] " : "    ", i, pos.x, pos.y, pos.z, dirNames[dir & 3u], b, b);
                            if (ImGui::Selectable(rowBuf, sel)) {
                                if (sel) {
                                    for (size_t k = 0; k < g_selectedBlocks.size(); ++k) {
                                        if (g_selectedBlocks[k].blockAddr == b) {
                                            RemovePulseFromEntry(g_selectedBlocks[k]);
                                            g_selectedBlocks.erase(g_selectedBlocks.begin() + k);
                                            break;
                                        }
                                    }
                                }
                                else {
                                    g_selectedBlocks.push_back({ b, 0.0f });
                                    g_state = ModState::Moving;
                                    if (g_cfg.cursorFollowsMove) {
                                        WriteCursorCoord(pos);
                                        g_lastWorldCursorPos = pos;
                                    }
                                    ApplyAllSelectionPulses();
                                }
                                RequestLiveEditorRebuild();
                            }
                            ++shown;
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(TR("4. Einstellungen & Hotkeys", "4. Settings & Hotkeys"))) {
                    if (ImGui::Checkbox(TR("Mod Aktiviert (Master-Switch)", "Mod Enabled (Master Switch)"), &g_cfg.modEnabled)) {
                        if (!g_cfg.modEnabled) RemoveAllSelectionPulses();
                        RequestLiveEditorRebuild();
                    }
                    ImGui::SameLine(295.0f);
                    ImGui::Checkbox(TR("Sound-Feedback (Beep)", "Sound Feedback (Beep)"), &g_cfg.soundFeedback);

                    bool unlimiterMode = (g_cfg.ruleMode == static_cast<int>(BuildRuleMode::TMUnlimiter));
                    if (ImGui::Checkbox(TR("TMUnlimiter-Baugröße (0 - 255)", "TMUnlimiter Build Size (0 - 255)"), &unlimiterMode)) {
                        g_cfg.ruleMode = unlimiterMode ? 1 : 0;
                    }
                    ImGui::SameLine(295.0f);
                    ImGui::Checkbox(TR("Kamerarelative Bewegung", "Camera-Relative Move"), &g_cfg.cameraRelativeMove);

                    ImGui::Separator();
                    float availW = ImGui::GetContentRegionAvail().x;
                    float halfW = (availW - 8.0f) * 0.5f;
                    if (ImGui::Button(TR("Preset 1: Standard (WASD/V + E/Q/R/L/I)", "Preset 1: Default (WASD/V + E/Q/R/L/I)"), ImVec2(halfW, 24.0f))) {
                        ApplyKeybindPreset(0);
                        SaveConfig();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Preset 2: Numpad + F", "Preset 2: Numpad + F"), ImVec2(halfW, 24.0f))) {
                        ApplyKeybindPreset(1);
                        SaveConfig();
                    }

                    ImGui::TextColored(ImVec4(0.00f, 0.88f, 1.00f, 1.0f),
                        TR("Alle Hotkeys (Klick = Ändern | ENTF = Taste löschen | ESC = Abbruch):",
                            "All Hotkeys (Click = Bind | DEL = Clear keybind | ESC = Cancel):"));

                    struct ShortKbItem { const char* nameDe; const char* nameEn; int* ptr; };
                    ShortKbItem kbItems[22] = {
                        { "GUI Menü:",          "GUI Menu:",          &g_cfg.keyToggleMenu },
                        { "Mod Ein/Aus:",       "Mod On/Off:",        &g_cfg.keyToggleModMode },
                        { "Block auswählen:",   "Select Block:",      &g_cfg.keySelectBlock },
                        { "Auswahl absetzen:",  "Deselect All:",      &g_cfg.keyConfirmSelection },
                        { "Vorwärts:",          "Move Forward:",      &g_cfg.keyMoveNorth },
                        { "Rückwärts:",         "Move Back:",         &g_cfg.keyMoveSouth },
                        { "Links:",             "Move Left:",         &g_cfg.keyMoveWest },
                        { "Rechts:",            "Move Right:",        &g_cfg.keyMoveEast },
                        { "Hoch (+Y):",         "Move Up (+Y):",      &g_cfg.keyMoveUp },
                        { "Runter (-Y):",       "Move Down (-Y):",    &g_cfg.keyMoveDown },
                        { "90° Rechts drehen:", "Rotate 90° CW:",     &g_cfg.keyRotateCW },
                        { "90° Links drehen:",  "Rotate 90° CCW:",    &g_cfg.keyRotateCCW },
                        { "Zum Cursor!:",       "Teleport Cursor:",   &g_cfg.keyTeleportToCursor },
                        { "Am Cursor klonen:",  "Clone at Cursor:",   &g_cfg.keyCloneAtCursor },
                        { "Letzter Block:",     "Last Placed Blk:",   &g_cfg.keySelectLastBuilt },
                        { "Alle Blöcke:",       "All Map Blocks:",    &g_cfg.keySelectAllBlocks },
                        { "Gleiche Y-Höhe:",    "Same Y-Height:",     &g_cfg.keySelectSameHeight },
                        { "Gleicher Blocktyp:", "Same Block Type:",   &g_cfg.keySelectSameType },
                        { "Box-Ecke 1 setzen:", "Set Box Corner 1:",  &g_cfg.keyBoxCorner1 },
                        { "Box-Bereich wählen:","Select Box Area:",   &g_cfg.keyBoxSelect2 },
                        { "Schrittweite (1-16):","Cycle Step Size:",  &g_cfg.keyStepSizeCycle },
                        { "Spawnen (Paste):",   "Spawn Blueprint:",   &g_cfg.keyPasteBlueprint }
                    };

                    ImGui::BeginChild("KbScrollChild", ImVec2(0, 240.0f), ImGuiChildFlags_Borders);
                    if (ImGui::BeginTable("KbTable", 2, ImGuiTableFlags_SizingStretchSame)) {
                        for (int i = 0; i < 22; ++i) {
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", TR(kbItems[i].nameDe, kbItems[i].nameEn));
                            ImGui::SameLine(132.0f);
                            bool isBindingThis = (g_bindingTarget == kbItems[i].ptr);
                            std::string btnLbl = (isBindingThis ? TR("Taste drücken...", "Press key...") : ("[ " + GetKeyName(*kbItems[i].ptr) + " ]")) + "##kb" + std::to_string(i);
                            if (ImGui::Button(btnLbl.c_str(), ImVec2(-1.0f, 22.0f))) {
                                g_bindingTarget = kbItems[i].ptr;
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndChild();

                    if (ImGui::Button(TR("Einstellungen speichern (.ini)", "Save Settings (.ini)"), ImVec2(halfW, 28.0f))) {
                        SaveConfig();
                        PlayBeepSound(1200, 80);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Diagnose speichern (.txt)", "Save Diagnostics (.txt)"), ImVec2(halfW, 28.0f))) {
                        WriteDiagnosticFile();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        g_menuVisible = open;
    }
    else {
        g_imguiWantKeyboard = false;
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

static const HookedD3DVTableEntry* FindHookedVTableEntryForDevice(IDirect3DDevice9* pDevice) {
    if (pDevice && g_hookedVTableCount > 0) {
        uintptr_t* vt = *reinterpret_cast<uintptr_t**>(pDevice);
        for (int i = 0; i < g_hookedVTableCount; ++i) {
            if (g_hookedVTables[i].vtable == vt) return &g_hookedVTables[i];
        }
    }
    return (g_hookedVTableCount > 0) ? &g_hookedVTables[0] : nullptr;
}

static HRESULT STDMETHODCALLTYPE HookedSetTransform(IDirect3DDevice9* pDevice, D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    if (pMatrix) {
        if (State == D3DTS_VIEW) {
            if (IsMainStadiumCameraView(pDevice, pMatrix)) {
                g_candView = *pMatrix;
                g_hasCandView = true;
                g_d3dView = *pMatrix;
                if (std::fabs(g_d3dProj._34) > 0.1f) {
                    g_d3dHas3DMatrices = true;
                }
            }
        }
        else if (State == D3DTS_PROJECTION) {
            if (std::fabs(pMatrix->_44) < 0.001f && std::fabs(pMatrix->_34) > 0.1f) {
                D3DVIEWPORT9 vp{};
                if (!pDevice || (SUCCEEDED(pDevice->GetViewport(&vp)) && vp.Width >= 400 && vp.Height >= 300)) {
                    g_d3dProj = *pMatrix;
                    if (g_hasCandView) {
                        g_d3dView = g_candView;
                        g_d3dHas3DMatrices = true;
                    }
                }
            }
        }
    }
    const HookedD3DVTableEntry* entry = FindHookedVTableEntryForDevice(pDevice);
    return (entry && entry->origSetTransform) ? entry->origSetTransform(pDevice, State, pMatrix) : D3D_OK;
}

static HRESULT STDMETHODCALLTYPE HookedReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (g_imguiInitialized) ImGui_ImplDX9_InvalidateDeviceObjects();
    const HookedD3DVTableEntry* entry = FindHookedVTableEntryForDevice(pDevice);
    HRESULT hr = (entry && entry->origReset) ? entry->origReset(pDevice, pPresentationParameters) : D3D_OK;
    if (SUCCEEDED(hr) && g_imguiInitialized) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9* pDevice) {
    ++g_endSceneCallCount;
    const HookedD3DVTableEntry* entry = FindHookedVTableEntryForDevice(pDevice);
    FnD3D9EndScene origEndScene = entry ? entry->origEndScene : nullptr;

    bool inEditor = (g_ctx.pEditorCat != 0 && g_ctx.pChallenge != 0);
    if (!inEditor && !g_menuVisible) {
        return origEndScene ? origEndScene(pDevice) : D3D_OK;
    }

    if (inEditor && g_rebuildPending) ExecuteEditorSceneRebuildNow();

    if (pDevice) {
        ScopedStateLock lock;
        RenderDearImGuiFrame(pDevice);
    }

    return origEndScene ? origEndScene(pDevice) : D3D_OK;
}

static bool InstallD3D9VTableHooks(uintptr_t* vtable) {
    if (!vtable) return false;
    for (int i = 0; i < g_hookedVTableCount; ++i) {
        if (g_hookedVTables[i].vtable == vtable) return true;
    }
    if (g_hookedVTableCount >= 16) return false;

    DWORD oldProt = 0;
    if (!VirtualProtect(&vtable[16], 32 * sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt)) return false;

    HookedD3DVTableEntry& entry = g_hookedVTables[g_hookedVTableCount];
    entry.vtable = vtable;
    entry.origReset = (vtable[16] != reinterpret_cast<uintptr_t>(&HookedReset))
        ? reinterpret_cast<FnD3D9Reset>(vtable[16])
        : (g_hookedVTableCount > 0 ? g_hookedVTables[0].origReset : nullptr);
    entry.origEndScene = (vtable[42] != reinterpret_cast<uintptr_t>(&HookedEndScene))
        ? reinterpret_cast<FnD3D9EndScene>(vtable[42])
        : (g_hookedVTableCount > 0 ? g_hookedVTables[0].origEndScene : nullptr);
    entry.origSetTransform = (vtable[44] != reinterpret_cast<uintptr_t>(&HookedSetTransform))
        ? reinterpret_cast<FnD3D9SetTransform>(vtable[44])
        : (g_hookedVTableCount > 0 ? g_hookedVTables[0].origSetTransform : nullptr);

    vtable[16] = reinterpret_cast<uintptr_t>(&HookedReset);
    vtable[42] = reinterpret_cast<uintptr_t>(&HookedEndScene);
    vtable[44] = reinterpret_cast<uintptr_t>(&HookedSetTransform);

    DWORD dummy = 0;
    VirtualProtect(&vtable[16], 32 * sizeof(uintptr_t), oldProt, &dummy);
    ++g_hookedVTableCount;
    return true;
}

static bool CheckStdcallRetBytes(uintptr_t fnAddr, uint8_t expectedStackBytes) {
    for (uintptr_t i = 0; i < 1024; ++i) {
        uint8_t b0 = 0, b1 = 0, b2 = 0;
        if (!SafeReadByte(fnAddr + i, &b0)) break;
        if (b0 == 0xC2 && SafeReadByte(fnAddr + i + 1, &b1) && SafeReadByte(fnAddr + i + 2, &b2)) {
            if (b1 == expectedStackBytes && b2 == 0x00) return true;
        }
    }
    return false;
}

static bool IsPlausibleD3D9DeviceVTable(uintptr_t vtAddr, uintptr_t d3dCodeStart, uintptr_t d3dCodeEnd) {
    if (vtAddr < 0x10000 || (vtAddr & 3u) != 0) return false;
    uint32_t fn0 = 0, fn1 = 0, fn2 = 0, fn16 = 0, fn41 = 0, fn42 = 0, fn43 = 0, fn44 = 0, fn46 = 0, fn47 = 0, fn82 = 0;
    if (!SafeReadU32(vtAddr + 0 * 4, &fn0) || !SafeReadU32(vtAddr + 1 * 4, &fn1) ||
        !SafeReadU32(vtAddr + 2 * 4, &fn2) || !SafeReadU32(vtAddr + 16 * 4, &fn16) ||
        !SafeReadU32(vtAddr + 41 * 4, &fn41) || !SafeReadU32(vtAddr + 42 * 4, &fn42) ||
        !SafeReadU32(vtAddr + 43 * 4, &fn43) || !SafeReadU32(vtAddr + 44 * 4, &fn44) ||
        !SafeReadU32(vtAddr + 46 * 4, &fn46) || !SafeReadU32(vtAddr + 47 * 4, &fn47) ||
        !SafeReadU32(vtAddr + 82 * 4, &fn82)) {
        return false;
    }
    if (fn41 == fn42 || fn41 == fn43) return false;

    int inD3DCount = 0;
    for (int m = 0; m < 119; ++m) {
        uint32_t fn = 0;
        if (!SafeReadU32(vtAddr + m * 4, &fn) || fn < 0x10000) return false;
        if (fn >= d3dCodeStart && fn < d3dCodeEnd) ++inD3DCount;
    }
    if (inD3DCount < 110) return false;

    return CheckStdcallRetBytes(fn0, 12) && CheckStdcallRetBytes(fn1, 4) &&
        CheckStdcallRetBytes(fn2, 4) && CheckStdcallRetBytes(fn43, 28) &&
        CheckStdcallRetBytes(fn46, 12) && CheckStdcallRetBytes(fn47, 8) &&
        CheckStdcallRetBytes(fn82, 28);
}

static void EnsureD3D9OverlayHooked() {
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) return;

    uintptr_t d3dBase = reinterpret_cast<uintptr_t>(hD3D9);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(d3dBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(d3dBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    uintptr_t d3dCodeStart = d3dBase + 0x1000;
    uintptr_t d3dCodeEnd = d3dBase + nt->OptionalHeader.SizeOfImage;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            d3dCodeStart = d3dBase + sec[i].VirtualAddress;
            d3dCodeEnd = d3dCodeStart + sec[i].Misc.VirtualSize;
            break;
        }
    }

    static bool s_d3d9Scanned = false;
    if (!s_d3d9Scanned) {
        s_d3d9Scanned = true;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (memcmp(sec[i].Name, ".text", 5) == 0 || memcmp(sec[i].Name, ".rdata", 6) == 0 || memcmp(sec[i].Name, ".data", 5) == 0) {
                uintptr_t secStart = d3dBase + sec[i].VirtualAddress;
                uintptr_t secEnd = secStart + sec[i].Misc.VirtualSize;
                for (uintptr_t p = secStart; p + 119 * 4 <= secEnd; p += 4) {
                    if (IsPlausibleD3D9DeviceVTable(p, d3dCodeStart, d3dCodeEnd)) {
                        InstallD3D9VTableHooks(reinterpret_cast<uintptr_t*>(p));
                    }
                }
            }
        }
    }

    if (g_foundD3DDeviceAddr == 0 && IsAnyHeapObject(g_ctx.pApp)) {
        for (uint32_t off1 = 0x04; off1 <= 0x500; off1 += 4) {
            uint32_t ptr1 = 0;
            if (!SafeReadU32(g_ctx.pApp + off1, &ptr1) || !IsAnyHeapObject(ptr1)) continue;
            const char* cls1 = GetHeapObjectClassName(ptr1);
            if (!cls1 || !strstr(cls1, "Viewport")) continue;
            g_foundViewportAddr = ptr1;
            for (uint32_t off2 = 0x04; off2 <= 0x3500; off2 += 4) {
                uint32_t cand = 0, vt = 0;
                if (!SafeReadU32(ptr1 + off2, &cand) || cand < 0x10000 || (cand & 3u) != 0) continue;
                if (!SafeReadU32(cand, &vt)) continue;
                bool knownVt = false;
                for (int k = 0; k < g_hookedVTableCount; ++k) {
                    if (reinterpret_cast<uintptr_t>(g_hookedVTables[k].vtable) == vt) {
                        knownVt = true;
                        break;
                    }
                }
                if (knownVt || IsPlausibleD3D9DeviceVTable(vt, d3dCodeStart, d3dCodeEnd)) {
                    g_foundD3DDeviceAddr = cand;
                    InstallD3D9VTableHooks(reinterpret_cast<uintptr_t*>(vt));
                    break;
                }
            }
            if (g_foundD3DDeviceAddr != 0) break;
        }
    }
}

static void UnhookD3D9Overlay() {
    for (int i = 0; i < g_hookedVTableCount; ++i) {
        uintptr_t* vt = g_hookedVTables[i].vtable;
        if (!vt) continue;
        DWORD oldProt = 0;
        if (VirtualProtect(&vt[16], 32 * sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
            if (g_hookedVTables[i].origReset)        vt[16] = reinterpret_cast<uintptr_t>(g_hookedVTables[i].origReset);
            if (g_hookedVTables[i].origEndScene)     vt[42] = reinterpret_cast<uintptr_t>(g_hookedVTables[i].origEndScene);
            if (g_hookedVTables[i].origSetTransform) vt[44] = reinterpret_cast<uintptr_t>(g_hookedVTables[i].origSetTransform);
            DWORD dummy = 0;
            VirtualProtect(&vt[16], 32 * sizeof(uintptr_t), oldProt, &dummy);
        }
    }
    g_hookedVTableCount = 0;

    if (g_imguiInitialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
    }
}

static bool WasKeyPressed(int vKey) {
    if (vKey <= 0 || vKey >= 256) return false;
    static bool prevStates[256] = { false };
    bool isDown = (GetAsyncKeyState(vKey) & 0x8000) != 0;
    bool pressed = isDown && !prevStates[vKey];
    prevStates[vKey] = isDown;
    return pressed;
}

static void UpdateStatusBanner() {
    static ULONGLONG lastTick = 0;
    static uintptr_t lastChallengePtr = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastTick < 200) return;
    lastTick = now;

    ScopedStateLock lock;
    bool mapOk = RefreshEditorContext();
    EnsureGameWindowSubclassed();
    EnsureD3D9OverlayHooked();

    if (!mapOk || g_ctx.pChallenge != lastChallengePtr) {
        g_selectedBlocks.clear();
        g_dirtyBlocks.clear();
        g_clonedSignOverrides.clear();
        g_lastMovedBlockAddr = 0;
        lastChallengePtr = g_ctx.pChallenge;
    }
    UpdateCursorBoxHighlightColor(!g_selectedBlocks.empty());
}

// ============================================================================
// 7. Haupt-Thread & DLL-Einstiegspunkt
// ============================================================================

DWORD WINAPI ModThread(LPVOID lpParam) {
    InitPaths(reinterpret_cast<HMODULE>(lpParam));
    LoadConfig();

    while (g_running) {
        if (g_bindingTarget) {
            for (int vk = 8; vk < 255; ++vk) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;
                if (WasKeyPressed(vk)) {
                    if (vk == VK_DELETE) {
                        *g_bindingTarget = 0;
                        SaveConfig();
                    }
                    else if (vk != VK_ESCAPE) {
                        *g_bindingTarget = vk;
                        SaveConfig();
                    }
                    g_bindingTarget = nullptr;
                    break;
                }
            }
            Sleep(10);
            continue;
        }

        UpdateStatusBanner();

        if (!IsTrackManiaProcessFocused() || IsTypingInBlueprintBox()) {
            Sleep(20);
            continue;
        }

        if (WasKeyPressed(g_cfg.keyToggleMenu)) {
            if (!g_menuVisible) {
                ScopedStateLock lock;
                if (RefreshEditorContext()) {
                    g_lastWorldCursorPos = ReadCursorCoord();
                }
                RefreshBlueprintFileList();
            }
            g_menuVisible = !g_menuVisible;
            PlayBeepSound(g_menuVisible ? 950 : 650, 45);
        }

        if (!g_cfg.modEnabled) {
            Sleep(50);
            continue;
        }

        if (WasKeyPressed(g_cfg.keyToggleModMode)) {
            ScopedStateLock lock;
            if (g_state == ModState::Inactive) {
                g_state = ModState::Selecting;
                PlayBeepSound(750, 80);
            }
            else {
                g_state = ModState::Inactive;
                ClearSelectionWithRestore();
                PlayBeepSound(400, 80);
            }
        }

        bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if ((ctrlDown && WasKeyPressed('C')) || WasKeyPressed(g_cfg.keyCopySelection)) CopySelectedBlocks();
        if ((ctrlDown && WasKeyPressed('V')) || WasKeyPressed(g_cfg.keyPasteBlueprint)) PasteFormationToSelection();

        if (g_state != ModState::Inactive && !ctrlDown) {
            bool selectKeyDown = (g_cfg.keySelectBlock > 0) && ((GetAsyncKeyState(g_cfg.keySelectBlock) & 0x8000) != 0);
            HandleContinuousDragSelect(selectKeyDown);

            if (WasKeyPressed(g_cfg.keySelectLastBuilt))  SelectLastPlacedBlock();
            if (WasKeyPressed(g_cfg.keySelectAllBlocks))  SelectAllMapBlocks();
            if (WasKeyPressed(g_cfg.keySelectSameHeight)) SelectBlocksOnSameHeightY();
            if (WasKeyPressed(g_cfg.keySelectSameType))   SelectBlocksOfSameType();
            if (WasKeyPressed(g_cfg.keyBoxCorner1))       SetBoxCorner1AtCursor();
            if (WasKeyPressed(g_cfg.keyBoxSelect2))       SelectBlocksInBoxToCursor();
            if (WasKeyPressed(g_cfg.keyStepSizeCycle))    CycleStepMultiplier();

            if (!g_selectedBlocks.empty()) {
                if (WasKeyPressed(g_cfg.keyConfirmSelection) || WasKeyPressed(g_cfg.keyBackToSelect)) {
                    ScopedStateLock lock;
                    ClearSelectionWithRestore();
                    g_state = ModState::Selecting;
                    PlayBeepSound(600, 60);
                }
                else {
                    if (WasKeyPressed(g_cfg.keyMoveNorth)) MoveSelectedBlocksCameraRelative(1, 0, 0);
                    if (WasKeyPressed(g_cfg.keyMoveSouth)) MoveSelectedBlocksCameraRelative(-1, 0, 0);
                    if (WasKeyPressed(g_cfg.keyMoveWest))  MoveSelectedBlocksCameraRelative(0, -1, 0);
                    if (WasKeyPressed(g_cfg.keyMoveEast))  MoveSelectedBlocksCameraRelative(0, 1, 0);
                    if (WasKeyPressed(g_cfg.keyMoveUp))    MoveSelectedBlocksCameraRelative(0, 0, 1);
                    if (WasKeyPressed(g_cfg.keyMoveDown))  MoveSelectedBlocksCameraRelative(0, 0, -1);

                    if (WasKeyPressed(g_cfg.keyRotateCW))  RotateSelectedBlocks(true);
                    if (WasKeyPressed(g_cfg.keyRotateCCW)) RotateSelectedBlocks(false);

                    if (WasKeyPressed(g_cfg.keyTeleportToCursor)) {
                        Int3 target = ReadCursorCoord();
                        g_lastWorldCursorPos = target;
                        TeleportSelectedBlocksToTarget(target);
                    }
                    if (WasKeyPressed(g_cfg.keyCloneAtCursor)) {
                        CopySelectedBlocks();
                        PasteFormationToSelection();
                    }
                }
            }
        }

        Sleep(10);
    }

    UnhookD3D9Overlay();
    if (g_hookedGameWnd && g_origGameWndProc) {
        SetWindowLongPtrA(g_hookedGameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origGameWndProc));
    }
    {
        ScopedStateLock lock;
        ClearSelectionWithRestore();
    }
    return 0;
}

extern "C" __declspec(dllexport) void __cdecl BlockMoverInit() {}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_stateCs);
        g_csInitialized = true;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, ModThread, hModule, 0, nullptr);
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_running = false;
    }
    return TRUE;
}