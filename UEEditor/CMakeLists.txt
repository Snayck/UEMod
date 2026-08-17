cmake_minimum_required(VERSION 3.20)
project(UEEditor LANGUAGES CXX)

# language settings
set(CMAKE_CXX_STANDARD 17)          # UEHook's API uses C++17-era stuff
set(CMAKE_CXX_STANDARD_REQUIRED ON) # error out instead of silently downgrading


set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/ImGui)
set(NE_DIR ${CMAKE_SOURCE_DIR}/third_party/NodeEditor)
set(NE_UTIL ${NE_DIR}/examples/blueprints-example/utilities)

# the ImGui sources this target needs
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

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/../UEHook ${CMAKE_BINARY_DIR}/UEHook)

# target
add_executable(UEEditor
    src/main.cpp
    src/toolbox.cpp
    src/graph.cpp
    src/nodes.cpp
    ${IMGUI_SOURCES}
    ${NE_SOURCES}
)

# headers
target_include_directories(UEEditor PRIVATE
    ${IMGUI_DIR}
    ${NE_DIR}
    ${NE_UTIL}
    ${CMAKE_SOURCE_DIR}/src
)

# windows link libraries
target_link_libraries(UEEditor PRIVATE
    d3d11
    dxgi
    d3dcompiler
    UEHook           # reflection + hooking backend (brings its own include dirs)
)