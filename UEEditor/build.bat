cmake -S . -B build-dll -G "Visual Studio 17 2022" -A x64 -DUEEDITOR_DLL=ON
cmake --build build-dll --config Release