#include "NameArray.h"
#include "ObjectArray.h"
#include "../Core/SafeMemory.h"
#include "../Engine/Offsets.h"
#include <cstring>

bool NameArray::Init(uintptr_t gnames, bool bIsFNamePool)
{
    GNames = gnames;
    bIsPool = bIsFNamePool;
    return GNames != 0;
}

// ---- FNamePool ---------------------------------------------------------------

bool NameArray::InitPool(uintptr_t base)
{
    if (!SafeMemory::IsReasonable(base))
        return false;

    uintptr_t block0 = 0;
    if (!SafeMemory::Read<uintptr_t>(base + 0x10, &block0) || !SafeMemory::IsReasonable(block0))
        return false;

    constexpr uint32 NoneAsU32   = 0x656E6F4E;         // "None"
    constexpr uint64 CoreUObjU64 = 0x6A624F5565726F43; // "/Script/CoreUObject"
    constexpr uint32 ByteAsU32   = 0x65747942;         // "Byte" of "ByteProperty"

    uint8 win[0x2008];
    if (!SafeMemory::ReadBuffer(block0, win, sizeof win))
        return false;

    int32 headerSize = -1;
    bool  confirmed  = false;
    for (int i = 0; i + 8 <= 0x2000; ++i)
    {
        uint32 v4; memcpy(&v4, win + i, 4);
        if (v4 == NoneAsU32 && headerSize < 0)
            headerSize = i;

        uint64 v8; memcpy(&v8, win + i, 8);
        if (v8 == CoreUObjU64)
        {
            confirmed = true;
            break;
        }
    }
    if (headerSize < 0 || headerSize > 0x40 || !confirmed)
        return false;

    StringOffset = headerSize;
    Stride       = (headerSize == 2) ? 2 : 4;
    HeaderOffset = (headerSize == 6) ? 4 : 0;

    uintptr_t bpEntryOff = StringOffset + 4;
    for (int pad = 0; pad < 4; ++pad)
    {
        if (bpEntryOff + StringOffset + 4 > sizeof win) return false;
        uint32 v; memcpy(&v, win + bpEntryOff + StringOffset, 4);
        if (v == ByteAsU32)
            break;
        bpEntryOff += 1;
    }
    if (bpEntryOff + StringOffset + 4 > sizeof win) return false;

    uint16 hdr = 0;
    if (bpEntryOff + HeaderOffset + sizeof hdr > sizeof win) return false;
    memcpy(&hdr, win + bpEntryOff + HeaderOffset, sizeof hdr);
    int32 shift = 0;
    while (hdr != 0x0C && shift < 16) { ++shift; hdr >>= 1; }
    ShiftCount = (shift < 16) ? shift : 6;

    ChunksStart = 0x10;
    bIsPool     = true;
    GNames      = base;
    return true;
}

// ---- legacy TNameEntryArray --------------------------------------------------

bool NameArray::InitLegacy(uintptr_t base)
{
    if (!SafeMemory::IsReasonable(base))
        return false;

    // base is the array object; inline chunk table at base+0: chunk0 = *(base).
    uintptr_t chunk0 = 0, entry0 = 0;
    if (!SafeMemory::Read<uintptr_t>(base, &chunk0) || !SafeMemory::IsReasonable(chunk0))
        return false;
    if (!SafeMemory::Read<uintptr_t>(chunk0, &entry0) || !SafeMemory::IsReasonable(entry0))
        return false;

    // Entry 0 must be "None". Scan for it to find the string offset.
    constexpr uint32 NoneAsU32 = 0x656E6F4E;
    int32 strOff = -1;
    for (int i = 0; i < 0x20; ++i)
    {
        uint32 v = 0;
        if (SafeMemory::Read<uint32>(entry0 + i, &v) && v == NoneAsU32) { strOff = i; break; }
    }
    if (strOff < 0)
        return false;

    LegacyStringOffset = strOff;
    LegacyIndexOffset  = 0x0C; // typical on x64; index low bit is the wide flag
    bIsPool = false;
    GNames  = base;
    return true;
}

// ---- block-offset-bits refinement -------------------------------------------

void NameArray::DetermineBlockBits()
{
    if (!bIsPool)
        return;

    const int32 count = ObjectArray::Num();
    const int32 sampleMax = count < 600 ? count : 600;

    auto scoreForBits = [&](int32 bits) -> int
    {
        BlockOffsetBits = bits;
        int score = 0;
        for (int32 i = 0; i < sampleMax; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            int32 idx = 0;
            SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(o) + Off::UObject::Name, &idx);
            std::string s = GetNameByIndex(idx);
            if (s.empty() || s.size() > 128) continue;
            bool clean = true;
            for (char c : s)
                if (!(c == '_' || c == '/' || (c >= '0' && c <= '9') ||
                      (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) { clean = false; break; }
            if (clean) ++score;
        }
        return score;
    };

    int32 bestBits = 0xE;
    int bestScore = -1;
    for (int32 bits : { 0xE, 0xF, 0x10, 0x11, 0x12, 0x13 })
    {
        int s = scoreForBits(bits);
        if (s > bestScore) { bestScore = s; bestBits = bits; }
    }
    BlockOffsetBits = bestBits;
}

// ---- lookup ------------------------------------------------------------------

const uint8_t* NameArray::GetEntryPtr(int32 comparisonIndex)
{
    if (!GNames || comparisonIndex < 0)
        return nullptr;

    if (bIsPool)
    {
        const uint32 chunkIdx = static_cast<uint32>(comparisonIndex) >> BlockOffsetBits;
        const uint32 inChunk  = (static_cast<uint32>(comparisonIndex) & ((1u << BlockOffsetBits) - 1)) * Stride;

        uintptr_t blockPtr = 0;
        if (!SafeMemory::Read<uintptr_t>(GNames + ChunksStart + chunkIdx * sizeof(void*), &blockPtr) ||
            !SafeMemory::IsReasonable(blockPtr))
            return nullptr;

        uintptr_t entry = blockPtr + inChunk;
        return SafeMemory::IsReasonable(entry) ? reinterpret_cast<const uint8_t*>(entry) : nullptr;
    }

    // Legacy: entry = ((void***)base)[chunkIdx][inChunk]
    const int32 chunkIdx = comparisonIndex / 0x4000;
    const int32 inChunk  = comparisonIndex % 0x4000;

    uintptr_t chunk = 0;
    if (!SafeMemory::Read<uintptr_t>(GNames + chunkIdx * sizeof(void*), &chunk) || !chunk)
        return nullptr;
    uintptr_t entry = 0;
    if (!SafeMemory::Read<uintptr_t>(chunk + inChunk * sizeof(void*), &entry) || !SafeMemory::IsReasonable(entry))
        return nullptr;
    return reinterpret_cast<const uint8_t*>(entry);
}

std::string NameArray::DecodeEntry(const uint8_t* entry)
{
    if (!entry)
        return "";

    const uintptr_t addr = reinterpret_cast<uintptr_t>(entry);

    if (bIsPool)
    {
        uint16 header = 0;
        if (!SafeMemory::Read<uint16>(addr + HeaderOffset, &header))
            return "";

        const int32 len = header >> ShiftCount;
        const bool wide = (header & NameWideMask) != 0;
        if (len <= 0 || len > 1024)
            return "";

        const size_t bytes = wide ? (size_t)len * 2 : (size_t)len;
        std::string out;
        out.resize(wide ? (size_t)len * 2 : (size_t)len);
        if (!SafeMemory::ReadBuffer(addr + StringOffset, out.data(), bytes))
            return "";

        if (wide)
        {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(out.data());
            for (int i = 0; i < len; ++i)
                out[i] = (w[i] >= 0x20 && w[i] <= 0x7E) ? static_cast<char>(w[i]) : '?';
        }
        else
        {
            for (int i = 0; i < len; ++i)
                if (out[i] < 0x20 || out[i] > 0x7E)
                    out[i] = '?';
        }
        out.resize(len);
        return out;
    }

    // Legacy: null-terminated string at entry+StringOffset; wide flag in index.
    int32 nameIdx = 0;
    SafeMemory::Read<int32>(addr + LegacyIndexOffset, &nameIdx);
    char buf[512] = {};
    if (SafeMemory::ReadString(addr + LegacyStringOffset, buf, sizeof(buf)) && buf[0])
        return std::string(buf);
    return "";
}

std::string NameArray::GetNameByIndex(int32 comparisonIndex)
{
    return DecodeEntry(GetEntryPtr(comparisonIndex));
}

std::string NameArray::GetFullName(int32 comparisonIndex, int32 number)
{
    std::string base = GetNameByIndex(comparisonIndex);
    if (!base.empty() && number > 0)
        base += "_" + std::to_string(number - 1);
    return base;
}

int32 NameArray::FindIndex(const std::string& name)
{
    if (!GNames || !bIsPool || name.empty())
        return -1;

    // FNamePool header: {..., uint32 CurrentBlock@0x08, uint32 CurrentByteCursor@0x0C,
    // uint8* Blocks[]@ChunksStart}. Walk each block's entries in order, computing
    // each entry's comparison index from its byte offset, and match the string.
    uint32 currentBlock = 0, currentCursor = 0;
    SafeMemory::Read<uint32>(GNames + 0x08, &currentBlock);
    SafeMemory::Read<uint32>(GNames + 0x0C, &currentCursor);
    if (currentBlock > 0x100000) // sanity: header not where we expect
        return -1;

    const int32 blockSizeBytes = (1 << BlockOffsetBits) * Stride; // bytes per block

    for (uint32 block = 0; block <= currentBlock; ++block)
    {
        uintptr_t blockPtr = 0;
        if (!SafeMemory::Read<uintptr_t>(GNames + ChunksStart + block * sizeof(void*), &blockPtr) ||
            !SafeMemory::IsReasonable(blockPtr))
            continue;

        const uint32 limit = (block == currentBlock) ? currentCursor : (uint32)blockSizeBytes;

        uint32 offset = 0;
        int guard = 0;
        while (offset < limit && guard++ < 0x40000)
        {
            const uintptr_t entry = blockPtr + offset;

            uint16 header = 0;
            if (!SafeMemory::Read<uint16>(entry + HeaderOffset, &header))
                break;

            const int32 len  = header >> ShiftCount;
            const bool  wide = (header & NameWideMask) != 0;
            if (len <= 0) // reached unused tail of this block
                break;
            if (len > 1024)
                break;

            const int32 idx = (int32)((block << BlockOffsetBits) + offset / Stride);
            if (DecodeEntry(reinterpret_cast<const uint8_t*>(entry)) == name)
                return idx;

            const int32 bytes = StringOffset + (wide ? len * 2 : len);
            offset += (uint32)Align(bytes, Stride);
        }
    }
    return -1;
}
