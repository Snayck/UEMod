#pragma once

#include "Unreal/UnrealTypes.h"

/*
 * Off:: holds every struct-member offset the reflection layer needs.
 *
 * Values here are live globals. They start at sensible modern-UE (4.25+ x64)
 * defaults, then OffsetFinder::Run() overwrites the ones it can detect at load
 * time. A consumer can also override any of them before UE::Initialize() via the
 * UEHookConfig struct (see UEHook.h) for games where detection fails.
 */
namespace Off
{
    // struct FUObjectItem layout inside the GObjects array
    namespace FUObjectItem
    {
        inline int32 Size = 0x18; // stride between items; 0x18 on most UE4/5, sometimes 0x10
    }

    namespace UObject
    {
        inline int32 Vft    = 0x00; // vtable pointer (always 0)
        inline int32 Flags  = 0x08; // EObjectFlags (int32)
        inline int32 Index  = 0x0C; // InternalIndex (int32) - index into GObjects
        inline int32 Class  = 0x10; // ClassPrivate (UClass*)
        inline int32 Name   = 0x18; // FName (NamePrivate)
        inline int32 Outer  = 0x20; // OuterPrivate (UObject*)
    }

    namespace UField
    {
        inline int32 Next   = 0x28; // UField* Next
    }

    namespace UStruct
    {
        inline int32 SuperStruct     = 0x40; // UStruct*
        inline int32 Children        = 0x48; // UField*  (functions + pre-4.25 properties)
        inline int32 ChildProperties = 0x50; // FField*  (properties, 4.25+); 0 if not present
        inline int32 Size            = 0x58; // PropertiesSize (int32)
        inline int32 MinAlignment    = 0x5C; // (int32)
    }

    namespace UFunction
    {
        inline int32 FunctionFlags = 0xB0; // EFunctionFlags (uint32)
        inline int32 NumParms      = 0xB8; // (uint8)
        inline int32 ParmsSize     = 0xBA; // (uint16)
        inline int32 ExecFunction  = 0xD8; // native func ptr (void*)
    }

    namespace UClass
    {
        inline int32 CastFlags           = 0x88; // EClassCastFlags (uint64)
        inline int32 ClassDefaultObject  = 0x118; // UObject* (CDO)
    }

    // Layout of properties. Two worlds:
    //   bUseFProperty == true  -> FProperty (FField-based), lives in UStruct::ChildProperties chain
    //   bUseFProperty == false -> UProperty (UField-based),  lives in UStruct::Children chain
    // The member offsets below are relative to the property object in whichever world is active.
    namespace Property
    {
        inline int32 ArrayDim      = 0x34; // int32
        inline int32 ElementSize   = 0x38; // int32
        inline int32 PropertyFlags = 0x40; // EPropertyFlags (uint64)
        inline int32 Offset        = 0x4C; // Offset_Internal (int32) - byte offset into owning object
    }

    // FField (base of FProperty in 4.25+)
    namespace FField
    {
        inline int32 Class = 0x08; // FFieldClass*
        inline int32 Owner = 0x10; // FFieldVariant (container: class, struct, or owning property)
        inline int32 Next  = 0x20; // FField*
        inline int32 Name  = 0x28; // FName
        inline int32 Flags = 0x30; // EObjectFlags
    }

    // UEnum (UField-derived). Names is a TArray<TPair<FName, int64>> of the
    // enumerators. Default is the common UE4.25+/UE5 layout; resolved lazily from
    // a real enum object on first use (see UEEnum::GetNames).
    namespace UEnum
    {
        inline int32 Names = 0x40; // TArray<TPair<FName,int64>>
    }

    namespace FBoolProperty
    {
        // == sizeof(FProperty): where FBoolProperty's 4 bytes (and every other
        // concrete property's extra members: StructProperty::Struct,
        // ArrayProperty::Inner, ...) begin. Shifted -0x08 in the compact layout.
        inline int32 Base = 0x78; // FieldSize/ByteOffset/ByteMask/FieldMask (4 uint8)
    }

    // Values resolved once, at init, that describe the running engine.
    namespace InSDK
    {
        inline bool  bUseFProperty = true;  // UE 4.25+ split UProperty -> FProperty
        inline bool  bIsChunked    = true;  // chunked FUObjectArray (4.21+/UE5) vs fixed

        namespace ProcessEvent
        {
            inline int32 Index    = 0;       // vtable index of UObject::ProcessEvent
            inline void* FuncPtr  = nullptr; // resolved address of ProcessEvent (hook target)
            inline void* Callable = nullptr; // address to invoke for manual calls;
                                             // hook layer points this at its trampoline,
                                             // otherwise it mirrors FuncPtr
        }

        namespace Name
        {
            inline bool bIsUsingFNamePool = true; // FNamePool (4.23+) vs TNameEntryArray
            inline int32 FNameSize        = 0x08; // sizeof(FName): 8 (index+number) or 4 (index only)
        }
    }
}
