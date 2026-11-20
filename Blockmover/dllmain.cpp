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
// 1. Safe Memory & Engine Helper Functions
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

constexpr int BLOCK_OFF_SKIN = 0x20; // CGameCtnBlockSkin*
constexpr int BLOCK_OFF_BLOCKINFO = 0x24; // CGameCtnBlockInfo*
constexpr int BLOCK_OFF_UNIT_CNT = 0x28; // uint32_t unitCount
constexpr int BLOCK_OFF_UNIT_ARR = 0x2C; // CGameCtnBlockUnit**
constexpr int BLOCK_OFF_MOBIL_1 = 0x30; // CSceneMobil*
constexpr int BLOCK_OFF_NOD_40 = 0x40;
constexpr int BLOCK_OFF_MOBIL_2 = 0x44;
constexpr int BLOCK_OFF_COORD_X = 0x48;
constexpr int BLOCK_OFF_COORD_Y = 0x4C;
constexpr int BLOCK_OFF_COORD_Z = 0x50;
constexpr int BLOCK_OFF_DIR = 0x54; // 0 = North, 1 = East, 2 = South, 3 = West
constexpr int BLOCK_OFF_FLAGS60 = 0x60; // Bits 0..5 = Variant, 0x1000 = IsGround, 0x2000 = Clip, 0x4000 = Connected, 0x8000 = HasSkin

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
// 2. Configuration, Localization (EN/DE) & Global State
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
    int  language = 1; // 1 = English (default), 0 = Deutsch
    int  ruleMode = static_cast<int>(BuildRuleMode::Vanilla);
    bool cameraRelativeMove = true;
    bool cursorFollowsMove = false; // Default false so mouse cursor never shakes/fights the game
    bool rotateAsGroup = true;
    bool soundFeedback = true;
    bool visualHighlight = true;
    bool showBlueBoxes = true;
    bool onlyPlayerBlocks = true;
    int  stepMultiplier = 1;

    // Default Keybinds
    int keyToggleMenu = VK_F3;            // 114
    int keyToggleModMode = VK_F4;         // 115
    int keySelectBlock = 'E';             // 69
    int keyConfirmSelection = VK_NUMPAD0; // 96
    int keyBackToSelect = VK_BACK;        // 8

    int keyMoveNorth = 'W';               // 87
    int keyMoveSouth = 'V';               // 86
    int keyMoveWest = 'A';                // 65
    int keyMoveEast = 'D';                // 68
    int keyMoveUp = VK_PRIOR;             // 33 (Page Up)
    int keyMoveDown = VK_NEXT;            // 34 (Page Down)
    int keyRotateCW = 'R';                // 82
    int keyRotateCCW = 'Q';               // 81

    // Optional Direct Tools (0 = Unbound)
    int keyTeleportToCursor = VK_NUMPAD5; // 101
    int keyCloneAtCursor = 'L';           // 76
    int keySelectLastBuilt = 0;
    int keySelectAllBlocks = 0;
    int keySelectSameHeight = 0;
    int keySelectSameType = 0;
    int keyBoxCorner1 = 0;
    int keyBoxSelect2 = 0;
    int keyStepSizeCycle = 0;
    int keyCopySelection = 0;
    int keyPasteBlueprint = 'I';          // 73
};

static ModConfig g_cfg;

static const char* TR(const char* de, const char* en) {
    return (g_cfg.language == 1) ? en : de;
}

// ============================================================================
// 3. Direct Access to CSceneMobils, CHmsItem & CHmsCorpus
// ============================================================================

struct DynamicEditorContext {
    uintptr_t pApp = 0;
    uintptr_t pEditorCat = 0;   // pApp+0x414
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
static volatile bool                   g_needVt31Rebuild = false;
static ULONGLONG                       g_deferredVt31Tick = 0;
static bool                            g_useSingleBoundingBox = false;

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
    g_needVt31Rebuild = true;
}

static uint32_t g_cachedSceneMobilVTable = 0;
static uint32_t g_cachedHmsCorpusVTable = 0;

static void AddUniqueMobilPtr(std::vector<uintptr_t>& outMobils, uintptr_t mob, uintptr_t blockAddr, bool isChildMobil = false) {
    uint32_t vt = 0;
    if (!IsAnyHeapObject(mob, &vt)) return;

    if (g_cachedSceneMobilVTable == 0 || vt != g_cachedSceneMobilVTable) {
        const char* cn = GetHeapObjectClassName(mob);
        if (!cn || !ClassStartsWith(cn, "CSceneMobil")) return;
        if (g_cachedSceneMobilVTable == 0) g_cachedSceneMobilVTable = vt;
    }

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

    // Direkt die bekannten Mobil-Slots (+0x30 und +0x44) prüfen statt 10 Offsets inkl. RTTI zu scannen
    uint32_t ptr1 = 0, ptr2 = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_1, &ptr1) && ptr1 >= 0x10000) {
        AddUniqueMobilPtr(outMobils, ptr1, blockAddr, true);
    }
    if (SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_2, &ptr2) && ptr2 >= 0x10000) {
        AddUniqueMobilPtr(outMobils, ptr2, blockAddr, true);
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
    static uintptr_t s_cachedInfo = 0;
    static bool      s_cachedResult = false;
    if (blockInfo == s_cachedInfo) return s_cachedResult;

    bool isPerim = false;
    for (int off = 0x18; off <= 0x38; off += 4) {
        uint32_t strPtr = 0;
        if (!SafeReadU32(blockInfo + off, &strPtr) || strPtr < 0x10000) continue;
        uint32_t hiByte = strPtr & 0xFF000000u;
        if (hiByte == 0x40000000u || hiByte == 0x3F000000u || hiByte == 0x3E000000u || hiByte >= 0x80000000u) continue;
        uint32_t first4 = 0;
        if (!SafeReadU32(strPtr, &first4)) continue;
        if ((first4 & 0xFFu) != 'S') continue;

        __try {
            const char* s = reinterpret_cast<const char*>(strPtr);
            if (std::strncmp(s, "Stadium", 7) == 0 &&
                (std::strstr(s, "Warp") != nullptr || std::strstr(s, "Tribune") != nullptr ||
                    std::strstr(s, "Sculpt") != nullptr || std::strstr(s, "Screen") != nullptr ||
                    std::strstr(s, "Light") != nullptr)) {
                isPerim = true;
                break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    s_cachedInfo = blockInfo;
    s_cachedResult = isPerim;
    return isPerim;
}

static bool IsMovableTrackBlock(uintptr_t blockAddr) {
    if (!IsAnyHeapObject(blockAddr)) return false;

    uint32_t flags60 = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_FLAGS60, &flags60)) {
        if ((flags60 & 0x2000u) != 0 || (flags60 & 0x3Fu) == 0x3Fu || (flags60 & 0x30000000u) != 0) {
            return false;
        }
    }

    uint32_t mob1 = 0, mob2 = 0;
    SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_1, &mob1);
    SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_2, &mob2);
    if (!IsAnyHeapObject(mob1) && !IsAnyHeapObject(mob2)) {
        return false;
    }

    Int3 c = ReadBlockCoord(blockAddr);

    uint32_t blockInfo = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_BLOCKINFO, &blockInfo) && IsAnyHeapObject(blockInfo)) {
        const char* biName = GetHeapObjectClassName(blockInfo);
        if (biName) {
            if (ClassContainsWord(biName, "BlockInfoClip") || ClassContainsWord(biName, "BlockInfoPylon")) {
                return false;
            }
            // BlockInfoFlat auf Y <= 1 sind die normalen Stadion-Grasbodenkacheln;
            // auf Y >= 2 sind es schwebende Terrain-Blöcke (Hügel/Plattformen in der Luft) -> mitbewegen!
            if (ClassContainsWord(biName, "BlockInfoFlat") && c.y <= 1) {
                return false;
            }
        }
    }

    if (g_cfg.onlyPlayerBlocks && !IsBlockSelected(blockAddr)) {
        if (c.y <= 0 || IsStadiumPerimeterBlockInfo(blockInfo)) return false;
    }

    return true;
}

static void WriteBlockCoord(uintptr_t blockAddr, const Int3& newC) {
    if (!IsAnyHeapObject(blockAddr)) return;
    SafeWriteU32(blockAddr + BLOCK_OFF_COORD_X, static_cast<uint32_t>(newC.x));
    SafeWriteU32(blockAddr + BLOCK_OFF_COORD_Y, static_cast<uint32_t>(newC.y));
    SafeWriteU32(blockAddr + BLOCK_OFF_COORD_Z, static_cast<uint32_t>(newC.z));

    uint32_t flags60 = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_FLAGS60, &flags60)) {
        uint32_t updated = (newC.y <= 1) ? (flags60 | 0x1000u) : (flags60 & ~0x1000u);
        if (updated != flags60) SafeWriteU32(blockAddr + BLOCK_OFF_FLAGS60, updated);
    }

    g_needVt31Rebuild = true;
}

// ============================================================================
// 3b. Auto-Calibrating Multi-Unit Block Dimensions (CGameCtnBlockUnit / Info)
// ============================================================================

enum class BlockUnitOffsetMode : int {
    None = 0,
    UnitInfoLocalU32, // CGameCtnBlockUnit -> CGameCtnBlockUnitInfo* -> local nat3 Offset (rotated by Dir)
    UnitAbsU32,       // CGameCtnBlockUnit -> absolute map nat3 Coord
    UnitRelU32,       // CGameCtnBlockUnit -> relative nat3 Offset
    UnitRelU8         // CGameCtnBlockUnit -> packed uint8 x,y,z
};

struct BlockUnitLayoutInfo {
    bool                calibrated = false;
    BlockUnitOffsetMode mode = BlockUnitOffsetMode::None;
    int                 unitPtrOff = -1;
    int                 dataOff = -1;
    bool                relAlreadyRotated = true;
};

static BlockUnitLayoutInfo g_unitLayout;

static void RotateLocalUnitOffset(int32_t lx, int32_t ly, int32_t lz, int32_t sizeX, int32_t sizeZ, uint32_t dir, int32_t& outRx, int32_t& outRy, int32_t& outRz) {
    outRy = ly;
    switch (dir & 3u) {
    case 0: outRx = lx;                 outRz = lz;                 break;
    case 1: outRx = (sizeZ - 1) - lz;   outRz = lx;                 break;
    case 2: outRx = (sizeX - 1) - lx;   outRz = (sizeZ - 1) - lz;   break;
    case 3: outRx = lz;                 outRz = (sizeX - 1) - lx;   break;
    }
}

static bool ReadBlockCorpusGridCornerDelta(uintptr_t blockAddr, int32_t& outDx, int32_t& outDz) {
    uint32_t mob = 0, hmsItem = 0, cCnt = 0, cArr = 0, corp0 = 0;
    float cx = 0.0f, cz = 0.0f;
    if (!SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_1, &mob) || !IsAnyHeapObject(mob)) return false;
    if (!SafeReadU32(mob + 0x28, &hmsItem) || !IsAnyHeapObject(hmsItem)) return false;
    if (!SafeReadU32(hmsItem + 0x34, &cCnt) || !SafeReadU32(hmsItem + 0x38, &cArr) ||
        cCnt == 0 || !IsSafeWritableHeapAddr(cArr)) return false;
    if (!SafeReadU32(cArr, &corp0) || !IsAnyHeapObject(corp0)) return false;
    if (!SafeReadFloat(corp0 + 0x3C, &cx) || !SafeReadFloat(corp0 + 0x44, &cz)) return false;

    Int3 bPos = ReadBlockCoord(blockAddr);
    outDx = static_cast<int32_t>(std::lround(cx / 32.0f)) - bPos.x;
    outDz = static_cast<int32_t>(std::lround(cz / 32.0f)) - bPos.z;
    return true;
}

static bool ValidateDistinctTriplets(const std::vector<Int3>& pts) {
    for (size_t i = 0; i < pts.size(); ++i) {
        for (size_t j = i + 1; j < pts.size(); ++j) {
            if (pts[i].x == pts[j].x && pts[i].y == pts[j].y && pts[i].z == pts[j].z) return false;
        }
    }
    return true;
}

static void EnsureBlockUnitLayoutCalibrated() {
    if (g_unitLayout.calibrated) return;
    if (!g_ctx.pBlocksArray || g_ctx.blockCount == 0) return;

    struct SampleBlk {
        uintptr_t blk;
        Int3      pos;
        uint32_t  dir;
        std::vector<uintptr_t> units;
    };

    std::vector<SampleBlk> samples;
    std::vector<uint32_t>  seenInfos;

    for (uint32_t i = 0; i < g_ctx.blockCount && samples.size() < 8; ++i) {
        uint32_t b = 0, uCnt = 0, uArr = 0, bInfo = 0;
        if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) || !IsAnyHeapObject(b)) continue;
        if (!SafeReadU32(b + BLOCK_OFF_UNIT_CNT, &uCnt) || !SafeReadU32(b + BLOCK_OFF_UNIT_ARR, &uArr)) continue;
        if (uCnt < 2 || uCnt > 128 || !IsSafeWritableHeapAddr(uArr)) continue;
        if (!IsMovableTrackBlock(b)) continue;

        SafeReadU32(b + BLOCK_OFF_BLOCKINFO, &bInfo);
        if (bInfo != 0 && std::find(seenInfos.begin(), seenInfos.end(), bInfo) != seenInfos.end() && samples.size() >= 3) {
            continue;
        }

        SampleBlk sb{};
        sb.blk = b;
        sb.pos = ReadBlockCoord(b);
        sb.dir = ReadBlockDir(b);
        bool allUnitsOk = true;
        for (uint32_t u = 0; u < uCnt; ++u) {
            uint32_t uPtr = 0;
            if (!SafeReadU32(uArr + u * 4, &uPtr) || !IsAnyHeapObject(uPtr)) {
                allUnitsOk = false;
                break;
            }
            sb.units.push_back(uPtr);
        }
        if (allUnitsOk && sb.units.size() >= 2) {
            samples.push_back(std::move(sb));
            if (bInfo != 0) seenInfos.push_back(bInfo);
        }
    }

    if (samples.empty()) return;

    // 1. Bevorzugt: CGameCtnBlockUnit -> CGameCtnBlockUnitInfo* -> lokales nat3 Offset (dreht immer live mit Dir!)
    int bestPtrOff = -1, bestInfoOff = -1, bestScore = -1;
    for (int pOff = 0x14; pOff <= 0x3C; pOff += 4) {
        uint32_t infoCand = 0;
        if (!SafeReadU32(samples[0].units[0] + pOff, &infoCand) || !IsAnyHeapObject(infoCand)) continue;
        const char* cn = GetHeapObjectClassName(infoCand);
        if (!cn || !ClassContainsWord(cn, "BlockUnitInfo")) continue;

        for (int iOff = 0x14; iOff <= 0x58; iOff += 4) {
            bool allMatch = true;
            int score = 0;
            for (const auto& sb : samples) {
                std::vector<Int3> pts;
                pts.reserve(sb.units.size());
                int32_t minX = 999, minY = 999, minZ = 999;
                int32_t maxX = -1, maxY = -1, maxZ = -1;
                for (uintptr_t uPtr : sb.units) {
                    uint32_t uiPtr = 0, ux = 0, uy = 0, uz = 0;
                    if (!SafeReadU32(uPtr + pOff, &uiPtr) || !IsAnyHeapObject(uiPtr) ||
                        !SafeReadU32(uiPtr + iOff, &ux) || !SafeReadU32(uiPtr + iOff + 4, &uy) || !SafeReadU32(uiPtr + iOff + 8, &uz) ||
                        ux > 24u || uy > 24u || uz > 24u) {
                        allMatch = false;
                        break;
                    }
                    int32_t ix = static_cast<int32_t>(ux), iy = static_cast<int32_t>(uy), iz = static_cast<int32_t>(uz);
                    minX = MinI32(minX, ix); minY = MinI32(minY, iy); minZ = MinI32(minZ, iz);
                    maxX = MaxI32(maxX, ix); maxY = MaxI32(maxY, iy); maxZ = MaxI32(maxZ, iz);
                    pts.push_back({ ix, iy, iz });
                }
                if (!allMatch || minX != 0 || minZ != 0 || minY > 2 || !ValidateDistinctTriplets(pts)) {
                    allMatch = false;
                    break;
                }
                score += 10;
                if ((maxX > 0) + (maxY > 0) + (maxZ > 0) >= 2) score += 15;

                int32_t cdx = 0, cdz = 0;
                if (ReadBlockCorpusGridCornerDelta(sb.blk, cdx, cdz)) {
                    int32_t sizeX = maxX + 1, sizeZ = maxZ + 1;
                    if (sb.dir == 1u && cdx == sizeZ) score += 50;
                    if (sb.dir == 2u && cdx == sizeX && cdz == sizeZ) score += 50;
                    if (sb.dir == 3u && cdz == sizeX) score += 50;
                }
            }
            if (allMatch && score > bestScore) {
                bestScore = score;
                bestPtrOff = pOff;
                bestInfoOff = iOff;
            }
        }
    }

    if (bestPtrOff >= 0 && bestInfoOff >= 0) {
        g_unitLayout.calibrated = true;
        g_unitLayout.mode = BlockUnitOffsetMode::UnitInfoLocalU32;
        g_unitLayout.unitPtrOff = bestPtrOff;
        g_unitLayout.dataOff = bestInfoOff;
        return;
    }

    // 2. Fallback: Direktes absolutes oder relatives nat3 in CGameCtnBlockUnit
    for (int uOff = 0x14; uOff <= 0x44; uOff += 4) {
        bool allAbs = true, allRel = true;
        int  corpusMatchRot = 0, corpusMatchLoc = 0;

        for (const auto& sb : samples) {
            std::vector<Int3> absPts, relPts;
            int32_t aMinX = 9999, aMinY = 9999, aMinZ = 9999;
            int32_t rMinX = 9999, rMinY = 9999, rMinZ = 9999;
            int32_t rMaxX = -1, rMaxZ = -1;

            for (uintptr_t uPtr : sb.units) {
                uint32_t ux = 0, uy = 0, uz = 0;
                if (!SafeReadU32(uPtr + uOff, &ux) || !SafeReadU32(uPtr + uOff + 4, &uy) || !SafeReadU32(uPtr + uOff + 8, &uz)) {
                    allAbs = false; allRel = false; break;
                }
                int32_t ix = static_cast<int32_t>(ux), iy = static_cast<int32_t>(uy), iz = static_cast<int32_t>(uz);

                if (allAbs) {
                    if (ix < sb.pos.x || ix > sb.pos.x + 24 || iy < sb.pos.y || iy > sb.pos.y + 24 || iz < sb.pos.z || iz > sb.pos.z + 24) {
                        allAbs = false;
                    }
                    else {
                        aMinX = MinI32(aMinX, ix); aMinY = MinI32(aMinY, iy); aMinZ = MinI32(aMinZ, iz);
                        absPts.push_back({ ix, iy, iz });
                    }
                }
                if (allRel) {
                    if (ux > 24u || uy > 24u || uz > 24u) {
                        allRel = false;
                    }
                    else {
                        rMinX = MinI32(rMinX, ix); rMinY = MinI32(rMinY, iy); rMinZ = MinI32(rMinZ, iz);
                        rMaxX = MaxI32(rMaxX, ix); rMaxZ = MaxI32(rMaxZ, iz);
                        relPts.push_back({ ix, iy, iz });
                    }
                }
            }
            if (allAbs && (aMinX != sb.pos.x || aMinZ != sb.pos.z || aMinY > sb.pos.y + 2 || !ValidateDistinctTriplets(absPts))) allAbs = false;
            if (allRel && (rMinX != 0 || rMinZ != 0 || rMinY > 2 || !ValidateDistinctTriplets(relPts))) allRel = false;

            if (allRel) {
                int32_t cdx = 0, cdz = 0;
                if (ReadBlockCorpusGridCornerDelta(sb.blk, cdx, cdz)) {
                    int32_t sx = rMaxX + 1, sz = rMaxZ + 1;
                    if (sb.dir == 1u) { if (cdx == sx) ++corpusMatchRot; if (cdx == sz) ++corpusMatchLoc; }
                    if (sb.dir == 3u) { if (cdz == sz) ++corpusMatchRot; if (cdz == sx) ++corpusMatchLoc; }
                }
            }
        }

        if (allAbs) {
            g_unitLayout.calibrated = true;
            g_unitLayout.mode = BlockUnitOffsetMode::UnitAbsU32;
            g_unitLayout.dataOff = uOff;
            return;
        }
        if (allRel) {
            g_unitLayout.calibrated = true;
            g_unitLayout.mode = BlockUnitOffsetMode::UnitRelU32;
            g_unitLayout.dataOff = uOff;
            g_unitLayout.relAlreadyRotated = (corpusMatchRot >= corpusMatchLoc);
            return;
        }
    }

    // 3. Fallback: Gepackte uint8_t (x, y, z) in CGameCtnBlockUnit
    for (int uOff = 0x14; uOff <= 0x44; ++uOff) {
        bool allRelU8 = true;
        for (const auto& sb : samples) {
            std::vector<Int3> pts;
            int32_t minX = 999, minY = 999, minZ = 999;
            for (uintptr_t uPtr : sb.units) {
                uint8_t bx = 0, by = 0, bz = 0;
                if (!SafeReadByte(uPtr + uOff, &bx) || !SafeReadByte(uPtr + uOff + 1, &by) || !SafeReadByte(uPtr + uOff + 2, &bz) ||
                    bx > 24 || by > 24 || bz > 24) {
                    allRelU8 = false;
                    break;
                }
                minX = MinI32(minX, bx); minY = MinI32(minY, by); minZ = MinI32(minZ, bz);
                pts.push_back({ bx, by, bz });
            }
            if (!allRelU8 || minX != 0 || minZ != 0 || minY > 2 || !ValidateDistinctTriplets(pts)) {
                allRelU8 = false;
                break;
            }
        }
        if (allRelU8) {
            g_unitLayout.calibrated = true;
            g_unitLayout.mode = BlockUnitOffsetMode::UnitRelU8;
            g_unitLayout.dataOff = uOff;
            g_unitLayout.relAlreadyRotated = true;
            return;
        }
    }
}

// Liefert ALLE realen 3D-Gridzellen (X, Y, Z), die ein (ggf. mehrteiliger) Block belegt
static void GetBlockOccupiedCells(uintptr_t blockAddr, std::vector<Int3>& outCells) {
    outCells.clear();
    if (!IsAnyHeapObject(blockAddr)) return;

    Int3 bPos = ReadBlockCoord(blockAddr);
    uint32_t uCnt = 0, uArr = 0;
    if (!SafeReadU32(blockAddr + BLOCK_OFF_UNIT_CNT, &uCnt) ||
        !SafeReadU32(blockAddr + BLOCK_OFF_UNIT_ARR, &uArr) ||
        uCnt <= 1 || uCnt > 128 || !IsSafeWritableHeapAddr(uArr)) {
        outCells.push_back(bPos);
        return;
    }

    EnsureBlockUnitLayoutCalibrated();
    if (!g_unitLayout.calibrated) {
        outCells.push_back(bPos);
        return;
    }

    uint32_t dir = ReadBlockDir(blockAddr);

    if (g_unitLayout.mode == BlockUnitOffsetMode::UnitInfoLocalU32) {
        std::vector<Int3> locals;
        locals.reserve(uCnt);
        int32_t maxLX = 0, maxLZ = 0;
        for (uint32_t u = 0; u < uCnt; ++u) {
            uint32_t uPtr = 0, uiPtr = 0, ux = 0, uy = 0, uz = 0;
            if (!SafeReadU32(uArr + u * 4, &uPtr) || !IsAnyHeapObject(uPtr)) continue;
            if (!SafeReadU32(uPtr + g_unitLayout.unitPtrOff, &uiPtr) || !IsAnyHeapObject(uiPtr)) continue;
            if (!SafeReadU32(uiPtr + g_unitLayout.dataOff, &ux) ||
                !SafeReadU32(uiPtr + g_unitLayout.dataOff + 4, &uy) ||
                !SafeReadU32(uiPtr + g_unitLayout.dataOff + 8, &uz)) continue;
            if (ux > 24u || uy > 24u || uz > 24u) continue;
            int32_t lx = static_cast<int32_t>(ux), ly = static_cast<int32_t>(uy), lz = static_cast<int32_t>(uz);
            if (lx > maxLX) maxLX = lx;
            if (lz > maxLZ) maxLZ = lz;
            locals.push_back({ lx, ly, lz });
        }
        int32_t sizeX = maxLX + 1, sizeZ = maxLZ + 1;
        outCells.reserve(locals.size());
        for (const Int3& lc : locals) {
            int32_t rx = 0, ry = 0, rz = 0;
            RotateLocalUnitOffset(lc.x, lc.y, lc.z, sizeX, sizeZ, dir, rx, ry, rz);
            outCells.push_back({ bPos.x + rx, bPos.y + ry, bPos.z + rz });
        }
    }
    else if (g_unitLayout.mode == BlockUnitOffsetMode::UnitAbsU32) {
        outCells.reserve(uCnt);
        for (uint32_t u = 0; u < uCnt; ++u) {
            uint32_t uPtr = 0, ux = 0, uy = 0, uz = 0;
            if (!SafeReadU32(uArr + u * 4, &uPtr) || !IsAnyHeapObject(uPtr)) continue;
            if (!SafeReadU32(uPtr + g_unitLayout.dataOff, &ux) ||
                !SafeReadU32(uPtr + g_unitLayout.dataOff + 4, &uy) ||
                !SafeReadU32(uPtr + g_unitLayout.dataOff + 8, &uz)) continue;
            int32_t ix = static_cast<int32_t>(ux), iy = static_cast<int32_t>(uy), iz = static_cast<int32_t>(uz);
            if (std::abs(ix - bPos.x) <= 32 && std::abs(iy - bPos.y) <= 32 && std::abs(iz - bPos.z) <= 32) {
                outCells.push_back({ ix, iy, iz });
            }
        }
    }
    else if (g_unitLayout.mode == BlockUnitOffsetMode::UnitRelU32 || g_unitLayout.mode == BlockUnitOffsetMode::UnitRelU8) {
        std::vector<Int3> rels;
        rels.reserve(uCnt);
        int32_t maxRX = 0, maxRZ = 0;
        for (uint32_t u = 0; u < uCnt; ++u) {
            uint32_t uPtr = 0;
            if (!SafeReadU32(uArr + u * 4, &uPtr) || !IsAnyHeapObject(uPtr)) continue;
            int32_t rx = 0, ry = 0, rz = 0;
            if (g_unitLayout.mode == BlockUnitOffsetMode::UnitRelU32) {
                uint32_t ux = 0, uy = 0, uz = 0;
                if (!SafeReadU32(uPtr + g_unitLayout.dataOff, &ux) ||
                    !SafeReadU32(uPtr + g_unitLayout.dataOff + 4, &uy) ||
                    !SafeReadU32(uPtr + g_unitLayout.dataOff + 8, &uz) ||
                    ux > 24u || uy > 24u || uz > 24u) continue;
                rx = static_cast<int32_t>(ux); ry = static_cast<int32_t>(uy); rz = static_cast<int32_t>(uz);
            }
            else {
                uint8_t bx = 0, by = 0, bz = 0;
                if (!SafeReadByte(uPtr + g_unitLayout.dataOff, &bx) ||
                    !SafeReadByte(uPtr + g_unitLayout.dataOff + 1, &by) ||
                    !SafeReadByte(uPtr + g_unitLayout.dataOff + 2, &bz) ||
                    bx > 24 || by > 24 || bz > 24) continue;
                rx = bx; ry = by; rz = bz;
            }
            if (rx > maxRX) maxRX = rx;
            if (rz > maxRZ) maxRZ = rz;
            rels.push_back({ rx, ry, rz });
        }
        int32_t sizeX = maxRX + 1, sizeZ = maxRZ + 1;
        outCells.reserve(rels.size());
        for (const Int3& rc : rels) {
            int32_t fx = rc.x, fy = rc.y, fz = rc.z;
            if (!g_unitLayout.relAlreadyRotated) {
                RotateLocalUnitOffset(rc.x, rc.y, rc.z, sizeX, sizeZ, dir, fx, fy, fz);
            }
            outCells.push_back({ bPos.x + fx, bPos.y + fy, bPos.z + fz });
        }
    }

    if (outCells.empty()) outCells.push_back(bPos);
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
    uint32_t vt = 0;
    if (!IsAnyHeapObject(corpusAddr, &vt)) return 0;
    if (g_cachedHmsCorpusVTable == 0 || vt != g_cachedHmsCorpusVTable) {
        const char* cn = GetHeapObjectClassName(corpusAddr);
        if (cn && !ClassStartsWith(cn, "CHmsCorpus")) return 0;
        if (cn && g_cachedHmsCorpusVTable == 0) g_cachedHmsCorpusVTable = vt;
    }

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
    case 0: r[0] = 1.0f;  r[2] = 0.0f;  r[6] = 0.0f;  r[8] = 1.0f;  break;
    case 1: r[0] = 0.0f;  r[2] = -1.0f; r[6] = 1.0f;  r[8] = 0.0f;  break;
    case 2: r[0] = -1.0f; r[2] = 0.0f;  r[6] = 0.0f;  r[8] = -1.0f; break;
    case 3: r[0] = 0.0f;  r[2] = 1.0f;  r[6] = -1.0f; r[8] = 0.0f;  break;
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
// 4. Cursor Access, Main-Thread Scene Rebuild & Visual Highlighting
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

    if (ov.flags60 != 0u) {
        uint32_t curFl60 = 0;
        if (SafeReadU32(ov.blockAddr + BLOCK_OFF_FLAGS60, &curFl60)) {
            uint32_t newFl60 = (curFl60 & ~0xFFFFu) | (ov.flags60 & 0xFFFFu);
            if (!IsAnyHeapObject(ov.pSkin)) newFl60 &= ~0x8000u;
            Int3 curPos = ReadBlockCoord(ov.blockAddr);
            if (curPos.y <= 1) newFl60 |= 0x1000u;
            else               newFl60 &= ~0x1000u;
            SafeWriteU32(ov.blockAddr + BLOCK_OFF_FLAGS60, newFl60);
        }
    }

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

        if (ov.corpus4C != 0 && IsSafeWritableHeapAddr(ov.corpus4C)) {
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
static bool                     g_hasBoxCorner1 = false;
static Int3                     g_boxCorner1 = { 0, 1, 0 };

static void RequestLiveEditorRebuild();

static bool ReadBlockPrimaryCorpusY(uintptr_t blockAddr, float* outY) {
    uint32_t mob = 0, hmsItem = 0, cCnt = 0, cArr = 0, corp0 = 0;
    if (SafeReadU32(blockAddr + BLOCK_OFF_MOBIL_1, &mob) && IsAnyHeapObject(mob) &&
        SafeReadU32(mob + 0x28, &hmsItem) && IsAnyHeapObject(hmsItem) &&
        SafeReadU32(hmsItem + 0x34, &cCnt) && SafeReadU32(hmsItem + 0x38, &cArr) &&
        cCnt > 0 && IsSafeWritableHeapAddr(cArr) &&
        SafeReadU32(cArr, &corp0) && IsAnyHeapObject(corp0) &&
        SafeReadFloat(corp0 + 0x40, outY)) {
        return true;
    }
    return false;
}

// Synchronisiert CHmsCorpus.Y exakt auf (pos.y * 8.0f + desiredPulseY), damit sich +2m niemals aufaddieren!
static bool SyncBlockVisualYToCoord(uintptr_t blockAddr, float desiredPulseY) {
    if (!IsAnyHeapObject(blockAddr)) return false;
    float curY = 0.0f;
    if (!ReadBlockPrimaryCorpusY(blockAddr, &curY)) return false;

    Int3 pos = ReadBlockCoord(blockAddr);
    float expectedY = pos.y * 8.0f + desiredPulseY;
    float diffY = expectedY - curY;
    if (std::fabs(diffY) > 0.05f) {
        return MoveBlockVisualsTargeted(blockAddr, 0.0f, diffY, 0.0f) > 0;
    }
    return true;
}

static void ApplyPulseToEntry(SelectedBlockEntry& entry) {
    if (!g_cfg.visualHighlight || !IsAnyHeapObject(entry.blockAddr)) return;
    if (SyncBlockVisualYToCoord(entry.blockAddr, 2.0f)) {
        entry.appliedPulseY = 2.0f;
    }
}

static void RemovePulseFromEntry(SelectedBlockEntry& entry) {
    if (IsAnyHeapObject(entry.blockAddr)) {
        SyncBlockVisualYToCoord(entry.blockAddr, 0.0f);
    }
    entry.appliedPulseY = 0.0f;
}

static void RemoveAllSelectionPulses() {
    for (auto& entry : g_selectedBlocks) RemovePulseFromEntry(entry);
}

static void ApplyAllSelectionPulses() {
    if (!g_cfg.visualHighlight || g_selectedBlocks.size() > 64) {
        RemoveAllSelectionPulses();
        return;
    }
    for (auto& entry : g_selectedBlocks) ApplyPulseToEntry(entry);
}
static void ClearSelectionWithRestore() {
    bool hadSelection = !g_selectedBlocks.empty();
    RemoveAllSelectionPulses();
    g_selectedBlocks.clear();
    g_hasBoxCorner1 = false;
    g_useSingleBoundingBox = false;
    if (hadSelection) {
        g_needVt31Rebuild = true;
        g_deferredVt31Tick = 0;
        RequestLiveEditorRebuild();
    }
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
    SafeWriteU32(g_ctx.pCursor + 0x14, static_cast<uint32_t>(c.x));
    SafeWriteU32(g_ctx.pCursor + 0x18, static_cast<uint32_t>(c.y));
    SafeWriteU32(g_ctx.pCursor + 0x1C, static_cast<uint32_t>(c.z));
}

static bool RefreshEditorContext() {
    InitExeBounds();
    DynamicEditorContext next{};

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
                next.pApp = appCand;
                g_cachedAppPtrAddr = gAddr;
                break;
            }
        }
    }
    if (!next.pApp) {
        g_ctx = next;
        return false;
    }

    uint32_t ch198 = 0;
    if (SafeReadU32(next.pApp + 0x198, &ch198) && IsAnyHeapObject(ch198)) {
        next.pChallenge = ch198;
    }

    if (next.pChallenge) {
        uint32_t cnt54 = 0, arr58 = 0;
        if (SafeReadU32(next.pChallenge + 0x54, &cnt54) &&
            SafeReadU32(next.pChallenge + 0x58, &arr58) &&
            cnt54 > 0 && cnt54 <= 50000 && arr58 >= 0x10000 && (arr58 & 3) == 0) {
            next.blockCount = cnt54;
            next.pBlocksArray = arr58;
        }
    }

    uint32_t ed414 = 0;
    if (SafeReadU32(next.pApp + 0x414, &ed414) && IsAnyHeapObject(ed414)) {
        next.pEditorCat = ed414;
        uint32_t curBC = 0;
        if (SafeReadU32(ed414 + 0xBC, &curBC) && IsAnyHeapObject(curBC)) {
            next.pCursor = curBC;
        }
        else {
            for (int off = 0x10; off <= 0x200; off += 4) {
                uint32_t field = 0;
                if (!SafeReadU32(ed414 + off, &field) || !IsAnyHeapObject(field)) continue;
                const char* cn = GetHeapObjectClassName(field);
                if (cn && ClassContainsWord(cn, "Cursor")) {
                    next.pCursor = field;
                    break;
                }
            }
        }
    }

    g_ctx = next;
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
            bool didSpawnNew = false;

            if (!g_pendingSpawns.empty()) {
                std::vector<PendingBlockSpawn> toSpawn;
                toSpawn.swap(g_pendingSpawns);

                uint32_t fnVt68 = 0;
                if (SafeReadU32(edVt + 68 * 4, &fnVt68) && fnVt68 >= 0x10000) {
                    RemoveAllSelectionPulses();
                    g_selectedBlocks.clear();
                    g_useSingleBoundingBox = false;
                    uint32_t spawned = 0;

                    for (size_t sIdx = 0; sIdx < toSpawn.size(); ++sIdx) {
                        PendingBlockSpawn sp = toSpawn[sIdx];
                        if (!IsAnyHeapObject(sp.pBlockInfo)) continue;
                        RefreshEditorContext();
                        uint32_t countBefore = g_ctx.blockCount;

                        if (!IsAnyHeapObject(sp.pPlugTree) && g_ctx.pBlocksArray && countBefore > 0) {
                            for (uint32_t bIdx = 0; bIdx < countBefore; ++bIdx) {
                                uint32_t candB = 0, candInfo = 0, candFl60 = 0;
                                if (!SafeReadU32(g_ctx.pBlocksArray + bIdx * 4, &candB) || !IsAnyHeapObject(candB)) continue;
                                SafeReadU32(candB + BLOCK_OFF_BLOCKINFO, &candInfo);
                                if (candInfo != sp.pBlockInfo) continue;
                                SafeReadU32(candB + BLOCK_OFF_FLAGS60, &candFl60);
                                if ((candFl60 & 0x403Fu) == (sp.flags60 & 0x403Fu)) {
                                    sp.pPlugTree = ReadBlockPlugTree(candB);
                                    if (IsAnyHeapObject(sp.pPlugTree)) break;
                                }
                            }
                        }

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

                                if (IsAnyHeapObject(sp.pSkin) || IsAnyHeapObject(sp.pPlugTree) || (sp.flags60 & 0x403Fu) != 0u) {
                                    uint32_t srcCorpus4C = 0;
                                    if (IsAnyHeapObject(sp.pSkin)) {
                                        for (uint32_t bIdx = 0; bIdx < countBefore; ++bIdx) {
                                            uint32_t candB = 0, candSkin = 0;
                                            if (SafeReadU32(g_ctx.pBlocksArray + bIdx * 4, &candB) && candB != newBlk &&
                                                SafeReadU32(candB + BLOCK_OFF_SKIN, &candSkin) && candSkin == sp.pSkin) {
                                                srcCorpus4C = ReadBlockCorpus4C(candB);
                                                break;
                                            }
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
                                        uint32_t mergedFl = (curFl60 & ~0xFFFFu) | (sp.flags60 & 0xFFFFu);
                                        if (sp.pos.y <= 1) mergedFl |= 0x1000u;
                                        else               mergedFl &= ~0x1000u;
                                        SafeWriteU32(newBlk + BLOCK_OFF_FLAGS60, mergedFl);
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
                    didSpawnNew = (spawned > 0);
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
            if (g_selectedBlocks.size() <= 64) {
                for (const auto& item : g_selectedBlocks) {
                    if (IsAnyHeapObject(item.blockAddr) &&
                        std::find(blocksToUpdate.begin(), blocksToUpdate.end(), item.blockAddr) == blocksToUpdate.end()) {
                        blocksToUpdate.push_back(item.blockAddr);
                    }
                }
            }
            for (uintptr_t bAddr : blocksToUpdate) {
                if (IsAnyHeapObject(bAddr)) RefreshBlockMobilsInScene3dMainThread(bAddr);
            }
            ++g_rebuildExecCount;

            if (g_needVt31Rebuild || didSpawnNew) {
                bool deferHeavyRebuild = (!didSpawnNew && g_selectedBlocks.size() > 64 &&
                    g_deferredVt31Tick != 0 && GetTickCount64() < g_deferredVt31Tick);

                if (!deferHeavyRebuild) {
                    g_needVt31Rebuild = false;
                    g_deferredVt31Tick = 0;

                    uint32_t fnVt31 = 0;
                    if (SafeReadU32(edVt + 31 * 4, &fnVt31) && fnVt31 >= 0x10000) {
                        CallEditorVTableRebuildSeh(g_ctx.pEditorCat, fnVt31);
                    }

                    for (const auto& ov : g_clonedSignOverrides) {
                        ApplyClonedSignVisualOverrideToBlock(ov);
                    }

                    // Exakte Y-Synchronisation (verhindert jeglichen Höhen-Drift nach VT[31])
                    if (g_cfg.visualHighlight && g_selectedBlocks.size() <= 64) {
                        for (auto& item : g_selectedBlocks) {
                            if (SyncBlockVisualYToCoord(item.blockAddr, 2.0f)) {
                                item.appliedPulseY = 2.0f;
                            }
                            if (IsAnyHeapObject(item.blockAddr)) {
                                RefreshBlockMobilsInScene3dMainThread(item.blockAddr);
                            }
                        }
                    }
                    else {
                        for (auto& item : g_selectedBlocks) {
                            item.appliedPulseY = 0.0f;
                            if (didSpawnNew) {
                                SyncBlockVisualYToCoord(item.blockAddr, 0.0f);
                                if (IsAnyHeapObject(item.blockAddr)) {
                                    RefreshBlockMobilsInScene3dMainThread(item.blockAddr);
                                }
                            }
                        }
                    }
                }
            }

            if (IsAnyHeapObject(sampleBlock)) {
                uint32_t m1 = 0;
                SafeReadU32(sampleBlock + BLOCK_OFF_MOBIL_1, &m1);
                g_mobilAfterRebuild = m1;
            }
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
// 5. Selection Management, Blueprints & Block Operations
// ============================================================================

struct ClipboardBlockData {
    Int3      relativeCoord;
    uint32_t  dir;
    uint32_t  blockInfoId;     // Block + 0x14
    uintptr_t blockInfoPtr;    // Block + 0x24
    uint32_t  reservedFlags60; // Block + 0x60
};

static std::vector<ClipboardBlockData> g_clipboard;
static std::vector<uintptr_t>          g_clipboardSkins;
static std::vector<uintptr_t>          g_clipboardPlugTrees;
static std::string g_baseDir = ".\\";
static std::string g_iniPath = ".\\BlockMover.ini";
static bool        g_running = true;

static bool      g_dragActive = false;
static bool      g_dragAddMode = true;
static uintptr_t g_lastDragBlock = 0;

static D3DMATRIX g_candView{};
static bool      g_hasCandView = false;
static D3DMATRIX g_d3dView{};
static D3DMATRIX g_d3dProj{};
static bool      g_d3dHas3DMatrices = false;

static volatile LONG g_pendingBeepPacked = 0;
static HANDLE        g_beepEvent = nullptr;

static DWORD WINAPI AsyncBeepWorkerThread(LPVOID) {
    while (g_running) {
        if (g_beepEvent) WaitForSingleObject(g_beepEvent, 250);
        LONG packed = InterlockedExchange(&g_pendingBeepPacked, 0);
        if (packed != 0 && g_cfg.soundFeedback) {
            DWORD freq = static_cast<DWORD>((packed >> 16) & 0xFFFF);
            DWORD dur = static_cast<DWORD>(packed & 0xFFFF);
            if (freq >= 37 && dur > 0) Beep(freq, dur);
        }
    }
    return 0;
}

static void PlayBeepSound(DWORD freq, DWORD dur) {
    if (!g_cfg.soundFeedback) return;
    static bool s_started = false;
    if (!s_started) {
        s_started = true;
        g_beepEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        CreateThread(nullptr, 0, AsyncBeepWorkerThread, nullptr, 0, nullptr);
    }
    LONG packed = static_cast<LONG>(((freq & 0xFFFF) << 16) | (dur & 0xFFFF));
    InterlockedExchange(&g_pendingBeepPacked, packed);
    if (g_beepEvent) SetEvent(g_beepEvent);
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
    g_cfg.keyToggleModMode = VK_F4;
    g_cfg.keyBackToSelect = VK_BACK;
    if (presetId == 0) {
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
    else {
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

static void CycleStepMultiplier(bool reverse = false) {
    const int presets[5] = { 1, 2, 4, 8, 16 };
    int next = 1;
    for (int i = 0; i < 5; ++i) {
        if (g_cfg.stepMultiplier == presets[i]) {
            next = reverse ? presets[(i + 4) % 5] : presets[(i + 1) % 5];
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
    g_cfg.ruleMode = ClampI32(GetPrivateProfileIntA("BlockMover", "RuleMode", 0, ini), 0, 1);

    g_cfg.cameraRelativeMove = GetPrivateProfileIntA("BlockMover", "CameraRelativeMove", 1, ini) != 0;
    g_cfg.cursorFollowsMove = GetPrivateProfileIntA("BlockMover", "CursorFollowsMove", 0, ini) != 0;
    g_cfg.rotateAsGroup = GetPrivateProfileIntA("BlockMover", "RotateAsGroup", 1, ini) != 0;
    g_cfg.soundFeedback = GetPrivateProfileIntA("BlockMover", "SoundFeedback", 1, ini) != 0;
    g_cfg.visualHighlight = GetPrivateProfileIntA("BlockMover", "VisualHighlight", 1, ini) != 0;
    g_cfg.showBlueBoxes = GetPrivateProfileIntA("BlockMover", "ShowBlueBoxes", 1, ini) != 0;
    g_cfg.onlyPlayerBlocks = GetPrivateProfileIntA("BlockMover", "OnlyPlayerBlocks", 1, ini) != 0;
    g_cfg.stepMultiplier = ClampI32(GetPrivateProfileIntA("BlockMover", "StepMultiplier", 1, ini), 1, 32);
    g_centerBlueprintAtCursor = GetPrivateProfileIntA("BlockMover", "CenterBlueprintAtCursor", 1, ini) != 0;

    g_cfg.keySelectBlock = GetPrivateProfileIntA("BlockMover", "KeySelectBlock", 'E', ini);
    if (g_cfg.keySelectBlock <= 0) g_cfg.keySelectBlock = 'E';

    g_cfg.keyToggleMenu = GetPrivateProfileIntA("BlockMover", "KeyToggleMenu", VK_F3, ini);
    if (g_cfg.keyToggleMenu <= 0 || g_cfg.keyToggleMenu == g_cfg.keySelectBlock || g_cfg.keyToggleMenu == 'E') {
        g_cfg.keyToggleMenu = VK_F3;
        WritePrivateProfileStringA("BlockMover", "KeyToggleMenu", "114", ini);
    }

    g_cfg.keyToggleModMode = GetPrivateProfileIntA("BlockMover", "KeyToggleModMode", VK_F4, ini);
    if (g_cfg.keyToggleModMode <= 0 || g_cfg.keyToggleModMode == g_cfg.keySelectBlock ||
        (g_cfg.keyToggleModMode >= VK_F5 && g_cfg.keyToggleModMode <= VK_F7)) {
        g_cfg.keyToggleModMode = VK_F4;
    }

    g_cfg.keyConfirmSelection = GetPrivateProfileIntA("BlockMover", "KeyConfirmSelection", VK_NUMPAD0, ini);
    g_cfg.keyBackToSelect = GetPrivateProfileIntA("BlockMover", "KeyBackToSelect", VK_BACK, ini);

    g_cfg.keyMoveNorth = GetPrivateProfileIntA("BlockMover", "KeyMoveNorth", 'W', ini);
    if (g_cfg.keyMoveNorth <= 0) g_cfg.keyMoveNorth = 'W';
    g_cfg.keyMoveSouth = GetPrivateProfileIntA("BlockMover", "KeyMoveSouth", 'V', ini);
    if (g_cfg.keyMoveSouth <= 0) g_cfg.keyMoveSouth = 'V';
    g_cfg.keyMoveWest = GetPrivateProfileIntA("BlockMover", "KeyMoveWest", 'A', ini);
    if (g_cfg.keyMoveWest <= 0) g_cfg.keyMoveWest = 'A';
    g_cfg.keyMoveEast = GetPrivateProfileIntA("BlockMover", "KeyMoveEast", 'D', ini);
    if (g_cfg.keyMoveEast <= 0) g_cfg.keyMoveEast = 'D';
    g_cfg.keyMoveUp = GetPrivateProfileIntA("BlockMover", "KeyMoveUp", VK_PRIOR, ini);
    if (g_cfg.keyMoveUp <= 0) g_cfg.keyMoveUp = VK_PRIOR;
    g_cfg.keyMoveDown = GetPrivateProfileIntA("BlockMover", "KeyMoveDown", VK_NEXT, ini);
    if (g_cfg.keyMoveDown <= 0) g_cfg.keyMoveDown = VK_NEXT;
    g_cfg.keyRotateCW = GetPrivateProfileIntA("BlockMover", "KeyRotateCW", 'R', ini);
    if (g_cfg.keyRotateCW <= 0) g_cfg.keyRotateCW = 'R';
    g_cfg.keyRotateCCW = GetPrivateProfileIntA("BlockMover", "KeyRotateCCW", 'Q', ini);
    if (g_cfg.keyRotateCCW <= 0) g_cfg.keyRotateCCW = 'Q';

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
    if (g_selectedBlocks.empty()) return;

    std::vector<uint32_t> validBlocks;
    validBlocks.reserve(g_ctx.blockCount);
    for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
        uint32_t b = 0;
        if (SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) && b >= 0x10000) {
            validBlocks.push_back(b);
        }
    }
    std::sort(validBlocks.begin(), validBlocks.end());

    g_selectedBlocks.erase(
        std::remove_if(g_selectedBlocks.begin(), g_selectedBlocks.end(), [&](const SelectedBlockEntry& sel) {
            return !std::binary_search(validBlocks.begin(), validBlocks.end(), static_cast<uint32_t>(sel.blockAddr));
            }),
        g_selectedBlocks.end()
    );
}

static uintptr_t FindBlockAtOrNearCursor(const Int3& target, Int3* outHitCell = nullptr) {
    if (!g_ctx.pBlocksArray || g_ctx.blockCount == 0) return 0;

    uintptr_t exact3DMatch = 0;
    Int3      exactHitCell = target;
    uintptr_t sameColumnNear = 0;
    Int3      sameColNearCell = target;
    int32_t   bestColNearDist = 999999;
    uintptr_t sameColumnAnyY = 0;
    Int3      sameColAnyCell = target;
    int32_t   bestColAnyDist = 999999;
    uintptr_t adjacentFallback = 0;
    Int3      adjCell = target;
    int32_t   bestAdjScore = 999999;

    std::vector<Int3> cells;
    for (int32_t i = static_cast<int32_t>(g_ctx.blockCount) - 1; i >= 0; --i) {
        uint32_t block = 0;
        if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &block) || block < 0x10000) continue;

        // Schneller Grobfilter (Ursprung ist immer minX/minY/minZ eines mehrteiligen Blocks)
        Int3 origin = ReadBlockCoord(block);
        if (target.x < origin.x - 1 || target.x > origin.x + 16) continue;
        if (target.z < origin.z - 1 || target.z > origin.z + 16) continue;
        if (!IsMovableTrackBlock(block)) continue;

        GetBlockOccupiedCells(block, cells);
        for (const Int3& pos : cells) {
            int32_t distXZ = std::abs(pos.x - target.x) + std::abs(pos.z - target.z);
            int32_t distY = std::abs(pos.y - target.y);

            if (distXZ == 0 && distY == 0) {
                exact3DMatch = block;
                exactHitCell = pos;
                break;
            }
            if (distXZ == 0 && distY <= 4 && distY < bestColNearDist) {
                bestColNearDist = distY;
                sameColumnNear = block;
                sameColNearCell = pos;
            }
            if (distXZ == 0 && distY < bestColAnyDist) {
                bestColAnyDist = distY;
                sameColumnAnyY = block;
                sameColAnyCell = pos;
            }
            if (distXZ == 1 && distY <= 1) {
                int32_t score = distXZ * 10 + distY;
                if (score < bestAdjScore) {
                    bestAdjScore = score;
                    adjacentFallback = block;
                    adjCell = pos;
                }
            }
        }
        if (exact3DMatch) break;
    }

    if (exact3DMatch) { if (outHitCell) *outHitCell = exactHitCell;    return exact3DMatch; }
    if (sameColumnNear) { if (outHitCell) *outHitCell = sameColNearCell; return sameColumnNear; }
    if (sameColumnAnyY) { if (outHitCell) *outHitCell = sameColAnyCell;  return sameColumnAnyY; }
    if (adjacentFallback && outHitCell) *outHitCell = adjCell;
    return adjacentFallback;
}

// Findet den Block an der Cursor-Zelle (inkl. aller Multi-Unit-Teile) sowie exakt dort geblockmixte Blöcke
// Findet den Block an der Cursor-Zelle (inkl. Multi-Unit-Teilen, exakt geblockmixten Blöcken
// sowie vollständig zusammenhängenden CGameCtnBlockInfoRectAsym-Strukturen)
static void FindAllBlocksAtOrNearCursor(const Int3& target, std::vector<uintptr_t>& outBlocks) {
    outBlocks.clear();
    Int3 hitCell = target;
    uintptr_t primary = FindBlockAtOrNearCursor(target, &hitCell);
    if (!primary) return;

    outBlocks.push_back(primary);

    // 1. Falls es sich um ein CGameCtnBlockInfoRectAsym handelt (besteht aus einzelnen 1x1-Teilstücken),
    //    alle direkt angrenzenden Teile derselben Struktur (6-Nachbarn) gemeinsam erfassen
    uint32_t primInfo = 0, primSkin = 0;
    SafeReadU32(primary + BLOCK_OFF_BLOCKINFO, &primInfo);
    SafeReadU32(primary + BLOCK_OFF_SKIN, &primSkin);
    uint32_t primDir = ReadBlockDir(primary);
    const char* primBiName = IsAnyHeapObject(primInfo) ? GetHeapObjectClassName(primInfo) : nullptr;

    if (primBiName && ClassContainsWord(primBiName, "BlockInfoRectAsym")) {
        struct RectCand { uintptr_t addr; Int3 pos; uint32_t dir; uint32_t skin; bool visited; };
        std::vector<RectCand> cands;
        cands.reserve(64);

        for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
            uint32_t b = 0, bInfo = 0;
            if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) || b < 0x10000) continue;
            if (!SafeReadU32(b + BLOCK_OFF_BLOCKINFO, &bInfo) || bInfo != primInfo) continue;
            if (!IsMovableTrackBlock(b)) continue;

            uint32_t bSkin = 0;
            SafeReadU32(b + BLOCK_OFF_SKIN, &bSkin);
            cands.push_back({ b, ReadBlockCoord(b), ReadBlockDir(b), bSkin, (b == primary) });
        }

        std::vector<Int3> bfsQueue;
        bfsQueue.push_back(ReadBlockCoord(primary));
        for (size_t qHead = 0; qHead < bfsQueue.size() && outBlocks.size() < 256; ++qHead) {
            Int3 curP = bfsQueue[qHead];
            for (auto& rc : cands) {
                if (rc.visited) continue;
                int32_t dx = std::abs(rc.pos.x - curP.x);
                int32_t dy = std::abs(rc.pos.y - curP.y);
                int32_t dz = std::abs(rc.pos.z - curP.z);

                bool isVerticalNeighbor = (dx == 0 && dz == 0 && dy == 1);
                bool isSideNeighbor = (dy == 0 && (dx + dz) == 1);
                if (!isVerticalNeighbor && !isSideNeighbor) continue;

                // Vertikal direkt aufeinanderliegende Teile (z.B. oberstes Abschlussstück) immer mitnehmen,
                // seitliche Nachbarn bei gleicher Richtung oder gleichem Skin
                bool compatible = isVerticalNeighbor
                    || (rc.dir == primDir)
                    || (primSkin != 0 && rc.skin == primSkin);
                if (!compatible) continue;

                rc.visited = true;
                outBlocks.push_back(rc.addr);
                bfsQueue.push_back(rc.pos);
            }
        }
    }

    // 2. Exakt in derselben Grid-Zelle (hitCell) geblockmixte Blöcke ebenfalls mitnehmen
    std::vector<Int3> cells;
    for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
        uint32_t b = 0;
        if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) || b < 0x10000) continue;
        if (std::find(outBlocks.begin(), outBlocks.end(), b) != outBlocks.end()) continue;

        Int3 origin = ReadBlockCoord(b);
        if (hitCell.x < origin.x || hitCell.x > origin.x + 16 ||
            hitCell.y < origin.y || hitCell.y > origin.y + 16 ||
            hitCell.z < origin.z || hitCell.z > origin.z + 16) continue;
        if (!IsMovableTrackBlock(b)) continue;

        GetBlockOccupiedCells(b, cells);
        for (const Int3& pos : cells) {
            if (pos.x == hitCell.x && pos.y == hitCell.y && pos.z == hitCell.z) {
                outBlocks.push_back(b);
                break;
            }
        }
    }

    // Deterministische Reihenfolge, damit E-Drag bei derselben Struktur nicht flackert
    std::sort(outBlocks.begin(), outBlocks.end());
}

static void SelectLastPlacedBlock() {
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) {
        PlayBeepSound(240, 80);
        return;
    }
    RemoveAllSelectionPulses();
    g_selectedBlocks.clear();
    g_useSingleBoundingBox = false;
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
    g_useSingleBoundingBox = true;
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
    g_useSingleBoundingBox = true;
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
    g_useSingleBoundingBox = false;
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
    g_useSingleBoundingBox = true;

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
    g_hasBoxCorner1 = false; // Clear green box marker once area is selected
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
    g_useSingleBoundingBox = false;

    Int3 target = ReadCursorCoord();
    if (IsAnyHeapObject(g_ctx.pCursor)) g_lastWorldCursorPos = target;

    std::vector<uintptr_t> mixedBlocks;
    FindAllBlocksAtOrNearCursor(target, mixedBlocks);
    if (mixedBlocks.empty()) {
        PlayBeepSound(300, 60);
        return;
    }

    bool anyAlreadySelected = false;
    for (uintptr_t b : mixedBlocks) {
        if (IsBlockSelected(b)) {
            anyAlreadySelected = true;
            break;
        }
    }

    if (anyAlreadySelected) {
        for (uintptr_t b : mixedBlocks) {
            for (auto it = g_selectedBlocks.begin(); it != g_selectedBlocks.end(); ++it) {
                if (it->blockAddr == b) {
                    RemovePulseFromEntry(*it);
                    g_selectedBlocks.erase(it);
                    break;
                }
            }
        }
        RequestLiveEditorRebuild();
        PlayBeepSound(500, 50);
        return;
    }

    for (uintptr_t b : mixedBlocks) {
        if (!IsBlockSelected(b)) {
            g_selectedBlocks.push_back({ b, 0.0f });
        }
    }
    ApplyAllSelectionPulses();
    g_state = ModState::Moving;
    RequestLiveEditorRebuild();
    PlayBeepSound(950, 60);
}

static void HandleContinuousDragSelect(bool selectKeyDown) {
    static Int3 s_lastDragCursor = { -999, -999, -999 };
    if (!selectKeyDown) {
        if (g_dragActive && !g_selectedBlocks.empty()) g_state = ModState::Moving;
        g_dragActive = false;
        g_lastDragBlock = 0;
        s_lastDragCursor = { -999, -999, -999 };
        return;
    }
    ScopedStateLock lock;
    if (!RefreshEditorContext() || !g_ctx.pBlocksArray || g_ctx.blockCount == 0) return;

    Int3 cur = ReadCursorCoord();
    if (g_dragActive && cur.x == s_lastDragCursor.x && cur.y == s_lastDragCursor.y && cur.z == s_lastDragCursor.z) {
        return;
    }
    s_lastDragCursor = cur;

    std::vector<uintptr_t> mixedBlocks;
    FindAllBlocksAtOrNearCursor(cur, mixedBlocks);
    if (mixedBlocks.empty() || mixedBlocks[0] == g_lastDragBlock) return;
    g_lastDragBlock = mixedBlocks[0];

    SanitizeSelection();
    g_useSingleBoundingBox = false;

    bool alreadySelected = false;
    for (uintptr_t b : mixedBlocks) {
        if (IsBlockSelected(b)) {
            alreadySelected = true;
            break;
        }
    }

    if (!g_dragActive) {
        g_dragActive = true;
        g_dragAddMode = !alreadySelected;
    }

    if (g_dragAddMode) {
        bool addedAny = false;
        for (uintptr_t b : mixedBlocks) {
            if (!IsBlockSelected(b)) {
                g_selectedBlocks.push_back({ b, 0.0f });
                addedAny = true;
            }
        }
        if (addedAny) {
            ApplyAllSelectionPulses();
            g_state = ModState::Moving;
            RequestLiveEditorRebuild();
            PlayBeepSound(950, 25);
        }
    }
    else {
        bool removedAny = false;
        for (uintptr_t b : mixedBlocks) {
            for (auto it = g_selectedBlocks.begin(); it != g_selectedBlocks.end(); ++it) {
                if (it->blockAddr == b) {
                    RemovePulseFromEntry(*it);
                    g_selectedBlocks.erase(it);
                    removedAny = true;
                    break;
                }
            }
        }
        if (removedAny) {
            RequestLiveEditorRebuild();
            PlayBeepSound(500, 25);
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
        if (rem >= static_cast<std::streamsize>(count * sizeof(ClipboardBlockData))) {
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

    Int3 liveCur = ReadCursorCoord();
    if (IsAnyHeapObject(g_ctx.pCursor)) {
        g_lastWorldCursorPos = liveCur;
    }
    Int3 targetOrigin = g_lastWorldCursorPos;

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
    g_useSingleBoundingBox = false;

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
    SanitizeSelection();
    if (g_selectedBlocks.empty()) { PlayBeepSound(240, 60); return; }

    bool isVanilla = (g_cfg.ruleMode == static_cast<int>(BuildRuleMode::Vanilla));
    int32_t minX = isVanilla ? VANILLA_MIN_X : 0, maxX = isVanilla ? VANILLA_MAX_X : UNLIMITER_MAX;
    int32_t minY = isVanilla ? VANILLA_MIN_Y : 1, maxY = isVanilla ? VANILLA_MAX_Y : UNLIMITER_MAX;
    int32_t minZ = isVanilla ? VANILLA_MIN_Z : 0, maxZ = isVanilla ? VANILLA_MAX_Z : UNLIMITER_MAX;

    int32_t dx = dirX * g_cfg.stepMultiplier;
    int32_t dy = dirY * g_cfg.stepMultiplier;
    int32_t dz = dirZ * g_cfg.stepMultiplier;

    int32_t maxPosX = 999999, maxNegX = 999999;
    int32_t maxPosY = 999999, maxNegY = 999999;
    int32_t maxPosZ = 999999, maxNegZ = 999999;

    std::vector<Int3> occCells;
    for (const auto& entry : g_selectedBlocks) {
        GetBlockOccupiedCells(entry.blockAddr, occCells);
        for (const Int3& p : occCells) {
            maxPosX = MinI32(maxPosX, maxX - p.x);
            maxNegX = MinI32(maxNegX, p.x - minX);
            maxPosY = MinI32(maxPosY, maxY - p.y);
            maxNegY = MinI32(maxNegY, p.y - minY);
            maxPosZ = MinI32(maxPosZ, maxZ - p.z);
            maxNegZ = MinI32(maxNegZ, p.z - minZ);
        }
    }

    int32_t effDx = (dx > 0) ? ClampI32(dx, 0, MaxI32(0, maxPosX))
        : (dx < 0) ? -ClampI32(-dx, 0, MaxI32(0, maxNegX)) : 0;
    int32_t effDy = (dy > 0) ? ClampI32(dy, 0, MaxI32(0, maxPosY))
        : (dy < 0) ? -ClampI32(-dy, 0, MaxI32(0, maxNegY)) : 0;
    int32_t effDz = (dz > 0) ? ClampI32(dz, 0, MaxI32(0, maxPosZ))
        : (dz < 0) ? -ClampI32(-dz, 0, MaxI32(0, maxNegZ)) : 0;

    if (effDx == 0 && effDy == 0 && effDz == 0) {
        PlayBeepSound(240, 80);
        return;
    }

    int totalVisualHits = 0;
    float fDx = effDx * 32.0f;
    float fDy = effDy * 8.0f;
    float fDz = effDz * 32.0f;

    for (auto& entry : g_selectedBlocks) {
        uintptr_t block = entry.blockAddr;
        Int3 oldPos = ReadBlockCoord(block);
        Int3 newPos = { oldPos.x + effDx, oldPos.y + effDy, oldPos.z + effDz };
        WriteBlockCoord(block, newPos);
        totalVisualHits += MoveBlockVisualsTargeted(block, fDx, fDy, fDz);
    }

    g_lastMovedFloats = totalVisualHits;

    if (g_cfg.cursorFollowsMove) {
        Int3 c = ReadBlockCoord(g_selectedBlocks[0].blockAddr);
        WriteCursorCoord(c);
        g_lastWorldCursorPos = c;
    }

    g_deferredVt31Tick = (g_selectedBlocks.size() > 64) ? (GetTickCount64() + 450) : 0;
    RequestLiveEditorRebuild();
    PlayBeepSound(880, 30);
}

static void MoveSelectedBlocksCameraRelative(int32_t forwardStep, int32_t rightStep, int32_t upStep) {
    int32_t dx = 0, dz = 0;
    if (g_cfg.cameraRelativeMove && g_d3dHas3DMatrices) {
        float sRight = (g_d3dProj._11 >= 0.0f) ? 1.0f : -1.0f;
        float rx = g_d3dView._11 * sRight;
        float rz = g_d3dView._31 * sRight;

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

    if (g_cfg.rotateAsGroup && g_selectedBlocks.size() > 1) {
        for (const auto& entry : g_selectedBlocks) {
            Int3 oldPos = ReadBlockCoord(entry.blockAddr);
            int32_t relX = oldPos.x - pivot.x, relZ = oldPos.z - pivot.z;
            int32_t nextX = clockwise ? (pivot.x - relZ) : (pivot.x + relZ);
            int32_t nextZ = clockwise ? (pivot.z + relX) : (pivot.z - relX);
            if (nextX < minX || nextX > maxX || nextZ < minZ || nextZ > maxZ) {
                ApplyAllSelectionPulses();
                PlayBeepSound(240, 80);
                return;
            }
        }
    }

    uint32_t dirStep = clockwise ? 1u : 3u;
    for (auto& entry : g_selectedBlocks) {
        uintptr_t block = entry.blockAddr;
        uint32_t newDir = (ReadBlockDir(block) + dirStep) & 3u;
        Int3 oldPos = ReadBlockCoord(block), newPos = oldPos;

        if (g_cfg.rotateAsGroup && g_selectedBlocks.size() > 1) {
            int32_t relX = oldPos.x - pivot.x, relZ = oldPos.z - pivot.z;
            newPos.x = clockwise ? (pivot.x - relZ) : (pivot.x + relZ);
            newPos.z = clockwise ? (pivot.z + relX) : (pivot.z - relX);
            WriteBlockCoord(block, newPos);
            MoveBlockVisualsTargeted(block, (newPos.x - oldPos.x) * 32.0f, 0.0f, (newPos.z - oldPos.z) * 32.0f);
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
    if (IsAnyHeapObject(skinPtr)) DumpMemoryRangeToStream(out, "CGameCtnBlockSkin (+0x20)", skinPtr, 0x40);
    if (IsAnyHeapObject(mob0)) DumpMemoryRangeToStream(out, "CSceneMobil (+0x30)", mob0, 0x60);
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

    if (!g_selectedBlocks.empty() && IsAnyHeapObject(g_selectedBlocks[0].blockAddr)) {
        DumpDeepBlockVisualHierarchy(out, g_selectedBlocks[0].blockAddr, "SELECTED-BLOCK [0]");
    }
    out.close();
    PlayBeepSound(1250, 100);
}

// ============================================================================
// 6. Dear ImGui 1.93.0 Direct3D 9 Overlay & Crash-Safe Rendering
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
static char                 g_bpNameBuf[64] = "MyBlueprint";
static bool                 g_imguiWantKeyboard = false;
static volatile bool        g_deviceResetInProgress = false;
static volatile bool        g_editorOverlayActive = false;


static bool IsTypingInBlueprintBox() {
    return (g_menuVisible && g_imguiInitialized && g_imguiWantKeyboard);
}

static bool IsModActiveMoveKey(WPARAM vk) {
    if (!g_cfg.modEnabled || g_state == ModState::Inactive) return false;
    if (!IsAnyHeapObject(g_ctx.pEditorCat) || !IsAnyHeapObject(g_ctx.pCursor) || !IsAnyHeapObject(g_ctx.pChallenge)) {
        return false;
    }
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
    if (msg == WM_ACTIVATEAPP || msg == WM_ACTIVATE) {
        g_dragActive = false;
        g_lastDragBlock = 0;
        return false;
    }

    if (!g_editorOverlayActive) return false;

    if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
        !IsTypingInBlueprintBox() && IsModActiveMoveKey(wParam)) {
        return true;
    }

    if (!g_imguiInitialized) return false;

    LRESULT imguiRes = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    if (imguiRes) return true;

    ImGuiIO& io = ImGui::GetIO();
    switch (msg) {
    case WM_MOUSEMOVE:
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
        if (g_menuVisible && io.WantTextInput) return true;
        break;
    default:
        break;
    }
    return false;
}

static void ApplyTrackManiaImGuiTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.WindowBorderSize = 1.2f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(6.0f, 6.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.10f, 0.14f, 0.94f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.08f, 0.11f, 0.85f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.12f, 0.17f, 0.96f);
    colors[ImGuiCol_Border] = ImVec4(0.16f, 0.58f, 0.78f, 0.55f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.17f, 0.24f, 0.95f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.25f, 0.35f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.32f, 0.45f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.11f, 0.17f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.16f, 0.25f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.10f, 0.85f, 1.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.14f, 0.20f, 0.29f, 0.95f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.32f, 0.46f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.06f, 0.56f, 0.78f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.12f, 0.20f, 0.30f, 0.92f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.17f, 0.30f, 0.44f, 0.98f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.08f, 0.52f, 0.72f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.30f, 0.42f, 0.55f);
    style.ScrollbarSize = 14.0f;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.07f, 0.10f, 0.90f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.42f, 0.58f, 0.95f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.24f, 0.56f, 0.78f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.10f, 0.72f, 0.95f, 1.00f);
}

static bool IsMainStadiumCameraView(IDirect3DDevice9* pDevice, const D3DMATRIX* v) {
    if (!v) return false;

    if (std::fabs(v->_44 - 1.0f) > 0.01f ||
        std::fabs(v->_14) > 0.01f || std::fabs(v->_24) > 0.01f || std::fabs(v->_34) > 0.01f) {
        return false;
    }

    float r0 = v->_11 * v->_11 + v->_12 * v->_12 + v->_13 * v->_13;
    float r1 = v->_21 * v->_21 + v->_22 * v->_22 + v->_23 * v->_23;
    float r2 = v->_31 * v->_31 + v->_32 * v->_32 + v->_33 * v->_33;
    if (std::fabs(r0 - 1.0f) > 0.1f || std::fabs(r1 - 1.0f) > 0.1f || std::fabs(r2 - 1.0f) > 0.1f) {
        return false;
    }

    bool isIdentityRot = (std::fabs(v->_11 - 1.0f) < 1e-3f &&
        std::fabs(v->_22 - 1.0f) < 1e-3f &&
        std::fabs(v->_33 - 1.0f) < 1e-3f);
    if (isIdentityRot) return false;

    float camX = -(v->_41 * v->_11 + v->_42 * v->_12 + v->_43 * v->_13);
    float camY = -(v->_41 * v->_21 + v->_42 * v->_22 + v->_43 * v->_23);
    float camZ = -(v->_41 * v->_31 + v->_42 * v->_32 + v->_43 * v->_33);
    if (camY < 1.0f || (camX * camX + camZ * camZ) < (60.0f * 60.0f)) {
        return false;
    }

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

static bool ProjectWorldToClip(float wx, float wy, float wz, const D3DMATRIX& view, const D3DMATRIX& proj, float& outCx, float& outCy, float& outCw) {
    float vx = wx * view._11 + wy * view._21 + wz * view._31 + view._41;
    float vy = wx * view._12 + wy * view._22 + wz * view._32 + view._42;
    float vz = wx * view._13 + wy * view._23 + wz * view._33 + view._43;
    float vw = wx * view._14 + wy * view._24 + wz * view._34 + view._44;

    outCx = vx * proj._11 + vy * proj._21 + vz * proj._31 + vw * proj._41;
    outCy = vx * proj._12 + vy * proj._22 + vz * proj._32 + vw * proj._42;
    outCw = vx * proj._14 + vy * proj._24 + vz * proj._34 + vw * proj._44;
    return (outCw >= 0.1f);
}

static void DrawClipped3DLineOnImGui(ImDrawList* dl, const ImVec2& dispSize,
    float ax, float ay, float az, float bx, float by, float bz, ImU32 lineCol, float thickness = 2.0f)
{
    float cxa, cya, cwa, cxb, cyb, cwb;
    ProjectWorldToClip(ax, ay, az, g_d3dView, g_d3dProj, cxa, cya, cwa);
    ProjectWorldToClip(bx, by, bz, g_d3dView, g_d3dProj, cxb, cyb, cwb);

    constexpr float kNearW = 0.25f;
    if (cwa < kNearW && cwb < kNearW) return;
    if (cwa < kNearW) {
        float t = (kNearW - cwa) / (cwb - cwa);
        cxa += t * (cxb - cxa);
        cya += t * (cyb - cya);
        cwa = kNearW;
    }
    else if (cwb < kNearW) {
        float t = (kNearW - cwb) / (cwa - cwb);
        cxb += t * (cxa - cxb);
        cyb += t * (cya - cyb);
        cwb = kNearW;
    }

    float nxA = cxa / cwa, nyA = cya / cwa;
    float nxB = cxb / cwb, nyB = cyb / cwb;
    if (std::fabs(nxA) > 12.0f || std::fabs(nyA) > 12.0f || std::fabs(nxB) > 12.0f || std::fabs(nyB) > 12.0f) return;

    ImVec2 sA((nxA * 0.5f + 0.5f) * dispSize.x, (0.5f - nyA * 0.5f) * dispSize.y);
    ImVec2 sB((nxB * 0.5f + 0.5f) * dispSize.x, (0.5f - nyB * 0.5f) * dispSize.y);
    dl->AddLine(sA, sB, lineCol, thickness);
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
        DrawClipped3DLineOnImGui(dl, dispSize,
            corners3D[a][0], corners3D[a][1], corners3D[a][2],
            corners3D[b][0], corners3D[b][1], corners3D[b][2], lineCol, 2.0f);
    }
}

struct ShellQuad3D {
    float p[4][3];
};

struct ShellEdge3D {
    float ax, ay, az;
    float bx, by, bz;
};

struct SelectionGridCell {
    int32_t x, y, z;
    int32_t pulseKey;
    float   pulseY;
};

static bool IsTrueSilhouetteMask(int m) {
    // 0 = leer, 15 = innen, 3/12/5/10 = flache Wand (keine Kante zeichnen!)
    return (m != 0 && m != 15 && m != 3 && m != 12 && m != 5 && m != 10);
}

static void BuildVoxelShellForPulseGroup(
    const std::vector<Int3>& sortedCells, float pulseY,
    std::vector<ShellQuad3D>& outQuads, std::vector<ShellEdge3D>& outEdges)
{
    auto cmpInt3 = [](const Int3& a, const Int3& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.x < b.x;
        };
    auto hasCell = [&](int32_t x, int32_t y, int32_t z) -> bool {
        Int3 k{ x, y, z };
        return std::binary_search(sortedCells.begin(), sortedCells.end(), k, cmpInt3);
        };

    // 1. Nur nach außen sichtbare Flächen sammeln & pro Ebene zu großen Quads verschmelzen (keine Innenwände!)
    struct PlaneCell { int32_t plane, u, v; };
    std::vector<PlaneCell> faceBuckets[6];
    for (const Int3& c : sortedCells) {
        if (!hasCell(c.x, c.y - 1, c.z)) faceBuckets[0].push_back({ c.y,     c.x, c.z }); // -Y
        if (!hasCell(c.x, c.y + 1, c.z)) faceBuckets[1].push_back({ c.y + 1, c.x, c.z }); // +Y
        if (!hasCell(c.x, c.y, c.z - 1)) faceBuckets[2].push_back({ c.z,     c.x, c.y }); // -Z
        if (!hasCell(c.x, c.y, c.z + 1)) faceBuckets[3].push_back({ c.z + 1, c.x, c.y }); // +Z
        if (!hasCell(c.x - 1, c.y, c.z)) faceBuckets[4].push_back({ c.x,     c.z, c.y }); // -X
        if (!hasCell(c.x + 1, c.y, c.z)) faceBuckets[5].push_back({ c.x + 1, c.z, c.y }); // +X
    }

    auto cmpPC = [](const PlaneCell& a, const PlaneCell& b) {
        if (a.plane != b.plane) return a.plane < b.plane;
        if (a.v != b.v) return a.v < b.v;
        return a.u < b.u;
        };

    for (int f = 0; f < 6; ++f) {
        auto& vec = faceBuckets[f];
        if (vec.empty()) continue;
        std::sort(vec.begin(), vec.end(), cmpPC);
        std::vector<uint8_t> used(vec.size(), 0);

        auto findUnused = [&](int32_t plane, int32_t u, int32_t v) -> int {
            PlaneCell key{ plane, u, v };
            auto it = std::lower_bound(vec.begin(), vec.end(), key, cmpPC);
            if (it != vec.end() && it->plane == plane && it->u == u && it->v == v) {
                int idx = static_cast<int>(it - vec.begin());
                if (!used[idx]) return idx;
            }
            return -1;
            };

        for (size_t i = 0; i < vec.size(); ++i) {
            if (used[i]) continue;
            int32_t pl = vec[i].plane, u0 = vec[i].u, u1 = u0, v0 = vec[i].v, v1 = v0;
            while (findUnused(pl, u1 + 1, v0) >= 0) ++u1;
            while (true) {
                bool okV = true;
                for (int32_t u = u0; u <= u1; ++u) {
                    if (findUnused(pl, u, v1 + 1) < 0) { okV = false; break; }
                }
                if (!okV) break;
                ++v1;
            }
            for (int32_t v = v0; v <= v1; ++v) {
                for (int32_t u = u0; u <= u1; ++u) {
                    int idx = findUnused(pl, u, v);
                    if (idx >= 0) used[idx] = 1;
                }
            }

            ShellQuad3D q{};
            if (f == 0 || f == 1) { // Y-Ebene: u=X, v=Z
                float wy = pl * 8.0f + pulseY;
                float x0 = u0 * 32.0f, x1 = (u1 + 1) * 32.0f;
                float z0 = v0 * 32.0f, z1 = (v1 + 1) * 32.0f;
                q.p[0][0] = x0; q.p[0][1] = wy; q.p[0][2] = z0;
                q.p[1][0] = x1; q.p[1][1] = wy; q.p[1][2] = z0;
                q.p[2][0] = x1; q.p[2][1] = wy; q.p[2][2] = z1;
                q.p[3][0] = x0; q.p[3][1] = wy; q.p[3][2] = z1;
            }
            else if (f == 2 || f == 3) { // Z-Ebene: u=X, v=Y
                float wz = pl * 32.0f;
                float x0 = u0 * 32.0f, x1 = (u1 + 1) * 32.0f;
                float y0 = v0 * 8.0f + pulseY, y1 = (v1 + 1) * 8.0f + pulseY;
                q.p[0][0] = x0; q.p[0][1] = y0; q.p[0][2] = wz;
                q.p[1][0] = x1; q.p[1][1] = y0; q.p[1][2] = wz;
                q.p[2][0] = x1; q.p[2][1] = y1; q.p[2][2] = wz;
                q.p[3][0] = x0; q.p[3][1] = y1; q.p[3][2] = wz;
            }
            else { // X-Ebene: u=Z, v=Y
                float wx = pl * 32.0f;
                float z0 = u0 * 32.0f, z1 = (u1 + 1) * 32.0f;
                float y0 = v0 * 8.0f + pulseY, y1 = (v1 + 1) * 8.0f + pulseY;
                q.p[0][0] = wx; q.p[0][1] = y0; q.p[0][2] = z0;
                q.p[1][0] = wx; q.p[1][1] = y0; q.p[1][2] = z1;
                q.p[2][0] = wx; q.p[2][1] = y1; q.p[2][2] = z1;
                q.p[3][0] = wx; q.p[3][1] = y1; q.p[3][2] = z0;
            }
            outQuads.push_back(q);
        }
    }

    // 2. Echte Außen- & Innen-Eckkanten (Silhouette/Crease) extrahieren & kollinear verschmelzen
    std::vector<Int3> candX, candY, candZ;
    candX.reserve(sortedCells.size() * 4);
    candY.reserve(sortedCells.size() * 4);
    candZ.reserve(sortedCells.size() * 4);

    for (const Int3& c : sortedCells) {
        candX.push_back({ c.x, c.y,     c.z });
        candX.push_back({ c.x, c.y + 1, c.z });
        candX.push_back({ c.x, c.y,     c.z + 1 });
        candX.push_back({ c.x, c.y + 1, c.z + 1 });

        candY.push_back({ c.x,     c.y, c.z });
        candY.push_back({ c.x + 1, c.y, c.z });
        candY.push_back({ c.x,     c.y, c.z + 1 });
        candY.push_back({ c.x + 1, c.y, c.z + 1 });

        candZ.push_back({ c.x,     c.y,     c.z });
        candZ.push_back({ c.x + 1, c.y,     c.z });
        candZ.push_back({ c.x,     c.y + 1, c.z });
        candZ.push_back({ c.x + 1, c.y + 1, c.z });
    }

    auto eqInt3 = [](const Int3& a, const Int3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };

    // X-Kanten (sortiert nach y, z, x)
    std::sort(candX.begin(), candX.end(), cmpInt3);
    candX.erase(std::unique(candX.begin(), candX.end(), eqInt3), candX.end());
    std::vector<Int3> keepX;
    for (const Int3& e : candX) {
        int m = (hasCell(e.x, e.y - 1, e.z - 1) ? 1 : 0)
            | (hasCell(e.x, e.y, e.z - 1) ? 2 : 0)
            | (hasCell(e.x, e.y - 1, e.z) ? 4 : 0)
            | (hasCell(e.x, e.y, e.z) ? 8 : 0);
        if (IsTrueSilhouetteMask(m)) keepX.push_back(e);
    }
    for (size_t i = 0; i < keepX.size(); ) {
        size_t j = i;
        while (j + 1 < keepX.size() && keepX[j + 1].y == keepX[i].y && keepX[j + 1].z == keepX[i].z && keepX[j + 1].x == keepX[j].x + 1) ++j;
        float wy = keepX[i].y * 8.0f + pulseY, wz = keepX[i].z * 32.0f;
        outEdges.push_back({ keepX[i].x * 32.0f, wy, wz, (keepX[j].x + 1) * 32.0f, wy, wz });
        i = j + 1;
    }

    // Y-Kanten (sortiert nach x, z, y)
    auto cmpY = [](const Int3& a, const Int3& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.z != b.z) return a.z < b.z;
        return a.y < b.y;
        };
    std::sort(candY.begin(), candY.end(), cmpY);
    candY.erase(std::unique(candY.begin(), candY.end(), eqInt3), candY.end());
    std::vector<Int3> keepY;
    for (const Int3& e : candY) {
        int m = (hasCell(e.x - 1, e.y, e.z - 1) ? 1 : 0)
            | (hasCell(e.x, e.y, e.z - 1) ? 2 : 0)
            | (hasCell(e.x - 1, e.y, e.z) ? 4 : 0)
            | (hasCell(e.x, e.y, e.z) ? 8 : 0);
        if (IsTrueSilhouetteMask(m)) keepY.push_back(e);
    }
    for (size_t i = 0; i < keepY.size(); ) {
        size_t j = i;
        while (j + 1 < keepY.size() && keepY[j + 1].x == keepY[i].x && keepY[j + 1].z == keepY[i].z && keepY[j + 1].y == keepY[j].y + 1) ++j;
        float wx = keepY[i].x * 32.0f, wz = keepY[i].z * 32.0f;
        outEdges.push_back({ wx, keepY[i].y * 8.0f + pulseY, wz, wx, (keepY[j].y + 1) * 8.0f + pulseY, wz });
        i = j + 1;
    }

    // Z-Kanten (sortiert nach x, y, z)
    auto cmpZ = [](const Int3& a, const Int3& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
        };
    std::sort(candZ.begin(), candZ.end(), cmpZ);
    candZ.erase(std::unique(candZ.begin(), candZ.end(), eqInt3), candZ.end());
    std::vector<Int3> keepZ;
    for (const Int3& e : candZ) {
        int m = (hasCell(e.x - 1, e.y - 1, e.z) ? 1 : 0)
            | (hasCell(e.x, e.y - 1, e.z) ? 2 : 0)
            | (hasCell(e.x - 1, e.y, e.z) ? 4 : 0)
            | (hasCell(e.x, e.y, e.z) ? 8 : 0);
        if (IsTrueSilhouetteMask(m)) keepZ.push_back(e);
    }
    for (size_t i = 0; i < keepZ.size(); ) {
        size_t j = i;
        while (j + 1 < keepZ.size() && keepZ[j + 1].x == keepZ[i].x && keepZ[j + 1].y == keepZ[i].y && keepZ[j + 1].z == keepZ[j].z + 1) ++j;
        float wx = keepZ[i].x * 32.0f, wy = keepZ[i].y * 8.0f + pulseY;
        outEdges.push_back({ wx, wy, keepZ[i].z * 32.0f, wx, wy, (keepZ[j].z + 1) * 32.0f });
        i = j + 1;
    }
}

static void RenderImGuiSelectionBoxes3D(IDirect3DDevice9* pDevice) {
    static std::vector<ShellQuad3D> s_cachedQuads;
    static std::vector<ShellEdge3D> s_cachedEdges;
    static bool                     s_hasSingleBox = false;
    static float                    s_singleBox[6] = {};
    static uint64_t                 s_cachedSig = 0;
    static bool                     s_cachedSingleMode = false;

    if (!g_cfg.modEnabled || !g_cfg.showBlueBoxes) return;

    if (g_selectedBlocks.empty()) {
        s_cachedQuads.clear();
        s_cachedEdges.clear();
        s_hasSingleBox = false;
        s_cachedSig = 0;
        if (!g_hasBoxCorner1) return;
    }

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

    if (!g_selectedBlocks.empty()) {
        uint64_t sig = 14695981039346656037ULL;
        auto mix = [&](uint64_t v) { sig ^= v; sig *= 1099511628211ULL; };
        mix(static_cast<uint64_t>(g_selectedBlocks.size()));
        mix(g_useSingleBoundingBox ? 1ULL : 0ULL);
        mix(static_cast<uint64_t>(g_rebuildExecCount));
        mix(static_cast<uint64_t>(g_mobilSceneUpdateCount));
        for (const auto& entry : g_selectedBlocks) {
            mix(static_cast<uint64_t>(entry.blockAddr));
            mix(static_cast<uint64_t>(static_cast<int32_t>(std::lround(entry.appliedPulseY * 10.0f))));
        }
        Int3 pFirst = ReadBlockCoord(g_selectedBlocks.front().blockAddr);
        Int3 pLast = ReadBlockCoord(g_selectedBlocks.back().blockAddr);
        mix(static_cast<uint64_t>(pFirst.x) ^ (static_cast<uint64_t>(pFirst.y) << 16) ^ (static_cast<uint64_t>(pFirst.z) << 32));
        mix(static_cast<uint64_t>(pLast.x) ^ (static_cast<uint64_t>(pLast.y) << 16) ^ (static_cast<uint64_t>(pLast.z) << 32));
        mix(ReadBlockDir(g_selectedBlocks.front().blockAddr));

        if (sig != s_cachedSig || s_cachedSingleMode != g_useSingleBoundingBox) {
            s_cachedSig = sig;
            s_cachedSingleMode = g_useSingleBoundingBox;
            s_cachedQuads.clear();
            s_cachedEdges.clear();
            s_hasSingleBox = false;

            std::vector<Int3> occCells;
            if (g_useSingleBoundingBox) {
                bool hasAny = false;
                float minWX = 1e9f, minWY = 1e9f, minWZ = 1e9f;
                float maxWX = -1e9f, maxWY = -1e9f, maxWZ = -1e9f;
                for (const auto& entry : g_selectedBlocks) {
                    if (!IsAnyHeapObject(entry.blockAddr)) continue;
                    GetBlockOccupiedCells(entry.blockAddr, occCells);
                    for (const Int3& pos : occCells) {
                        float x0 = pos.x * 32.0f;
                        float y0 = pos.y * 8.0f + entry.appliedPulseY;
                        float z0 = pos.z * 32.0f;
                        float x1 = (pos.x + 1) * 32.0f;
                        float y1 = (pos.y + 1) * 8.0f + entry.appliedPulseY;
                        float z1 = (pos.z + 1) * 32.0f;
                        if (x0 < minWX) minWX = x0;
                        if (y0 < minWY) minWY = y0;
                        if (z0 < minWZ) minWZ = z0;
                        if (x1 > maxWX) maxWX = x1;
                        if (y1 > maxWY) maxWY = y1;
                        if (z1 > maxWZ) maxWZ = z1;
                        hasAny = true;
                    }
                }
                if (hasAny) {
                    s_hasSingleBox = true;
                    s_singleBox[0] = minWX - 0.15f; s_singleBox[1] = minWY - 0.15f; s_singleBox[2] = minWZ - 0.15f;
                    s_singleBox[3] = maxWX + 0.15f; s_singleBox[4] = maxWY + 0.15f; s_singleBox[5] = maxWZ + 0.15f;
                }
            }
            else {
                std::vector<SelectionGridCell> allCells;
                allCells.reserve(g_selectedBlocks.size() * 2);
                for (const auto& entry : g_selectedBlocks) {
                    if (!IsAnyHeapObject(entry.blockAddr)) continue;
                    GetBlockOccupiedCells(entry.blockAddr, occCells);
                    int32_t pk = static_cast<int32_t>(std::lround(entry.appliedPulseY * 10.0f));
                    for (const Int3& pos : occCells) {
                        allCells.push_back({ pos.x, pos.y, pos.z, pk, entry.appliedPulseY });
                    }
                }

                auto cmpCell = [](const SelectionGridCell& a, const SelectionGridCell& b) {
                    if (a.pulseKey != b.pulseKey) return a.pulseKey < b.pulseKey;
                    if (a.y != b.y) return a.y < b.y;
                    if (a.z != b.z) return a.z < b.z;
                    return a.x < b.x;
                    };
                std::sort(allCells.begin(), allCells.end(), cmpCell);
                allCells.erase(std::unique(allCells.begin(), allCells.end(), [](const SelectionGridCell& a, const SelectionGridCell& b) {
                    return a.x == b.x && a.y == b.y && a.z == b.z && a.pulseKey == b.pulseKey;
                    }), allCells.end());

                for (size_t i = 0; i < allCells.size(); ) {
                    int32_t curPk = allCells[i].pulseKey;
                    float   curPy = allCells[i].pulseY;
                    std::vector<Int3> groupCells;
                    while (i < allCells.size() && allCells[i].pulseKey == curPk) {
                        groupCells.push_back({ allCells[i].x, allCells[i].y, allCells[i].z });
                        ++i;
                    }
                    BuildVoxelShellForPulseGroup(groupCells, curPy, s_cachedQuads, s_cachedEdges);
                }
            }
        }

        if (s_cachedSingleMode && s_hasSingleBox) {
            DrawProjected3DBoxOnImGui(bgList, dispSize,
                s_singleBox[0], s_singleBox[1], s_singleBox[2],
                s_singleBox[3], s_singleBox[4], s_singleBox[5],
                IM_COL32(0, 229, 255, 245), IM_COL32(0, 180, 255, 18));
        }
        else {
            ImU32 fillCol = IM_COL32(0, 180, 255, 28);
            size_t maxQ = (s_cachedQuads.size() > 512) ? 512 : s_cachedQuads.size();
            for (size_t i = 0; i < maxQ; ++i) {
                const auto& q = s_cachedQuads[i];
                ImVec2 s[4];
                if (ProjectWorldToScreen(q.p[0][0], q.p[0][1], q.p[0][2], g_d3dView, g_d3dProj, dispSize, s[0]) &&
                    ProjectWorldToScreen(q.p[1][0], q.p[1][1], q.p[1][2], g_d3dView, g_d3dProj, dispSize, s[1]) &&
                    ProjectWorldToScreen(q.p[2][0], q.p[2][1], q.p[2][2], g_d3dView, g_d3dProj, dispSize, s[2]) &&
                    ProjectWorldToScreen(q.p[3][0], q.p[3][1], q.p[3][2], g_d3dView, g_d3dProj, dispSize, s[3])) {
                    bgList->AddQuadFilled(s[0], s[1], s[2], s[3], fillCol);
                }
            }
            ImU32 lineCol = IM_COL32(0, 229, 255, 245);
            size_t maxE = (s_cachedEdges.size() > 1024) ? 1024 : s_cachedEdges.size();
            for (size_t i = 0; i < maxE; ++i) {
                const auto& e = s_cachedEdges[i];
                DrawClipped3DLineOnImGui(bgList, dispSize, e.ax, e.ay, e.az, e.bx, e.by, e.bz, lineCol, 2.0f);
            }
        }
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
    bool inEditor = g_editorOverlayActive && (g_ctx.pEditorCat != 0 && g_ctx.pChallenge != 0 && g_ctx.pCursor != 0);
    if (!inEditor) return;

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
        ApplyTrackManiaImGuiTheme();
        ImGui_ImplWin32_Init(hGame);
        ImGui_ImplDX9_Init(pDevice);
        g_imguiDevice = pDevice;
        g_imguiHwnd = hGame;
        g_imguiInitialized = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    // Only draw ImGui software cursor when the full studio panel is open (prevents double-cursor in editor)
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
    g_imguiWantKeyboard = g_menuVisible && io.WantTextInput;

    static Int3 s_prevHwCursor = { -1, -1, -1 };
    Int3 hwCur = ReadCursorCoord();
    if (hwCur.x != s_prevHwCursor.x || hwCur.y != s_prevHwCursor.y || hwCur.z != s_prevHwCursor.z) {
        g_lastWorldCursorPos = hwCur;
        s_prevHwCursor = hwCur;
    }

    RenderImGuiSelectionBoxes3D(pDevice);

    // Clickable quick-toggle pill in the top-left corner so players don't need to memorize F3!
    if (!g_menuVisible) {
        ImGui::SetNextWindowPos(ImVec2(14.0f, 12.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.78f);
        ImGuiWindowFlags pillFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##BlockMoverQuickPill", nullptr, pillFlags)) {
            char pillLbl[64];
            if (!g_cfg.modEnabled) {
                sprintf_s(pillLbl, "BlockMover [OFF] (F3)##pill");
            }
            else if (!g_selectedBlocks.empty()) {
                sprintf_s(pillLbl, "BlockMover [%zu sel] (F3)##pill", g_selectedBlocks.size());
            }
            else {
                sprintf_s(pillLbl, "BlockMover (F3)##pill");
            }
            if (ImGui::SmallButton(pillLbl)) {
                g_menuVisible = true;
                g_lastWorldCursorPos = ReadCursorCoord();
                RefreshBlueprintFileList();
                PlayBeepSound(950, 45);
            }
        }
        ImGui::End();
    }
    else {
        const size_t selCount = g_selectedBlocks.size();
        const bool hasSelection = (selCount > 0);
        Int3 curPos = g_lastWorldCursorPos;

        static bool   s_prevModEnabled = g_cfg.modEnabled;
        static ImVec2 s_savedExpandedSize(495.0f, 650.0f);

        ImGui::SetNextWindowPos(ImVec2(18.0f, 18.0f), ImGuiCond_FirstUseEver);

        ImGuiWindowFlags winFlags = 0;
        if (!g_cfg.modEnabled) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 82.0f), ImVec2(900.0f, 180.0f));
            winFlags = ImGuiWindowFlags_AlwaysAutoResize;
        }
        else {
            ImGui::SetNextWindowSizeConstraints(ImVec2(450.0f, 340.0f), ImVec2(1050.0f, 980.0f));
            winFlags = ImGuiWindowFlags_AlwaysVerticalScrollbar;
            if (!s_prevModEnabled) {
                ImGui::SetNextWindowSize(s_savedExpandedSize, ImGuiCond_Always);
            }
            else {
                ImGui::SetNextWindowSize(s_savedExpandedSize, ImGuiCond_FirstUseEver);
            }
        }
        s_prevModEnabled = g_cfg.modEnabled;

        bool open = g_menuVisible;
        if (ImGui::Begin("BlockMover Studio (F3)###MainStudio", &open, winFlags)) {
            if (g_cfg.modEnabled) {
                ImVec2 curWinSize = ImGui::GetWindowSize();
                if (curWinSize.x >= 445.0f && curWinSize.y >= 335.0f) {
                    s_savedExpandedSize = curWinSize;
                }
            }

            float availW = ImGui::GetContentRegionAvail().x;
            float halfW = (availW - 8.0f) * 0.5f;

            // =================================================================
            // MASTER SWITCH: "Enable Mod"
            // =================================================================
            if (ImGui::Checkbox(TR("Mod aktivieren###MasterSw", "Enable Mod###MasterSw"), &g_cfg.modEnabled)) {
                if (!g_cfg.modEnabled) {
                    ClearSelectionWithRestore();
                }
                else if (g_state == ModState::Inactive) {
                    g_state = ModState::Selecting;
                }
                SaveConfig();
            }

            ImGui::SameLine();
            if (!g_cfg.modEnabled) {
                ImGui::TextDisabled("| OFF");
            }
            else if (hasSelection) {
                ImGui::TextColored(ImVec4(0.10f, 0.90f, 1.00f, 1.0f), "| %zu %s | (%d,%d,%d)",
                    selCount, TR("gew\xC3\xA4hlt", "selected"), curPos.x, curPos.y, curPos.z);
            }
            else {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "| %s | (%d,%d,%d)",
                    TR("Bereit [E]", "Ready [E]"), curPos.x, curPos.y, curPos.z);
            }

            float rightLangX = ImGui::GetCursorStartPos().x + availW - 48.0f;
            if (rightLangX > ImGui::GetCursorPosX()) ImGui::SameLine(rightLangX);
            if (ImGui::Button(g_cfg.language == 0 ? "EN###LangBtn" : "DE###LangBtn", ImVec2(48.0f, 22.0f))) {
                g_cfg.language = (g_cfg.language == 0) ? 1 : 0;
                SaveConfig();
            }

            if (!g_cfg.modEnabled) {
                ImGui::Separator();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", TR("Aktiviere 'Mod aktivieren', um Werkzeuge anzuzeigen.",
                    "Check 'Enable Mod' above to reveal studio tools."));
                ImGui::PopTextWrapPos();
            }
            else {
                ImGui::Separator();

                // =============================================================
                // 1. CONTROL
                // =============================================================
                if (ImGui::CollapsingHeader(TR("Steuerung###HdrControl", "Control###HdrControl"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    const float gap = 6.0f;
                    float rightColW = MaxI32(114, static_cast<int32_t>(availW * 0.25f)) * 1.0f;
                    float leftPadW = availW - rightColW - 12.0f;
                    float btnW = (leftPadW - 2.0f * gap) / 3.0f;
                    float btnH = 28.0f;

                    if (ImGui::Button(TR("90\xC2\xB0 Links###rotL", "CCW 90\xC2\xB0###rotL"), ImVec2(btnW, btnH))) RotateSelectedBlocks(false);
                    ImGui::SameLine(0.0f, gap);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.27f, 0.40f, 1.0f));
                    if (ImGui::Button(TR("Vorw\xC3\xA4rts###fwd", "Forward###fwd"), ImVec2(btnW, btnH))) MoveSelectedBlocksCameraRelative(1, 0, 0);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, gap);
                    if (ImGui::Button(TR("90\xC2\xB0 Rechts###rotR", "CW 90\xC2\xB0###rotR"), ImVec2(btnW, btnH))) RotateSelectedBlocks(true);

                    ImGui::SameLine(0.0f, 12.0f);
                    if (ImGui::Button(TR("Hoch (+Y)###up", "Up (+Y)###up"), ImVec2(rightColW, btnH))) MoveSelectedBlocksCameraRelative(0, 0, 1);

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.27f, 0.40f, 1.0f));
                    if (ImGui::Button(TR("Links###lft", "Left###lft"), ImVec2(btnW, btnH))) MoveSelectedBlocksCameraRelative(0, -1, 0);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, gap);
                    if (ImGui::Button(TR("Zum Cursor###cur", "To Cursor###cur"), ImVec2(btnW, btnH))) {
                        Int3 liveTarget = ReadCursorCoord();
                        if (IsAnyHeapObject(g_ctx.pCursor)) g_lastWorldCursorPos = liveTarget;
                        TeleportSelectedBlocksToTarget(g_lastWorldCursorPos);
                    }
                    ImGui::SameLine(0.0f, gap);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.27f, 0.40f, 1.0f));
                    if (ImGui::Button(TR("Rechts###rgt", "Right###rgt"), ImVec2(btnW, btnH))) MoveSelectedBlocksCameraRelative(0, 1, 0);
                    ImGui::PopStyleColor();

                    ImGui::SameLine(0.0f, 12.0f);
                    char stepBtnLbl[40];
                    sprintf_s(stepBtnLbl, "%s: %dx###stp", TR("Schritt", "Step"), g_cfg.stepMultiplier);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.42f, 0.58f, 1.0f));
                    if (ImGui::Button(stepBtnLbl, ImVec2(rightColW, btnH))) CycleStepMultiplier(false);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) CycleStepMultiplier(true);
                    ImGui::PopStyleColor();

                    ImGui::Dummy(ImVec2(btnW, btnH));
                    ImGui::SameLine(0.0f, gap);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.27f, 0.40f, 1.0f));
                    if (ImGui::Button(TR("Zur\xC3\xBC""ck###bck", "Back###bck"), ImVec2(btnW, btnH))) MoveSelectedBlocksCameraRelative(-1, 0, 0);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, gap);
                    ImGui::Dummy(ImVec2(btnW, btnH));

                    ImGui::SameLine(0.0f, 12.0f);
                    if (ImGui::Button(TR("Runter (-Y)###dn", "Down (-Y)###dn"), ImVec2(rightColW, btnH))) MoveSelectedBlocksCameraRelative(0, 0, -1);

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.46f, 0.64f, 1.0f));
                    if (ImGui::Button(TR("Auswahl am Cursor klonen (L)###btnClone", "Clone Selection at Cursor (L)###btnClone"), ImVec2(availW, 26.0f))) {
                        CopySelectedBlocks();
                        PasteFormationToSelection();
                    }
                    ImGui::PopStyleColor();

                    if (ImGui::TreeNode(TR("Weitere Steuerungs-Optionen###NodeCtrlOpt", "More Control Options###NodeCtrlOpt"))) {
                        ImGui::Checkbox(TR("Kamerarelativ bewegen###chkCam", "Camera-Relative Movement###chkCam"), &g_cfg.cameraRelativeMove);
                        ImGui::Checkbox(TR("Als Gruppe drehen (90\xC2\xB0)###chkGrp", "Rotate Group Together###chkGrp"), &g_cfg.rotateAsGroup);
                        ImGui::Checkbox(TR("Cursor folgt beim Bewegen###chkCur", "Cursor Follows Selection###chkCur"), &g_cfg.cursorFollowsMove);
                        if (ImGui::Checkbox(TR("Auswahl +2m schweben lassen###chkFlt", "Float Highlight (+2m)###chkFlt"), &g_cfg.visualHighlight)) {
                            if (g_cfg.visualHighlight) ApplyAllSelectionPulses();
                            else RemoveAllSelectionPulses();
                            RequestLiveEditorRebuild();
                        }
                        ImGui::Checkbox(TR("3D-Auswahlboxen anzeigen###chkBox", "Show 3D Wireframe Boxes###chkBox"), &g_cfg.showBlueBoxes);
                        ImGui::TreePop();
                    }
                }

                // =============================================================
                // 2. SELECTION
                // =============================================================
                if (ImGui::CollapsingHeader(TR("Auswahl###HdrSelection", "Selection###HdrSelection"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", TR("Bewege den Editor-Cursor auf einen Block und dr\xC3\xBC""cke [E] (oder Klick):",
                        "Point the editor cursor at any block and press [E] (or click below):"));
                    ImGui::PopTextWrapPos();

                    if (ImGui::Button(TR("Cursor-Block w\xC3\xA4hlen [E]###btnPick", "Pick Block at Cursor [E]###btnPick"), ImVec2(halfW, 26.0f))) ToggleBlockAtCursor();
                    ImGui::SameLine(0.0f, 8.0f);
                    if (ImGui::Button(TR("Ganze Strecke w\xC3\xA4hlen###btnAll", "Select Entire Track###btnAll"), ImVec2(halfW, 26.0f))) SelectAllMapBlocks();

                    if (ImGui::Button(TR("Gleiche Y-H\xC3\xB6he###btnSameY", "Same Height (Y)###btnSameY"), ImVec2(halfW, 26.0f))) SelectBlocksOnSameHeightY();
                    ImGui::SameLine(0.0f, 8.0f);
                    if (ImGui::Button(TR("Gleicher Blocktyp###btnSameT", "Same Block Type###btnSameT"), ImVec2(halfW, 26.0f))) SelectBlocksOfSameType();

                    if (hasSelection) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.54f, 0.16f, 0.19f, 0.95f));
                        if (ImGui::Button(TR("Auswahl aufheben (Backspace)###btnDesel", "Deselect All (Backspace)###btnDesel"), ImVec2(availW, 25.0f))) {
                            ClearSelectionWithRestore();
                            g_state = ModState::Selecting;
                            PlayBeepSound(400, 60);
                        }
                        ImGui::PopStyleColor();
                    }

                    if (ImGui::TreeNode(TR("Bereichs-Box & Letzter Block###NodeSelBox", "Area Box & Last Placed###NodeSelBox"))) {
                        if (ImGui::Button(TR("Letzten gebauten Block w\xC3\xA4hlen###btnLast", "Select Last Placed Block###btnLast"), ImVec2(availW, 25.0f))) SelectLastPlacedBlock();

                        char c1Label[64];
                        if (g_hasBoxCorner1) sprintf_s(c1Label, "%s (%d,%d,%d)###btnC1", TR("Ecke 1:", "Corner 1:"), g_boxCorner1.x, g_boxCorner1.y, g_boxCorner1.z);
                        else sprintf_s(c1Label, "%s###btnC1", TR("1. Box-Ecke 1 setzen", "1. Set Box Corner 1"));
                        if (ImGui::Button(c1Label, ImVec2(halfW, 25.0f))) SetBoxCorner1AtCursor();
                        ImGui::SameLine(0.0f, 8.0f);
                        if (ImGui::Button(TR("2. Box bis Cursor###btnC2", "2. Select Box to Cursor###btnC2"), ImVec2(halfW, 25.0f))) SelectBlocksInBoxToCursor();

                        ImGui::SetNextItemWidth(availW - 118.0f);
                        if (ImGui::InputInt3("##TargetCurXYZ", &g_lastWorldCursorPos.x)) {
                            g_lastWorldCursorPos.x = ClampI32(g_lastWorldCursorPos.x, 0, UNLIMITER_MAX);
                            g_lastWorldCursorPos.y = ClampI32(g_lastWorldCursorPos.y, 1, UNLIMITER_MAX);
                            g_lastWorldCursorPos.z = ClampI32(g_lastWorldCursorPos.z, 0, UNLIMITER_MAX);
                            WriteCursorCoord(g_lastWorldCursorPos);
                        }
                        ImGui::SameLine(0.0f, 8.0f);
                        if (ImGui::Button(TR("Auf Auswahl###btnToSel", "To Selection###btnToSel"), ImVec2(-1.0f, 0.0f)) && hasSelection) {
                            g_lastWorldCursorPos = ReadBlockCoord(g_selectedBlocks[0].blockAddr);
                            WriteCursorCoord(g_lastWorldCursorPos);
                        }
                        ImGui::TreePop();
                    }
                }

                // =============================================================
                // 3. BLUEPRINTS (.tmgroup)
                // =============================================================
                if (ImGui::CollapsingHeader(TR("Blaupausen###HdrBlueprints", "Blueprints###HdrBlueprints"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    float saveBtnW = 92.0f;
                    ImGui::SetNextItemWidth(availW - saveBtnW - 8.0f);
                    ImGui::InputTextWithHint("##BpNameInput", TR("Blueprint-Name...", "Blueprint name..."), g_bpNameBuf, sizeof(g_bpNameBuf));
                    ImGui::SameLine(0.0f, 8.0f);
                    if (ImGui::Button(TR("Speichern###bpSave", "Save###bpSave"), ImVec2(saveBtnW, 0.0f))) {
                        if (hasSelection) {
                            CopySelectedBlocks();
                            SaveBlueprintToFile(g_bpNameBuf);
                        }
                        else {
                            PlayBeepSound(240, 80);
                        }
                    }

                    ImGuiWindowFlags bpChildFlags = (g_blueprintFiles.size() <= 4) ? ImGuiWindowFlags_NoScrollWithMouse : 0;
                    ImGui::BeginChild("BpFilesScroll", ImVec2(0.0f, 100.0f), ImGuiChildFlags_Borders, bpChildFlags);
                    if (g_blueprintFiles.empty()) {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextDisabled("%s", TR("Keine .tmgroup Dateien im Ordner Blueprints\\ gefunden.",
                            "No .tmgroup files found in Blueprints\\ folder yet."));
                        ImGui::PopTextWrapPos();
                    }
                    else {
                        for (size_t i = 0; i < g_blueprintFiles.size(); ++i) {
                            const std::string& fn = g_blueprintFiles[i];
                            bool isCurrent = (_stricmp(fn.c_str(), g_bpNameBuf) == 0);
                            if (ImGui::Selectable((fn + ".tmgroup##bp_" + std::to_string(i)).c_str(),
                                isCurrent, ImGuiSelectableFlags_AllowDoubleClick)) {
                                strncpy_s(g_bpNameBuf, fn.c_str(), sizeof(g_bpNameBuf) - 1);
                                LoadBlueprintFromFile(g_bpNameBuf);
                                if (ImGui::IsMouseDoubleClicked(0)) PasteFormationToSelection();
                            }
                        }
                    }
                    ImGui::EndChild();

                    float refreshBtnW = 92.0f;
                    char pasteLbl[64];
                    sprintf_s(pasteLbl, "%s (%zu)###bpPaste", TR("Paste am Cursor (I)", "Paste at Cursor (I)"), g_clipboard.size());
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.46f, 0.64f, 1.0f));
                    if (ImGui::Button(pasteLbl, ImVec2(availW - refreshBtnW - 8.0f, 26.0f))) {
                        PasteFormationToSelection();
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, 8.0f);
                    if (ImGui::Button(TR("Scannen###bpScan", "Refresh###bpScan"), ImVec2(refreshBtnW, 26.0f))) {
                        RefreshBlueprintFileList();
                    }

                    if (ImGui::TreeNode(TR("Blueprint-Optionen###NodeBpOpt", "Blueprint Options###NodeBpOpt"))) {
                        ImGui::Checkbox(TR("Horizontal (X/Z) am Cursor zentrieren###chkCtr", "Center Horizontally (X/Z) at Cursor###chkCtr"), &g_centerBlueprintAtCursor);
                        ImGui::TreePop();
                    }
                }

                // =============================================================
                // 4. HOTKEYS & ADVANCED
                // =============================================================
                if (ImGui::CollapsingHeader(TR("Hotkeys & Erweitert###HdrAdvanced", "Hotkeys & Advanced###HdrAdvanced"))) {
                    bool unlimiterMode = (g_cfg.ruleMode == static_cast<int>(BuildRuleMode::TMUnlimiter));
                    if (ImGui::Checkbox(TR("TMUnlimiter-Raster (0 - 255)###chkUnl", "TMUnlimiter Grid (0 - 255)###chkUnl"), &unlimiterMode)) {
                        g_cfg.ruleMode = unlimiterMode ? 1 : 0;
                    }
                    ImGui::Checkbox(TR("Sound-Feedback (Beep)###chkSnd", "Sound Feedback (Beep)###chkSnd"), &g_cfg.soundFeedback);

                    if (ImGui::TreeNode(TR("Tastenbelegung anpassen###NodeKb", "Customize Keybinds###NodeKb"))) {
                        if (ImGui::Button("Preset: WASD###pr0", ImVec2(halfW, 24.0f))) { ApplyKeybindPreset(0); SaveConfig(); }
                        ImGui::SameLine(0.0f, 8.0f);
                        if (ImGui::Button("Preset: Numpad###pr1", ImVec2(halfW, 24.0f))) { ApplyKeybindPreset(1); SaveConfig(); }

                        struct ShortKbItem { const char* nameDe; const char* nameEn; int* ptr; };
                        ShortKbItem kbItems[22] = {
                            { "GUI Men\xC3\xBC:",          "GUI Menu:",         &g_cfg.keyToggleMenu },
                            { "Mod Ein/Aus:",             "Mod On/Off:",       &g_cfg.keyToggleModMode },
                            { "Block ausw\xC3\xA4hlen:",  "Select Block:",     &g_cfg.keySelectBlock },
                            { "Auswahl absetzen:",        "Deselect All:",     &g_cfg.keyConfirmSelection },
                            { "Vorw\xC3\xA4rts:",         "Move Forward:",     &g_cfg.keyMoveNorth },
                            { "R\xC3\xBC""ckw\xC3\xA4rts:", "Move Back:",      &g_cfg.keyMoveSouth },
                            { "Links:",                   "Move Left:",        &g_cfg.keyMoveWest },
                            { "Rechts:",                  "Move Right:",       &g_cfg.keyMoveEast },
                            { "Hoch (+Y):",               "Move Up (+Y):",     &g_cfg.keyMoveUp },
                            { "Runter (-Y):",             "Move Down (-Y):",   &g_cfg.keyMoveDown },
                            { "90\xC2\xB0 Rechts:",       "Rotate 90\xC2\xB0 CW:",  &g_cfg.keyRotateCW },
                            { "90\xC2\xB0 Links:",        "Rotate 90\xC2\xB0 CCW:", &g_cfg.keyRotateCCW },
                            { "Zum Cursor!:",             "Teleport Cursor:",  &g_cfg.keyTeleportToCursor },
                            { "Am Cursor klonen:",        "Clone at Cursor:",  &g_cfg.keyCloneAtCursor },
                            { "Letzter Block:",           "Last Placed Blk:",  &g_cfg.keySelectLastBuilt },
                            { "Alle Bl\xC3\xB6""cke:",    "All Map Blocks:",   &g_cfg.keySelectAllBlocks },
                            { "Gleiche Y-H\xC3\xB6he:",   "Same Y-Height:",    &g_cfg.keySelectSameHeight },
                            { "Gleicher Blocktyp:",       "Same Block Type:",  &g_cfg.keySelectSameType },
                            { "Box-Ecke 1 setzen:",       "Set Box Corner 1:", &g_cfg.keyBoxCorner1 },
                            { "Box-Bereich w\xC3\xA4hlen:", "Select Box Area:", &g_cfg.keyBoxSelect2 },
                            { "Schrittweite (1-16):",     "Cycle Step Size:",  &g_cfg.keyStepSizeCycle },
                            { "Spawnen (Paste):",         "Spawn Blueprint:",  &g_cfg.keyPasteBlueprint }
                        };

                        ImGui::BeginChild("KbCompactScroll", ImVec2(0.0f, 175.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                        for (int i = 0; i < 22; ++i) {
                            ImGui::Text("%s", TR(kbItems[i].nameDe, kbItems[i].nameEn));
                            ImGui::SameLine(185.0f);
                            bool isBindingThis = (g_bindingTarget == kbItems[i].ptr);
                            std::string btnLbl = (isBindingThis ? TR("Taste dr\xC3\xBC""cken...", "Press key...") : ("[ " + GetKeyName(*kbItems[i].ptr) + " ]")) + "###kb" + std::to_string(i);
                            if (ImGui::Button(btnLbl.c_str(), ImVec2(-1.0f, 22.0f))) {
                                g_bindingTarget = kbItems[i].ptr;
                            }
                        }
                        ImGui::EndChild();
                        ImGui::TreePop();
                    }

                    if (ImGui::TreeNode(TR("Streckenbl\xC3\xB6""cke-Liste###NodeBlkList", "Track Blocks List###NodeBlkList"))) {
                        ImGui::Checkbox(TR("Stadion-Au\xC3\x9F""enring ausblenden###chkPerim", "Hide Stadium Perimeter###chkPerim"), &g_cfg.onlyPlayerBlocks);
                        ImGui::BeginChild("AdvBlockList", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                        if (g_ctx.pBlocksArray && g_ctx.blockCount > 0) {
                            const char* dirNames[4] = { "N", "E", "S", "W" };
                            int shown = 0;
                            for (int32_t i = static_cast<int32_t>(g_ctx.blockCount) - 1; i >= 0 && shown < 250; --i) {
                                uint32_t b = 0;
                                if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) || !IsMovableTrackBlock(b)) continue;
                                Int3 pos = ReadBlockCoord(b);
                                uint32_t dir = ReadBlockDir(b);
                                bool sel = IsBlockSelected(b);
                                char rowBuf[128];
                                sprintf_s(rowBuf, "%s#%d  |  (%d, %d, %d)  |  %s##blk_%u",
                                    sel ? "[*] " : "    ", i, pos.x, pos.y, pos.z, dirNames[dir & 3u], b);
                                if (ImGui::Selectable(rowBuf, sel)) {
                                    g_useSingleBoundingBox = false;
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
                        ImGui::TreePop();
                    }

                    if (ImGui::Button(TR("Einstellungen speichern (.ini)###btnIni", "Save Settings (.ini)###btnIni"), ImVec2(halfW, 25.0f))) {
                        SaveConfig();
                        PlayBeepSound(1200, 80);
                    }
                    ImGui::SameLine(0.0f, 8.0f);
                    if (ImGui::Button(TR("Diagnose speichern (.txt)###btnDiag", "Save Diagnostics (.txt)###btnDiag"), ImVec2(halfW, 25.0f))) {
                        WriteDiagnosticFile();
                    }
                }
            }
        }
        ImGui::End();

        if (!open) {
            g_menuVisible = false;
            ClearSelectionWithRestore();
            g_state = ModState::Selecting;
        }
    }
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

// SEH Crash-Catch wrapper around ImGui rendering (prevents crashes on Alt+Tab / DeviceLost)
static void RenderDearImGuiFrameSeh(IDirect3DDevice9* pDevice) {
    __try {
        RenderDearImGuiFrame(pDevice);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastRebuildException = GetExceptionCode();
        g_imguiInitialized = false;
        g_imguiDevice = nullptr;
    }
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
    g_deviceResetInProgress = true;
    if (g_imguiInitialized) {
        __try { ImGui_ImplDX9_InvalidateDeviceObjects(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_imguiInitialized = false; }
    }
    const HookedD3DVTableEntry* entry = FindHookedVTableEntryForDevice(pDevice);
    HRESULT hr = (entry && entry->origReset) ? entry->origReset(pDevice, pPresentationParameters) : D3D_OK;
    if (SUCCEEDED(hr) && g_imguiInitialized) {
        __try { ImGui_ImplDX9_CreateDeviceObjects(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_imguiInitialized = false; }
    }
    g_deviceResetInProgress = false;
    return hr;
}

static HRESULT SafeTestCooperativeLevel(IDirect3DDevice9* pDevice) {
    __try {
        return pDevice->TestCooperativeLevel();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return D3DERR_DEVICELOST;
    }
}

static HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9* pDevice) {
    ++g_endSceneCallCount;
    const HookedD3DVTableEntry* entry = FindHookedVTableEntryForDevice(pDevice);
    FnD3D9EndScene origEndScene = entry ? entry->origEndScene : nullptr;

    if (pDevice && !g_deviceResetInProgress) {
        if (SafeTestCooperativeLevel(pDevice) == D3D_OK) {
            bool doRebuild = false;
            {
                ScopedStateLock lock;
                bool inEditor = g_editorOverlayActive && (g_ctx.pEditorCat != 0 && g_ctx.pChallenge != 0 && g_ctx.pCursor != 0);
                if (inEditor && g_rebuildPending) doRebuild = true;
            }
            if (doRebuild) ExecuteEditorSceneRebuildNow();

            ScopedStateLock lock;
            bool inEditor = g_editorOverlayActive && (g_ctx.pEditorCat != 0 && g_ctx.pChallenge != 0 && g_ctx.pCursor != 0);
            if (inEditor) {
                RenderDearImGuiFrameSeh(pDevice);
            }
        }
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

static bool IsEditorObjectsValid() {
    return (IsAnyHeapObject(g_ctx.pEditorCat) && IsAnyHeapObject(g_ctx.pCursor) &&
        IsAnyHeapObject(g_ctx.pChallenge) && g_ctx.pBlocksArray != 0 && g_ctx.blockCount > 0);
}

static bool IsCursorAttachedToScene() {
    if (!IsAnyHeapObject(g_ctx.pCursor)) return false;
    uint32_t curMob = 0, curScene = 0;
    if (SafeReadU32(g_ctx.pCursor + 0x40, &curMob) && IsAnyHeapObject(curMob)) {
        return (SafeReadU32(curMob + 0x14, &curScene) && IsAnyHeapObject(curScene));
    }
    return false;
}

static bool IsActivelyInTrackEditor() {
    if (!IsEditorObjectsValid()) return false;
    if (g_menuVisible) return true;
    return IsCursorAttachedToScene();
}

static void UpdateStatusBanner() {
    static ULONGLONG lastTick = 0;
    static uintptr_t lastChallengePtr = 0;
    static int       s_detachedTicks = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastTick < 200) return;
    lastTick = now;

    ScopedStateLock lock;
    bool mapOk = RefreshEditorContext();
    EnsureGameWindowSubclassed();
    EnsureD3D9OverlayHooked();

    if (!mapOk || !IsEditorObjectsValid()) {
        g_editorOverlayActive = false;
        g_selectedBlocks.clear();
        g_dirtyBlocks.clear();
        g_clonedSignOverrides.clear();
        g_hasBoxCorner1 = false;
        g_lastMovedBlockAddr = 0;
        g_menuVisible = false;
        lastChallengePtr = 0;
        s_detachedTicks = 0;
        return;
    }

    // When outside the track editor (normal gameplay / test-drive), the 3D cursor is detached from CScene3d
    if (!IsCursorAttachedToScene()) {
        ++s_detachedTicks;
        if (!g_menuVisible && s_detachedTicks >= 2) {
            g_editorOverlayActive = false;
        }
        if (s_detachedTicks >= 4) {
            g_editorOverlayActive = false;
            g_selectedBlocks.clear();
            g_hasBoxCorner1 = false;
            g_menuVisible = false;
        }
    }
    else {
        s_detachedTicks = 0;
        g_editorOverlayActive = true;
    }

    if (g_ctx.pChallenge != lastChallengePtr) {
        g_selectedBlocks.clear();
        g_dirtyBlocks.clear();
        g_clonedSignOverrides.clear();
        g_hasBoxCorner1 = false;
        g_unitLayout = BlockUnitLayoutInfo{};
        g_lastMovedBlockAddr = 0;
        lastChallengePtr = g_ctx.pChallenge;

        bool hasUnlimiterBlocks = false;
        for (uint32_t i = 0; i < g_ctx.blockCount; ++i) {
            uint32_t b = 0;
            if (!SafeReadU32(g_ctx.pBlocksArray + i * 4, &b) || !IsAnyHeapObject(b)) continue;
            Int3 pos = ReadBlockCoord(b);
            if (pos.x > 31 || pos.y > 31 || pos.z > 31) {
                hasUnlimiterBlocks = true;
                break;
            }
        }
        g_cfg.ruleMode = hasUnlimiterBlocks
            ? static_cast<int>(BuildRuleMode::TMUnlimiter)
            : static_cast<int>(BuildRuleMode::Vanilla);
    }
}

// ============================================================================
// 7. Main Thread & DLL Entry Point (with SEH Crash Protection)
// ============================================================================

static void RunModMainLoop(HMODULE hMod) {
    InitPaths(hMod);
    LoadConfig();

    while (g_running) {
        if (g_bindingTarget) {
            for (int vk = 8; vk < 255; ++vk) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;
                if (vk >= VK_F5 && vk <= VK_F7) continue;
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

        if (g_needVt31Rebuild && g_deferredVt31Tick != 0 && GetTickCount64() >= g_deferredVt31Tick) {
            g_deferredVt31Tick = 0;
            RequestLiveEditorRebuild();
        }
        if (!IsTrackManiaProcessFocused() || !IsActivelyInTrackEditor() || IsTypingInBlueprintBox()) {
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
            if (WasKeyPressed(g_cfg.keyStepSizeCycle))    CycleStepMultiplier(false);

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
}

static void ResetModStateAfterException(uint32_t exCode) {
    g_lastRebuildException = exCode;
    g_selectedBlocks.clear();
    g_dirtyBlocks.clear();
    g_hasBoxCorner1 = false;
    Sleep(250);
}

static void RunModMainLoopSeh(HMODULE hMod) {
    __try {
        RunModMainLoop(hMod);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ResetModStateAfterException(GetExceptionCode());
    }
}

DWORD WINAPI ModThread(LPVOID lpParam) {
    HMODULE hMod = reinterpret_cast<HMODULE>(lpParam);
    while (g_running) {
        RunModMainLoopSeh(hMod);
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