#pragma once

#include <memory>
#include <vector>
#include <string>
#include <sstream>

class MemoryScanner;

class UEGlobalsDetector {
public:
    UEGlobalsDetector();
    ~UEGlobalsDetector();

    bool FindUEGlobals();

    uintptr_t GetGObjects() const { return m_gObjects; }
    uintptr_t GetFNames() const { return m_fNames; }
    uintptr_t GetGWorld() const { return m_gWorld; }
    bool IsChunkedObjectArray() const { return m_isChunkedArray; }
    
private:
    bool ValidateGObjects(uintptr_t address);
    bool ValidateFNames(uintptr_t address);
    bool ValidateGWorld(uintptr_t address);

    std::vector<uint8_t> ParsePatternString(const char* patternStr);
    std::vector<bool> CreateMaskFromPattern(const char* patternStr);
    
    std::unique_ptr<MemoryScanner> m_scanner;
    uintptr_t m_gObjects = 0;
    uintptr_t m_fNames = 0;
    uintptr_t m_gWorld = 0;
    bool m_isChunkedArray = false;
};