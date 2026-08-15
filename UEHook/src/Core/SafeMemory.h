#pragma once

#include <cstdint>
#include <Windows.h>

// Centralized safe memory access.
//
// Hot reads use a cheap address range-check plus SEH (__try/__except) to survive
// bad pointers. They deliberately do NOT call VirtualQuery per read -- doing so
// on hundreds of thousands of objects turns detection into minutes. Use the
// explicit IsReadable() (which does query the page) only for one-off validation.
class SafeMemory
{
public:
    // Cheap: only checks the pointer is in the plausible user-mode range.
    static inline bool IsReasonable(uintptr_t address, size_t /*size*/ = sizeof(void*))
    {
        return address >= 0x10000 && address < 0x7FFFFFFEFFFF;
    }

    // Expensive: actually queries the page state. For init/validation, not loops.
    static bool IsReadable(uintptr_t address, size_t size = sizeof(void*))
    {
        if (!IsReasonable(address, size))
            return false;

        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        DWORD readableFlags = PAGE_READONLY | PAGE_READWRITE |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        return (mbi.Protect & readableFlags) != 0;
    }

    template<typename T>
    static bool Read(uintptr_t address, T* outValue)
    {
        if (!outValue || !IsReasonable(address, sizeof(T)))
            return false;

        __try
        {
            *outValue = *reinterpret_cast<T*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Null-terminated ANSI read, printable characters only.
    static bool ReadString(uintptr_t address, char* buffer, size_t maxLength)
    {
        if (!buffer || maxLength == 0 || !IsReasonable(address, 1))
            return false;

        size_t i = 0;
        __try
        {
            const char* src = reinterpret_cast<const char*>(address);
            for (; i < maxLength - 1; ++i)
            {
                char c = src[i];
                if (c == '\0') { buffer[i] = '\0'; return i > 0; }
                if (c >= 0x20 && c <= 0x7E) buffer[i] = c;
                else break;
            }
            buffer[i] = '\0';
            return i > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            buffer[i < maxLength ? i : 0] = '\0';
            return i > 0;
        }
    }

    static bool ReadWString(uintptr_t address, wchar_t* buffer, size_t maxLength)
    {
        if (!buffer || maxLength == 0 || !IsReasonable(address, sizeof(wchar_t)))
            return false;

        size_t i = 0;
        __try
        {
            const wchar_t* src = reinterpret_cast<const wchar_t*>(address);
            for (; i < maxLength - 1; ++i)
            {
                wchar_t c = src[i];
                if (c == L'\0') { buffer[i] = L'\0'; return i > 0; }
                buffer[i] = c;
            }
            buffer[i] = L'\0';
            return i > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            buffer[i < maxLength ? i : 0] = L'\0';
            return i > 0;
        }
    }
};
