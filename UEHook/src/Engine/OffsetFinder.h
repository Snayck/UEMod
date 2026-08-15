#pragma once

#include "Unreal/UnrealTypes.h"

/*
 * Derives Off:: values at runtime from live objects.
 *
 * Requires GObjects + GNames to already be located (UEGlobalsDetector) and the
 * chunked/pool flags set. Each detector validates its own result and leaves the
 * compiled-in default untouched on failure, so a bad detection never regresses
 * below the defaults. Anything a consumer overrides via UEHookConfig is honored
 * and skipped here.
 */
namespace OffsetFinder
{
    // When true, detectors print step-by-step progress to stdout.
    inline bool Verbose = false;

    struct Result
    {
        bool ItemSize      = false;
        bool ClassOffset   = false;
        bool NameOffset    = false;
        bool OuterOffset   = false;
        bool SuperOffset   = false;
        bool ChildrenOffset= false;
        bool PropChain     = false; // ChildProperties + bUseFProperty
        bool ProcessEvent  = false;
    };

    // Runs all detectors. ObjectArray + NameArray must be initialized first.
    // Consumer overrides should be applied AFTER this so they always win.
    Result Run(uintptr_t gObjectsAddr, bool bIsChunked);

    // Signature-scan the main module for UObject::ProcessEvent. Sets
    // Off::InSDK::ProcessEvent::FuncPtr on success.
    bool DetectProcessEvent();
}
