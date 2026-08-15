# Universal UE Hook SDK

A universal Unreal Engine hooking SDK that works across UE3, UE4, and UE5, providing an easy-to-use interface for function hooking, property access, and object iteration.

## Features

- **Universal Compatibility**: Works with UE3, UE4, and UE5 games
- **Automatic Detection**: Finds GObjects, FNames, and GWorld automatically using memory patterns
- **Function Hooking**: Hook functions with Pre and Post callbacks
- **Property Access**: Get/Set object properties easily
- **Reflection System**: Find classes, functions, and properties by name
- **Pawn/Actor Iteration**: Efficient iteration over game objects
- **C and C++ API**: Easy integration with existing projects

## Quick Start

### 1. Build the SDK

```bash
mkdir build && cd build
cmake .. -DBUILD_EXAMPLES=ON
cmake --build . --config Release
```

### 2. Basic Usage (C++)

```cpp
#include "UniversalUEHook.h"

// Initialize SDK
UEHook::SDK sdk;
if (!sdk.IsReady()) {
    // Handle initialization failure
    return;
}

// Hook a function
HookFunctionPost("Canvas.DrawText", [](void* thisPtr, void* params, void* result) {
    // Your hook code here
    std::cout << "Canvas DrawText called!" << std::endl;
});

// Get player pawns
auto pawns = UEHook::GetPawns();
for (void* pawn : pawns) {
    float health = GetFloatProperty(pawn, "Health");
    std::cout << "Pawn health: " << health << std::endl;
}

// Access all objects in GObjects array
auto allObjects = UEHook::GetAllObjects();
std::cout << "Total objects: " << allObjects.size() << std::endl;

// Find specific object types
auto actors = UEHook::FindObjectsByClass("Actor");
auto components = UEHook::FindObjectsByClass("Component");

// Iterate all objects with callback
IterateAllObjects([](void* obj) {
    if (IsActor(obj)) {
        std::cout << "Found actor: " << obj << std::endl;
    }
});

// Find a specific function
UEHook::UEFunction drawText("DrawText");
if (drawText.IsValid()) {
    void* funcPtr = drawText.GetPtr();
    // Use function pointer
}
```

### 3. Basic Usage (C)

```c
#include "UniversalUEHook.h"

// Initialize
if (!InitializeUEHook()) {
    // Handle error
    return;
}

// Hook function
HookFunctionPre("Pawn.Tick", my_callback);

// Get engine
void* engine = GetEngine();

// Iterate pawns
IteratePawns(print_pawn_info);

// Cleanup
ShutdownUEHook();
```

## API Reference

### Core Functions

- `InitializeUEHook()` - Initialize the SDK
- `ShutdownUEHook()` - Cleanup resources
- `IsInitialized()` - Check if SDK is ready

### Function Hooking

- `HookFunctionPre(name, callback)` - Hook before function execution
- `HookFunctionPost(name, callback)` - Hook after function execution
- `UnhookFunction(name)` - Remove hooks
- `GetFunction(name)` - Get function pointer

### Object Access

- `GetEngine()` - Get UEngine instance
- `GetWorld()` - Get UWorld instance
- `GetPawnList(count)` - Get array of pawns
- `GetAllActors(count)` - Get all actors
- `GetAllObjects(count)` - Get ALL objects from GObjects
- `GetObjectsByClass(className, count)` - Find objects by class name
- `GetObjectsByName(objectName, count)` - Find objects by name
- `GetTotalObjectCount()` - Get total object count in GObjects
- `IteratePawns(callback)` - Iterate pawns with callback
- `IterateAllObjects(callback)` - Iterate ALL objects with callback
- `IterateObjectsByClass(className, callback)` - Iterate specific object type

### Property Access

- `GetBoolProperty(obj, name)` - Get boolean property
- `GetIntProperty(obj, name)` - Get integer property
- `GetFloatProperty(obj, name)` - Get float property
- `GetStringProperty(obj, name)` - Get string property
- `GetObjectProperty(obj, name)` - Get object property
- `SetXXXProperty(obj, name, value)` - Set properties

### Reflection

- `GetAllClasses(count)` - Get all UClass objects
- `GetClassesByName(name, count)` - Find classes by name
- `GetAllFunctions(class, count)` - Get class functions
- `GetAllProperties(class, count)` - Get class properties

### Object Type Checking

- `IsObjectOfClass(obj, className)` - Check if object is of specific class
- `IsActor(obj)` - Check if object is an Actor
- `IsPawn(obj)` - Check if object is a Pawn
- `IsPlayerController(obj)` - Check if object is a PlayerController
- `IsComponent(obj)` - Check if object is a Component
- `GetObjectClassName(obj)` - Get object's class name

## Example Use Cases

### 1. Canvas Rendering Hook

```cpp
// Hook rendering function
HookFunctionPost("Canvas.DrawText", [](void* thisPtr, void* params, void* result) {
    // Add custom drawing/overlay here
    std::cout << "Canvas DrawText called!" << std::endl;
});
```

### 2. Player Health Monitoring

```cpp
// Hook player tick to monitor health
HookFunctionPre("Character.Tick", [](void* thisPtr, void* params, void* result) {
    float health = GetFloatProperty(thisPtr, "Health");
    if (health < 50.0f) {
        std::cout << "Low health warning: " << health << std::endl;
    }
});
```

### 3. Object Spawning

```cpp
// Find spawn function
auto spawnFunc = UEHook::UEFunction("World.SpawnActor");
if (spawnFunc.IsValid()) {
    // Call spawn function with parameters
    // This requires setting up proper parameters
}
```

### 4. Full GObjects Scanning

```cpp
// Scan all objects in the game
std::cout << "Total objects: " << GetTotalObjectCount() << std::endl;

// Find all UI elements
auto uiObjects = UEHook::FindObjectsByClass("Widget");
for (void* widget : uiObjects) {
    std::cout << "Found UI widget: " << widget << std::endl;
    
    // Check if it's visible
    bool isVisible = GetBoolProperty(widget, "bIsVisible");
    if (isVisible) {
        std::cout << "  Widget is visible!" << std::endl;
    }
}

// Find all mesh components for ESP
IterateObjectsByClass("MeshComponent", [](void* meshComp) {
    // Get the mesh component's world location
    void* location = GetObjectProperty(meshComp, "WorldLocation");
    // Draw ESP at this location
});

// Find all player controllers
auto controllers = UEHook::FindObjectsByClass("PlayerController");
for (void* controller : controllers) {
    void* pawn = GetObjectProperty(controller, "Pawn");
    if (pawn) {
        std::cout << "Controller " << controller << " controls pawn " << pawn << std::endl;
    }
}
```

### 5. Class Information Gathering

```cpp
// Find all weapon classes
auto weaponClasses = UEHook::FindClasses("Weapon");
for (const auto& weaponClass : weaponClasses) {
    std::cout << "Found weapon class with " << weaponClass.GetFunctions().size() << " functions\n";
    
    // List properties
    for (const auto& prop : weaponClass.GetProperties()) {
        std::cout << "  Property: " << prop.GetName() 
                  << " (size: " << prop.GetSize() << ")\n";
    }
}
```

## Integration with Your Project

1. Copy the built DLL and header to your project
2. Include `UniversalUEHook.h`
3. Link against the DLL or load it dynamically
4. Initialize the SDK in your DLL_PROCESS_ATTACH

```cpp
// In your DLL
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        // Initialize SDK
        UEHook::SDK sdk;  // RAII - will initialize automatically
        
        // Your mod code here
        SetupHooks();
        
        // SDK will cleanup automatically when dll unloads
    }
    return TRUE;
}
```

## Building

### Requirements

- Windows 10/11
- Visual Studio 2019 or later (or any compiler supporting C++17)
- CMake 3.16+

### Build Steps

```bash
git clone [repository-url]
cd UniversalUEHook
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=ON
cmake --build . --config Release
```

### Output

- `bin/UniversalUEHook.dll` - Main SDK library
- `bin/cpp_example.exe` - Example usage (can be injected as DLL)
- `include/UniversalUEHook.h` - Public header

## Supported Games

This SDK should work with most UE3/UE4/UE5 games. The automatic pattern detection adapts to different engine versions.

## Safety Notes

- Always test in single-player or practice modes first
- Be careful with property modifications - they can crash the game
- This SDK is intended for educational and legitimate modding purposes
- Respect game terms of service and applicable laws

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## License

[Your chosen license here]

## Troubleshooting

### SDK fails to initialize
- Make sure the game is fully loaded before initializing
- Try different injection timing (add delays)
- Check if the game uses protection that interferes

### Functions not found
- Verify function names (they may be different across UE versions)
- Try partial name matching
- Use reflection to explore available functions

### Crashes
- Validate pointers before dereferencing
- Be careful with string property access
- Make sure hook callbacks don't modify stack in dangerous ways

### Performance
- Avoid excessive property access in hot paths
- Cache function pointers instead of looking them up repeatedly
- Use filtered iteration instead of processing all objects
