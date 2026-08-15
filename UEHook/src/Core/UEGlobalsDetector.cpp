#include "UEGlobalsDetector.h"
#include "MemoryScanner.h"
#include <Windows.h>
#include <iostream>

UEGlobalsDetector::UEGlobalsDetector() : m_scanner(std::make_unique<MemoryScanner>()) {}

UEGlobalsDetector::~UEGlobalsDetector() = default;

bool UEGlobalsDetector::FindUEGlobals() {
    std::cout << "Starting optimized UE global scan..." << std::endl;

    // All patterns we're looking for
    struct PatternInfo {
        const char* patternStr;
        std::vector<uint8_t> pattern;
        std::vector<bool> mask;
        enum Type { GOBJECTS, FNAMES, GWORLD } type;
        int resolveOffset;
    };

    std::vector<PatternInfo> patterns;

    // GObjects patterns
    patterns.push_back({"48 8B 05 ? ? ? ? 48 8B 0C C8 48 8D 04 D1", {}, {}, PatternInfo::GOBJECTS, 7});
    patterns.push_back({"48 8B 05 ? ? ? ? 48 8B 0C C8 48 8D 14 D1", {}, {}, PatternInfo::GOBJECTS, 7});
    patterns.push_back({"48 8B 05 ? ? ? ? 48 8B 14 C8", {}, {}, PatternInfo::GOBJECTS, 7});
    patterns.push_back({"48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05 ? ? ? ? ?", {}, {}, PatternInfo::GOBJECTS, 7});

    // FNames patterns (some need multiple offset attempts)
    // For the first pattern, we'll try offset 11 first (primary), then fallback to 7 and 3
    patterns.push_back({"48 89 5C 24 ? 57 48 83 EC 20 48 8B D9 48 85 C9 75 ? 48 8B 0D", {}, {}, PatternInfo::FNAMES, 11});
    patterns.push_back({"48 8B 05 ? ? ? ? 48 85 C0 75 ? B9", {}, {}, PatternInfo::FNAMES, 7});
    patterns.push_back({"48 8D 35 ? ? ? ? EB ? 48 8D 35", {}, {}, PatternInfo::FNAMES, 7});

    // GWorld patterns
    patterns.push_back({"48 89 05 ? ? ? ? 48 8B 4F 78", {}, {}, PatternInfo::GWORLD, 7});
    patterns.push_back({"48 89 1D ? ? ? ? E8 ? ? ? ? 48 8B D8", {}, {}, PatternInfo::GWORLD, 7});
    patterns.push_back({"48 8B 1D ? ? ? ? 48 85 DB 74", {}, {}, PatternInfo::GWORLD, 7});

    // Parse all patterns
    for (auto& p : patterns) {
        p.pattern = ParsePatternString(p.patternStr);
        p.mask = CreateMaskFromPattern(p.patternStr);
    }

    // Single memory scan checking all patterns
    const uint8_t* start = reinterpret_cast<const uint8_t*>(m_scanner->GetModuleBase());
    const uint8_t* end = start + m_scanner->GetModuleSize();

    size_t maxPatternSize = 0;
    for (const auto& p : patterns) {
        maxPatternSize = std::max(maxPatternSize, p.pattern.size());
    }

    bool foundGObjects = false;
    bool foundFNames = false;
    bool foundGWorld = false;

    // Scan memory once, checking all patterns at each position
    for (const uint8_t* current = start; current <= end - maxPatternSize; ++current) {
        // Early exit if we found everything
        if (foundGObjects && foundFNames && foundGWorld) {
            break;
        }

        // Check each pattern at this position
        for (const auto& p : patterns) {
            // Skip if we already found this type
            if ((p.type == PatternInfo::GOBJECTS && foundGObjects) ||
                (p.type == PatternInfo::FNAMES && foundFNames) ||
                (p.type == PatternInfo::GWORLD && foundGWorld)) {
                continue;
            }

            // Check if pattern matches
            bool matches = true;
            for (size_t i = 0; i < p.pattern.size(); ++i) {
                if (p.mask[i] && current[i] != p.pattern[i]) {
                    matches = false;
                    break;
                }
            }

            if (!matches) continue;

            // Pattern matched! Resolve and validate
            uintptr_t foundAddr = reinterpret_cast<uintptr_t>(current);
            uintptr_t resolved = m_scanner->ResolveRelativeAddress(foundAddr, p.resolveOffset);

            if (p.type == PatternInfo::GOBJECTS && !foundGObjects) {
                if (ValidateGObjects(resolved)) {
                    m_gObjects = resolved;
                    foundGObjects = true;
                    std::cout << "  GObjects found at: 0x" << std::hex << resolved << std::dec << std::endl;
                }
            } else if (p.type == PatternInfo::FNAMES && !foundFNames) {
                // FNames may need multiple offset attempts
                std::vector<int> offsets = {p.resolveOffset, 7, 3};
                for (int offset : offsets) {
                    uintptr_t fnamesResolved = m_scanner->ResolveRelativeAddress(foundAddr, offset);
                    if (ValidateFNames(fnamesResolved)) {
                        m_fNames = fnamesResolved;
                        foundFNames = true;
                        std::cout << "  FNames found at: 0x" << std::hex << fnamesResolved << std::dec << std::endl;
                        break;
                    }
                }
            } else if (p.type == PatternInfo::GWORLD && !foundGWorld) {
                if (ValidateGWorld(resolved)) {
                    m_gWorld = resolved;
                    foundGWorld = true;
                    std::cout << "  GWorld found at: 0x" << std::hex << resolved << std::dec << std::endl;
                }
            }
        }
    }

    std::cout << "Scan complete." << std::endl;
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

    // Basic validation for FNames array
    void** fnameArray = reinterpret_cast<void**>(address);
    return m_scanner->IsValidPointer(reinterpret_cast<uintptr_t>(*fnameArray));
}

bool UEGlobalsDetector::ValidateGWorld(uintptr_t address) {
    if (!m_scanner->IsValidPointer(address)) {
        return false;
    }

    void** gworld = reinterpret_cast<void**>(address);
    if (*gworld == nullptr) {
        return true; // GWorld can be null before world is loaded
    }

    return m_scanner->IsValidPointer(reinterpret_cast<uintptr_t>(*gworld));
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