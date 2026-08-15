#pragma once

#include "Unreal/UnrealTypes.h"

/*
 * Access to GObjects (the global FUObjectArray).
 *
 * Handles both layouts:
 *   - Fixed  (UE 4.11-4.20): Objects is a flat FUObjectItem array.
 *   - Chunked (UE 4.21+/UE5): Objects is an array of chunk pointers.
 *
 * Returns raw UObject* pointers; wrap them with UEObject to do anything useful.
 */
class ObjectArray
{
public:
    static bool Init(uintptr_t gobjects, bool bIsChunked);
    static bool IsInitialized() { return GObjects != 0; }

    static int32 Num();
    static void* GetByIndex(int32 index);

    // Linear iteration helper. Callback gets each non-null UObject*.
    template<typename Fn>
    static void ForEach(Fn&& callback)
    {
        const int32 count = Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* obj = GetByIndex(i);
            if (obj)
                callback(obj);
        }
    }

private:
    static inline uintptr_t GObjects = 0;
    static inline bool bChunked = true;

    static constexpr int32 kElementsPerChunk = 65536;
};
