#pragma once

#include "Unreal/UnrealTypes.h"
#include <string>

/*
 * Resolves FName comparison indices to strings, following Dumper-7's proven
 * FNamePool / TNameEntryArray algorithms.
 *
 * The pool parameters (entry stride, header shift, string offset, block-offset
 * bits) are all *derived* from the running game's memory rather than assumed,
 * because they vary across UE versions.
 */
class NameArray
{
public:
    // Thin entry point used before OffsetFinder runs; just records the base.
    static bool Init(uintptr_t gnames, bool bIsFNamePool);

    static bool IsInitialized() { return GNames != 0; }
    static void SetBase(uintptr_t gnames) { GNames = gnames; }
    static uintptr_t GetBase() { return GNames; }
    static bool UsingPool() { return bIsPool; }

    // Validate + configure an FNamePool located at `base` (blocks inline at
    // base+0x10). Confirms by locating "None" and "/Script/CoreUObject" in the
    // first block and derives stride/header-shift/string-offset. Returns false
    // if `base` is not actually an FNamePool.
    static bool InitPool(uintptr_t base);

    // Validate + configure a legacy TNameEntryArray at `base`.
    static bool InitLegacy(uintptr_t base);

    // Refine FNameBlockOffsetBits by scoring object-name decodes (needs the
    // object array + UObject::Name offset to be known).
    static void DetermineBlockBits();

    static std::string GetNameByIndex(int32 comparisonIndex);
    static std::string GetFullName(int32 comparisonIndex, int32 number);

    // Reverse lookup: scan the pool for an entry whose string equals `name` and
    // return its comparison index, or -1 if not present. Used to turn a string
    // into an FName for writes/calls without allocating a new name entry.
    // (FNamePool only; legacy TNameEntryArray returns -1.)
    static int32 FindIndex(const std::string& name);

private:
    static const uint8_t* GetEntryPtr(int32 comparisonIndex);
    static std::string DecodeEntry(const uint8_t* entry);

private:
    static inline uintptr_t GNames = 0;
    static inline bool bIsPool = true;

    // FNamePool parameters (all derived by InitPool).
    static inline int32 ChunksStart     = 0x10; // offset of inline block array in the pool
    static inline int32 Stride          = 2;    // bytes per FName entry-id unit (2 or 4)
    static inline int32 StringOffset    = 2;    // header size = offset from entry to string
    static inline int32 HeaderOffset    = 0;    // offset from entry to the length header
    static inline int32 ShiftCount      = 6;    // Len = header >> ShiftCount
    static inline int32 BlockOffsetBits = 0xE;  // index split: block = idx >> bits

    // Legacy TNameEntryArray parameters.
    static inline int32 LegacyIndexOffset  = 0x0C;
    static inline int32 LegacyStringOffset = 0x10;

    static constexpr int32 NameWideMask = 0x1;
};
