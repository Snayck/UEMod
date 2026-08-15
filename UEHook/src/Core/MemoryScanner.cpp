#include "MemoryScanner.h"
#include <Windows.h>
#include <psapi.h>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iostream>

MemoryScanner::MemoryScanner() : m_moduleBase(0), m_moduleSize(0) {
    InitializeModuleInfo();
}

void MemoryScanner::InitializeModuleInfo() {
    HMODULE hModule = GetModuleHandle(nullptr);
    if (hModule) {
        MODULEINFO moduleInfo;
        if (K32GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo))) {
            m_moduleBase = reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll);
            m_moduleSize = moduleInfo.SizeOfImage;
        }
    }
}

uintptr_t MemoryScanner::FindPattern(const std::vector<uint8_t>& pattern, const std::vector<bool>& mask) const {
    if (pattern.size() != mask.size() || pattern.empty()) {
        return 0;
    }
    
    const uint8_t* start = reinterpret_cast<const uint8_t*>(m_moduleBase);
    const uint8_t* end = start + m_moduleSize - pattern.size();
    
    for (const uint8_t* current = start; current <= end; ++current) {
        bool found = true;
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (mask[i] && current[i] != pattern[i]) {
                found = false;
                break;
            }
        }
        if (found) {
            return reinterpret_cast<uintptr_t>(current);
        }
    }
    
    return 0;
}

uintptr_t MemoryScanner::FindPatternString(const char* patternStr) const {
    std::vector<uint8_t> pattern;
    std::vector<bool> mask;
    
    // Parse pattern string like "48 8B ? ? 48 89"
    std::string str(patternStr);
    std::istringstream iss(str);
    std::string token;
    
    while (iss >> token) {
        if (token == "?") {
            pattern.push_back(0);
            mask.push_back(false);
        } else {
            pattern.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
            mask.push_back(true);
        }
    }
    
    return FindPattern(pattern, mask);
}

std::vector<uintptr_t> MemoryScanner::FindAllPatterns(const std::vector<uint8_t>& pattern, const std::vector<bool>& mask) const {
    std::vector<uintptr_t> results;
    
    if (pattern.size() != mask.size() || pattern.empty()) {
        return results;
    }
    
    const uint8_t* start = reinterpret_cast<const uint8_t*>(m_moduleBase);
    const uint8_t* end = start + m_moduleSize - pattern.size();
    
    for (const uint8_t* current = start; current <= end; ++current) {
        bool found = true;
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (mask[i] && current[i] != pattern[i]) {
                found = false;
                break;
            }
        }
        if (found) {
            results.push_back(reinterpret_cast<uintptr_t>(current));
            current += pattern.size() - 1; // Skip ahead to avoid overlapping matches
        }
    }
    
    return results;
}

uintptr_t MemoryScanner::ResolveRelativeAddress(uintptr_t instructionAddr, int instructionLength) const {
    if (instructionAddr == 0) {
        return 0;
    }
    
    int32_t offset = *reinterpret_cast<int32_t*>(instructionAddr + instructionLength - 4);
    return instructionAddr + instructionLength + offset;
}

bool MemoryScanner::IsValidPointer(uintptr_t address) const {
    if (address < 0x10000 || address > 0x7FFFFFFEFFFF) {
        return false;
    }
    
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    
    return (mbi.State == MEM_COMMIT) && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}