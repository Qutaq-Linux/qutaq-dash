cmake_minimum_required(VERSION 3.16)
project(qutaq-dash VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Qt >= 6.5: per-window layer-shell integration (the Dash is a layer surface, its menus are xdg_popups)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets DBus)
# UI translations (lrelease). Without Qt Linguist the Dash still builds, in English only.
find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS LinguistTools)

# LayerShellQt 5.x (Plasma 5) turns ALL windows into layer surfaces, including QMenu. Hence 6.x only.
find_package(LayerShellQt 6.0 REQUIRED)

qt_add_executable(qutaq-dash
    src/main.cpp
    src/dashwindow.h        src/dashwindow.cpp
    src/resultsview.h       src/resultsview.cpp
    src/hudwindow.h         src/hudwindow.cpp
    src/search.h            src/search.cpp
    src/thumbnails.h        src/thumbnails.cpp
    src/desktopentries.h    src/desktopentries.cpp
    src/icons.h             src/icons.cpp
    src/settings.h
)

# translations/qutaq-dash_<lang>.ts → .qm files built into the program (:/i18n)
file(GLOB QUTAQ_TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/translations/qutaq-dash_*.ts)
if(TARGET Qt6::lrelease AND QUTAQ_TS_FILES)
    qt_add_translations(qutaq-dash
        TS_FILES ${QUTAQ_TS_FILES}
        RESOURCE_PREFIX /i18n
        LUPDATE_OPTIONS -no-obsolete -locations none
    )
else()
    message(WARNING "Qt6 LinguistTools not found: building without translations (English only)")
endif()

target_link_libraries(qutaq-dash PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus
    LayerShellQt::Interface
)
target_compile_definitions(qutaq-dash PRIVATE
    LSQ_VERSION_MAJOR=${LayerShellQt_VERSION_MAJOR}
    LSQ_VERSION_MINOR=${LayerShellQt_VERSION_MINOR}
)

install(TARGETS qutaq-dash RUNTIME DESTINATION bin)
