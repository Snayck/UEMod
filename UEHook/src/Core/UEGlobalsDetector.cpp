#include "UEGlobalsDetector.h"
#include "MemoryScanner.h"
#include <Windows.h>
#include <cstring>
#include <iostream>
#include <vector>

UEGlobalsDetector::UEGlobalsDetector() : m_scanner(std::make_unique<MemoryScanner>()) {}

UEGlobalsDetector::~UEGlobalsDetector() = default;

namespace { enum { PAT_GOBJECTS = 0, PAT_FNAMES, PAT_GWORLD }; }

static uintptr_t ResolveDisp(uintptr_t at, int len, int extra)
{
    uintptr_t dispAddr = at + len - 4;
    return dispAddr + *reinterpret_cast<const int32_t*>(dispAddr) + extra;
}

// Raw scan, no C++ objects (SEH-compatible).
// 1 = match at *out (cursor advanced), 0 = done, -1 = section faulted.
static int ScanNextMatch(const uint8_t* s, size_t sz,
                         const uint8_t* bytes, const char* mask, size_t n,
                         size_t anchor, uint8_t anchorVal,
                         size_t* cursor, const uint8_t** out)
{
    __try
    {
        if (n == 0 || n > sz) return 0;
        const size_t last = sz - n;
        const uint8_t* lim = s + last + anchor;
        for (;;)
        {
            const uint8_t* from = s + *cursor + anchor;
            if (from > lim) { *cursor = last + 1; return 0; }
            const uint8_t* hit = (const uint8_t*)memchr(from, anchorVal, (size_t)(lim - from + 1));
            if (!hit) { *cursor = last + 1; return 0; }
            *cursor = (size_t)(hit - s) + 1 - anchor;
            const uint8_t* cand = hit - anchor;
            bool ok = true;
            for (size_t i = 0; i < n; ++i)
                if (mask[i] && cand[i] != bytes[i]) { ok = false; break; }
            if (ok) { *out = cand; return 1; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

bool UEGlobalsDetector::FindUEGlobals() {
    const DWORD t0 = GetTickCount();

    struct PatDef { const char* str; int type; int resolveOffset; };
    static const PatDef defs[] = {
        {"48 8B 05 ? ? ? ? 48 8B 0C C8 48 8D 04 D1", PAT_GOBJECTS, 7},
        {"48 8B 05 ? ? ? ? 48 8B 0C C8 48 8D 14 D1", PAT_GOBJECTS, 7},
        {"48 8B 05 ? ? ? ? 48 8B 14 C8",             PAT_GOBJECTS, 7},
        {"48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05 ? ? ? ? ?", PAT_GOBJECTS, 7},
        {"4C 8B 15 ? ? ? ? 8B 5D 10",                PAT_GOBJECTS, 7},
        {"4C 8B 15 ? ? ? ? 44 8B 4D",                PAT_GOBJECTS, 7},
        {"4C 8B 0D ? ? ? ? 4C 8B C3",                PAT_GOBJECTS, 7},
        {"4C 8B 0D ? ? ? ? 48 8B 0C C8",             PAT_GOBJECTS, 7},
        {"4C 8B 0D ? ? ? ? 8B 41 0C",                PAT_GOBJECTS, 7},
        {"4C 8B 0D ? ? ? ? 4C 8B D3",                PAT_GOBJECTS, 7},
        {"4C 8B 0D ? ? ? ? 48 8B D1",                PAT_GOBJECTS, 7},
        {"4C 8B 0D ? ? ? ? 4D 85 C9",                PAT_GOBJECTS, 7},
        {"48 8B 0D ? ? ? ? 48 85 C9 74",             PAT_GOBJECTS, 7},
        {"4C 8B 05 ? ? ? ? 4C 8B C3",                PAT_GOBJECTS, 7},
        {"4C 8B 05 ? ? ? ? 4D 85 C0",                PAT_GOBJECTS, 7},
        {"48 89 05 ? ? ? ? 48 85 C0 74",             PAT_GOBJECTS, 7},
        {"4C 8B 15 ? ? ? ? 4D 85 D2",                PAT_GOBJECTS, 7},
        {"4C 8B 15 ? ? ? ? 4C 8B CA",                PAT_GOBJECTS, 7},
        {"4C 8B 0D ? ? ? ? 49 8B 41",                PAT_GOBJECTS, 7},
        {"4C 8B 0D ? ? ? ? 4D 8B 41",                PAT_GOBJECTS, 7},
        {"48 89 5C 24 ? 57 48 83 EC 20 48 8B D9 48 85 C9 75 ? 48 8B 0D", PAT_FNAMES, 11},
        {"48 8B 05 ? ? ? ? 48 85 C0 75 ? B9",        PAT_FNAMES, 7},
        {"48 8D 35 ? ? ? ? EB ? 48 8D 35",           PAT_FNAMES, 7},
        {"48 89 1D ? ? ? ? 48 8B 5C 24 20 48 83 C4 28", PAT_FNAMES, 7},
        {"48 89 1D ? ? ? ? 48 8B 5C 24 ? 48 83 C4",  PAT_FNAMES, 7},
        {"48 89 1D ? ? ? ? 48 8B 74 24 ?",           PAT_FNAMES, 7},
        {"48 89 1D ? ? ? ? EB",                      PAT_FNAMES, 7},
        {"48 8B 1D ? ? ? ? 48 85 DB 75",             PAT_FNAMES, 7},
        {"48 89 35 ? ? ? ? 48 8B 5C 24 ?",           PAT_FNAMES, 7},
        {"48 89 3D ? ? ? ? 48 8B 5C 24 ?",           PAT_FNAMES, 7},
        {"48 8D 05 ? ? ? ? 48 89 1D ? ? ? ?",        PAT_FNAMES, 7},
        {"48 89 1D ? ? ? ? E8 ? ? ? ?",              PAT_FNAMES, 7},
        {"48 8D 1D ? ? ? ? 48 89 1D ? ? ? ?",        PAT_FNAMES, 7},
        {"48 89 1D ? ? ? ? 48 83 C4 28 C3",          PAT_FNAMES, 7},
        {"48 8B 1D ? ? ? ? 48 85 DB 0F 85",          PAT_FNAMES, 7},
        {"48 89 05 ? ? ? ? 48 8B 4F 78",             PAT_GWORLD, 7},
        {"48 89 1D ? ? ? ? E8 ? ? ? ? 48 8B D8",     PAT_GWORLD, 7},
        {"48 8B 1D ? ? ? ? 48 85 DB 74",             PAT_GWORLD, 7},
    };

    struct Compiled {
        std::vector<uint8_t> bytes;
        std::vector<char>    mask;   // flat, not vector<bool>
        int   type;
        int   resolveOffset;
        size_t   anchor;
        uint8_t  anchorVal;
    };
    std::vector<Compiled> pats;
    for (const PatDef& d : defs)
    {
        Compiled c;
        c.type = d.type;
        c.resolveOffset = d.resolveOffset;
        const char* p = d.str;
        while (*p)
        {
            if (*p == '?') {
                c.bytes.push_back(0);
                c.mask.push_back(0);
                while (*p == '?') ++p;
            } else {
                c.bytes.push_back((uint8_t)strtoul(p, nullptr, 16));
                c.mask.push_back(1);
                while (*p && *p != ' ') ++p;
            }
            while (*p == ' ') ++p;
        }
        for (size_t i = 0; i < c.bytes.size(); ++i)
            if (c.mask[i]) { c.anchor = i; c.anchorVal = c.bytes[i]; break; }
        pats.push_back(std::move(c));
    }

    uintptr_t modBase = m_scanner->GetModuleBase();
    uintptr_t modSize = m_scanner->GetModuleSize();

    std::vector<std::pair<const uint8_t*, size_t>> sections;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(modBase);
    if (modBase && dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
        if (dos->e_lfanew > 0 && dos->e_lfanew < (LONG)modSize &&
            nt->Signature == IMAGE_NT_SIGNATURE)
        {
            auto sec = IMAGE_FIRST_SECTION(nt);
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            {
                if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                size_t sz = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
                if (sz < 16) continue;
                if (sec->VirtualAddress + sz > modSize) sz = modSize - sec->VirtualAddress;
                if (sz < 16) continue;
                sections.push_back({ reinterpret_cast<const uint8_t*>(modBase + sec->VirtualAddress), sz });
            }
        }
    }
    if (sections.empty() && modBase && modSize)
        sections.push_back({ reinterpret_cast<const uint8_t*>(modBase), modSize });

    bool foundGObjects = false, foundFNames = false, foundGWorld = false;

    auto typeFound = [&](int t) {
        return (t == PAT_GOBJECTS && foundGObjects) ||
               (t == PAT_FNAMES && foundFNames) ||
               (t == PAT_GWORLD && foundGWorld);
    };

    for (const auto& sec : sections)
    {
        if (foundGObjects && foundFNames && foundGWorld) break;
        const uint8_t* s = sec.first;
        const size_t  sz = sec.second;

        for (Compiled& p : pats)
        {
            if (typeFound(p.type)) continue;
            if (p.bytes.empty() || p.bytes.size() > sz) continue;

            size_t cursor = 0;
            for (;;)
            {
                const uint8_t* match = nullptr;
                int rc = ScanNextMatch(s, sz, p.bytes.data(), p.mask.data(), p.bytes.size(),
                                       p.anchor, p.anchorVal, &cursor, &match);
                if (rc <= 0) break;
                if (typeFound(p.type)) break;

                uintptr_t foundAddr = reinterpret_cast<uintptr_t>(match);
                if (p.type == PAT_FNAMES)
                {
                    for (int offset : { p.resolveOffset, 7, 3 })
                        for (int extra : { 4, -4 })
                        {
                            uintptr_t r = ResolveDisp(foundAddr, offset, extra);
                            if (ValidateFNames(r))
                            {
                                m_fNames = r;
                                foundFNames = true;
                                std::cout << "  FNames found at: 0x" << std::hex << r << std::dec << std::endl;
                                break;
                            }
                        }
                    if (foundFNames) break;
                }
                else if (p.type == PAT_GOBJECTS)
                {
                    for (int extra : { 4, -4 })
                    {
                        uintptr_t r = ResolveDisp(foundAddr, p.resolveOffset, extra);
                        if (ValidateGObjects(r))
                        {
                            m_gObjects = r;
                            foundGObjects = true;
                            std::cout << "  GObjects found at: 0x" << std::hex << r << std::dec << std::endl;
                            break;
                        }
                    }
                }
                else
                {
                    for (int extra : { 4, -4 })
                    {
                        uintptr_t r = ResolveDisp(foundAddr, p.resolveOffset, extra);
                        if (ValidateGWorld(r))
                        {
                            m_gWorld = r;
                            foundGWorld = true;
                            std::cout << "  GWorld found at: 0x" << std::hex << r << std::dec << std::endl;
                            break;
                        }
                    }
                }
            }
        }
    }

    std::cout << "Scan complete in " << (GetTickCount() - t0) << " ms" << std::endl;
    std::cout << "  GObjects: " << (foundGObjects ? "Found" : "Not Found") << std::endl;
    std::cout << "  FNames: " << (foundFNames ? "Found" : "Not Found") << std::endl;
    std::cout << "  GWorld: " << (foundGWorld ? "Found" : "Not Found") << std::endl;

    return foundGObjects && foundFNames && foundGWorld;
}

bool UEGlobalsDetector::ValidateGObjects(uintptr_t address) {
    if (!m_scanner->IsValidPointer(address)) {
        return false;
    }

    __try {
        // Try Fixed Array Layout (UE4.11-4.20)
        // Layout: [Objects*, MaxElements, NumElements]
        {
            void** objects = *reinterpret_cast<void***>(address + 0x0);
            int32_t maxElements = *reinterpret_cast<int32_t*>(address + 0x8);
            int32_t numElements = *reinterpret_cast<int32_t*>(address + 0xC);

            if (numElements >= 0x1000 && numElements < maxElements && maxElements < 0x400000) {
                if (m_scanner->IsValidPointer(reinterpret_cast<uintptr_t>(objects))) {
                    // Validate 5th object has InternalIndex == 5
                    struct FUObjectItem {
                        void* Object;
                        int32_t Flags;
                        int32_t ClusterIndex;
                        int32_t SerialNumber;
                    };

                    FUObjectItem* objectItems = reinterpret_cast<FUObjectItem*>(objects);
                    if (m_scanner->IsValidPointer(reinterpret_cast<uintptr_t>(objectItems[5].Object))) {
                        int32_t fifthObjectIndex = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(objectItems[5].Object) + 0xC);

                        if (fifthObjectIndex == 5) {
                            m_isChunkedArray = false;
                            return true;
                        }
                    }
                }
            }
        }

        // Try Chunked Array Layout (UE4.21+, UE5)
        // Layout: [Objects**, MaxElements, NumElements, MaxChunks, NumChunks]
        {
            void*** objects = *reinterpret_cast<void****>(address + 0x0);
            int32_t maxElements = *reinterpret_cast<int32_t*>(address + 0x10);
            int32_t numElements = *reinterpret_cast<int32_t*>(address + 0x14);
            int32_t maxChunks = *reinterpret_cast<int32_t*>(address + 0x18);
            int32_t numChunks = *reinterpret_cast<int32_t*>(address + 0x1C);

            if (numElements > 0x800 && maxElements > 0x10000 &&
                numChunks >= 0x1 && numChunks <= 0x14 &&
                maxChunks >= 0x6 && maxChunks <= 0x5FF) {

                if (m_scanner->IsValidPointer(reinterpret_cast<uintptr_t>(objects))) {
                    // Check if chunks are valid
                    bool chunksValid = true;
                    for (int i = 0; i < numChunks && i < 5; i++) {
                        if (!m_scanner->IsValidPointer(reinterpret_cast<uintptr_t>(objects[i]))) {
                            chunksValid = false;
                            break;
                        }
                    }

                    if (chunksValid) {
                        m_isChunkedArray = true;
                        return true;
                    }
                }
            }
        }

    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return false;
}

bool UEGlobalsDetector::ValidateFNames(uintptr_t address) {
    if (!m_scanner->IsValidPointer(address)) {
        return false;
    }

    void* first = nullptr;
    __try {
        first = *reinterpret_cast<void**>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return m_scanner->IsValidPointer(reinterpret_cast<uintptr_t>(first));
}

bool UEGlobalsDetector::ValidateGWorld(uintptr_t address) {
    if (!m_scanner->IsValidPointer(address)) {
        return false;
    }

    void* world = nullptr;
    __try {
        world = *reinterpret_cast<void**>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (world == nullptr) {
        return true; // GWorld can be null before world is loaded
    }

    return m_scanner->IsValidPointer(reinterpret_cast<uintptr_t>(world));
}


std::vector<uint8_t> UEGlobalsDetector::ParsePatternString(const char* patternStr) {
    std::vector<uint8_t> pattern;
    std::string str(patternStr);
    std::istringstream iss(str);
    std::string token;
    
    while (iss >> token) {
        if (token == "?") {
            pattern.push_back(0);
        } else {
            pattern.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
        }
    }
    
    return pattern;
}

std::vector<bool> UEGlobalsDetector::CreateMaskFromPattern(const char* patternStr) {
    std::vector<bool> mask;
    std::string str(patternStr);
    std::istringstream iss(str);
    std::string token;
    
    while (iss >> token) {
        mask.push_back(token != "?");
    }
    
    return mask;
}