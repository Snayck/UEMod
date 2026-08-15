#pragma once

#include <vector>
#include <string>
#include <sstream>

class MemoryScanner {
public:
    MemoryScanner();
    
    // Find a single pattern
    uintptr_t FindPattern(const std::vector<uint8_t>& pattern, const std::vector<bool>& mask) const;
    uintptr_t FindPatternString(const char* patternStr) const;
    
    // Find all instances of a pattern
    std::vector<uintptr_t> FindAllPatterns(const std::vector<uint8_t>& pattern, const std::vector<bool>& mask) const;
    
    // Resolve relative addresses (for x64 RIP-relative addressing)
    uintptr_t ResolveRelativeAddress(uintptr_t instructionAddr, int instructionLength) const;
    
    // Utility functions
    bool IsValidPointer(uintptr_t address) const;
    uintptr_t GetModuleBase() const { return m_moduleBase; }
    size_t GetModuleSize() const { return m_moduleSize; }

private:
    void InitializeModuleInfo();
    
    uintptr_t m_moduleBase = 0;
    size_t m_moduleSize = 0;
};