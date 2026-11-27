cmake_minimum_required(VERSION 3.19)
project(MasterTex VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)

set(SOURCES
    src/main.cpp
    src/DocumentManager.cpp
    src/Project.cpp
    src/LatexHighlighter.cpp
    src/CodeEditor.cpp
    src/StructurePanel.cpp
    src/SymbolPanel.cpp
    src/Wizards.cpp
    src/SettingsDialog.cpp
    src/MainWindow.cpp
    src/MainWindowMenus.cpp
    src/FindBar.cpp
    src/Dialogs.cpp
)

set(HEADERS
    src/DocumentManager.h
    src/Project.h
    src/LatexHighlighter.h
    src/CodeEditor.h
    src/StructurePanel.h
    src/SymbolPanel.h
    src/Wizards.h
    src/SettingsDialog.h
    src/MainWindow.h
    src/FindBar.h
    src/Dialogs.h
)

if(APPLE)
    set(MACOSX_BUNDLE_BUNDLE_NAME "MasterTex")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "org.mastertex.MasterTex")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    add_executable(MasterTex MACOSX_BUNDLE ${SOURCES} ${HEADERS})
else()
    add_executable(MasterTex ${SOURCES} ${HEADERS})
endif()

target_link_libraries(MasterTex PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
