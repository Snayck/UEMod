cmake_minimum_required(VERSION 3.20)
project(UEEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF -> standalone .exe (no backend init, nothing executes).
# ON  -> injected SHARED library that initializes uehook and runs graphs.
option(UEEDITOR_DLL "Build as an injected shared library instead of a standalone exe" OFF)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/ImGui)
set(NE_DIR ${CMAKE_SOURCE_DIR}/third_party/NodeEditor)
set(NE_UTIL ${NE_DIR}/examples/blueprints-example/utilities)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
)
set(NE_SOURCES
    ${NE_DIR}/imgui_node_editor.cpp
    ${NE_DIR}/imgui_node_editor_api.cpp
    ${NE_DIR}/crude_json.cpp
    ${NE_DIR}/imgui_canvas.cpp
    ${NE_UTIL}/widgets.cpp
    ${NE_UTIL}/drawing.cpp
)

# Sources shared by both build flavors.
set(COMMON_SOURCES
    src/app.cpp
    src/toolbox.cpp
    src/graph.cpp
    src/nodes.cpp
    src/nodelogic.cpp
    ${IMGUI_SOURCES}
    ${NE_SOURCES}
)

if(UEEDITOR_DLL)
    add_library(UEEditor SHARED src/dllmain.cpp ${COMMON_SOURCES})
    target_compile_definitions(UEEditor PRIVATE UEEDITOR_WITH_BACKEND)

    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/../UEHook ${CMAKE_BINARY_DIR}/UEHook)
    target_link_libraries(UEEditor PRIVATE UEHook)
else()
    add_executable(UEEditor src/main.cpp ${COMMON_SOURCES})
endif()

target_include_directories(UEEditor PRIVATE
    ${IMGUI_DIR}
    ${NE_DIR}
    ${NE_UTIL}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(UEEditor PRIVATE
    d3d11
    dxgi
    d3dcompiler
)
