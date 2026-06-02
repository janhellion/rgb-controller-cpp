cmake_minimum_required(VERSION 3.20)
project(rgb-controller-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Svg Qml Quick)

set(ENGINE_SRC
    src/engine/effects.cpp
)

# ── Standalone tray app ──
add_executable(rgb-controller
    src/main_standalone.cpp
    ${ENGINE_SRC}
    src/engine/orgb_client.h
    src/engine/wave_math.h
)

target_link_libraries(rgb-controller PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Svg
    pthread
)

set_target_properties(rgb-controller PROPERTIES
    AUTOMOC_MACRO_FILENAME "main_standalone.moc"
)

include(GNUInstallDirs)
install(TARGETS rgb-controller RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES rgb-controller.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# ── KDE System Settings KCM (Plasma 6 QML-based) ──
option(BUILD_KCM "Build KDE System Settings KCM plugin" OFF)
if(BUILD_KCM)
    find_package(ECM REQUIRED NO_MODULE)
    set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
    include(KDEInstallDirs)
    include(KDECMakeSettings)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(BUILD_SHARED_LIBS ON)
    find_package(Qt6 REQUIRED COMPONENTS Core Qml Quick)
    find_package(KF6KCMUtils REQUIRED)
    find_package(KF6CoreAddons REQUIRED)
    find_package(KF6Kirigami REQUIRED)
    find_package(KF6I18n REQUIRED)

    set(KCM_SRC
        src/main.cpp
        src/kcm_engine.cpp
        src/kcm_engine.h
        ${ENGINE_SRC}
    )

    kcmutils_add_qml_kcm(kcm_rgbcontroller SOURCES ${KCM_SRC})

    target_link_libraries(kcm_rgbcontroller PRIVATE
        Qt6::Core Qt6::Qml Qt6::Quick
        KF6::KCMUtils
        KF6::KCMUtilsQuick
        KF6::CoreAddons
        KF6::Kirigami
        KF6::I18n
        pthread
    )

    target_include_directories(kcm_rgbcontroller PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
    )

    message(STATUS "KCM build enabled — target: rgb_controller_kcm")
endif()
