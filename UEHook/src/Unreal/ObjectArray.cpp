#include "ObjectArray.h"
#include "../Core/SafeMemory.h"
#include "../Engine/Offsets.h"

bool ObjectArray::Init(uintptr_t gobjects, bool bIsChunked)
{
    if (!gobjects)
        return false;

    GObjects = gobjects;
    bChunked = bIsChunked;
    return Num() > 0;
}

int32 ObjectArray::Num()
{
    if (!GObjects)
        return 0;

    // NumElements sits at 0x14 (chunked) or 0xC (fixed).
    const uintptr_t numOff = bChunked ? 0x14 : 0x0C;
    int32 num = 0;
    if (!SafeMemory::Read<int32>(GObjects + numOff, &num))
        return 0;

    if (num <= 0 || num > 0x40000000)
        return 0;

    return num;
}

void* ObjectArray::GetByIndex(int32 index)
{
    if (!GObjects || index < 0)
        return nullptr;

    const int32 itemSize = Off::FUObjectItem::Size;
    uintptr_t itemAddr = 0;

    if (bChunked)
    {
        const int32 chunkIndex  = index / kElementsPerChunk;
        const int32 withinChunk = index % kElementsPerChunk;

        uintptr_t chunksArray = 0; // Objects*** at offset 0
        if (!SafeMemory::Read<uintptr_t>(GObjects, &chunksArray) || !chunksArray)
            return nullptr;

        uintptr_t chunk = 0;
        if (!SafeMemory::Read<uintptr_t>(chunksArray + chunkIndex * sizeof(void*), &chunk) || !chunk)
            return nullptr;

        itemAddr = chunk + static_cast<uintptr_t>(withinChunk) * itemSize;
    }
    else
    {
        uintptr_t items = 0; // Objects* at offset 0
        if (!SafeMemory::Read<uintptr_t>(GObjects, &items) || !items)
            return nullptr;

        itemAddr = items + static_cast<uintptr_t>(index) * itemSize;
    }

    // FUObjectItem begins with the UObject* itself.
    uintptr_t obj = 0;
    if (!SafeMemory::Read<uintptr_t>(itemAddr, &obj) || !SafeMemory::IsReasonable(obj))
        return nullptr;

    return reinterpret_cast<void*>(obj);
}
