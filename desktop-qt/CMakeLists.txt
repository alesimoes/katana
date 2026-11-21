cmake_minimum_required(VERSION 3.21)
project(katana-desktop-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Test)
qt_standard_project_setup()

qt_add_executable(katana-desktop-qt
    src/main.cpp
    src/backend.cpp
    src/backend.h
    src/terminal.cpp
    src/terminal.h
    src/sessoes.cpp
    src/sessoes.h
    src/selftest.cpp
    src/selftest.h
)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(katana-desktop-qt
    URI Katana
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/ActivityGroup.qml
        qml/ActionRow.qml
        qml/PlanView.qml
        qml/Verbs.js
        qml/Highlight.js
        qml/Theme.qml
        qml/Icon.qml
        qml/TerminalPanel.qml
        qml/IconButton.qml
        qml/Sidebar.qml
        qml/ModelPicker.qml
        qml/ModelStore.qml
        qml/MessageView.qml
        qml/AssistantBody.qml
        qml/CodeBlock.qml
        qml/ToolCard.qml
        qml/Composer.qml
        qml/TextMenu.qml
        qml/ContextMeter.qml
        qml/Markdown.js
)

qt_add_resources(katana-desktop-qt icones
    PREFIX /icons
    BASE icons
    FILES
        icons/katana.svg
        icons/samurai.png
        icons/titulo-claro.png
        icons/titulo-escuro.png
        icons/katana-16.png
        icons/katana-32.png
        icons/katana-48.png
        icons/katana-64.png
        icons/katana-128.png
        icons/katana-256.png
        icons/katana-k.png
        icons/katana-512.png
)

qt_add_resources(katana-desktop-qt fontes
    PREFIX /fonts
    BASE fonts
    FILES
        fonts/JetBrainsMono-Regular.ttf
        fonts/JetBrainsMono-Medium.ttf
        fonts/JetBrainsMono-SemiBold.ttf
        fonts/JetBrainsMono-Bold.ttf
        fonts/JetBrainsMono-Italic.ttf
        fonts/IBMPlexSans-Regular.ttf
        fonts/IBMPlexSans-Medium.ttf
        fonts/IBMPlexSans-SemiBold.ttf
        fonts/IBMPlexSans-Bold.ttf
        fonts/IBMPlexSans-Italic.ttf
        fonts/IBMPlexMono-Regular.ttf
        fonts/IBMPlexMono-Medium.ttf
)

target_link_libraries(katana-desktop-qt PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test)
