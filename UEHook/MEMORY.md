# Project Memory Index

- [UEHook rebuild](../../.claude/projects/C--Users-Chris-Documents-GitHub-UEHook/memory/uehook-rebuild.md) — direction/design for turning UEHook into a proper UE4/5 reflection library (static lib, auto-detect offsets, rich C++ wrappers)
- [UEHook detection quirks](../../.claude/projects/C--Users-Chris-Documents-GitHub-UEHook/memory/uehook-detection-quirks.md) — ProcessEvent vtable detection + compact FFieldVariant (UE5.1.1+) 0x08 offset shift
- [UEHook end goal: node editor](../../.claude/projects/C--Users-Chris-Documents-GitHub-UEHook/memory/uehook-endgoal-node-editor.md) — library is the runtime engine for a future node-editor front-end; needs a dynamic/variant API surface
- [UEHook frontend architecture](../../.claude/projects/C--Users-Chris-Documents-GitHub-UEHook/memory/uehook-frontend-architecture.md) — same-process C++, own Win32+D3D11 window, ImGui+node-editor; game-thread dispatch required for calls/writes
