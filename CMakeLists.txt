cmake_minimum_required(VERSION 3.20)
project(rgb-controller-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Svg)

set(ENGINE_SRC
    src/engine/effects.cpp
)

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

option(BUILD_KCM "Build KDE System Settings KCM plugin" OFF)
if(BUILD_KCM)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Svg)
    find_package(KF6KCMUtils REQUIRED)
    find_package(KF6CoreAddons REQUIRED)
    find_package(KF6Kirigami REQUIRED)
    find_package(KF6I18n REQUIRED)

    add_library(rgb_controller_kcm MODULE
        src/main.cpp ${ENGINE_SRC}
    )

    target_link_libraries(rgb_controller_kcm PRIVATE
        Qt6::Core Qt6::Widgets
        KF6::KCMUtils KF6::CoreAddons KF6::Kirigami KF6::I18n
        pthread
    )

    target_include_directories(rgb_controller_kcm PRIVATE ${CMAKE_SOURCE_DIR})

    set_target_properties(rgb_controller_kcm PROPERTIES AUTOMOC ON PREFIX "")

    install(TARGETS rgb_controller_kcm DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/qt6/plugins/plasma/kcms/systemsettings)
    install(FILES kcm_rgbcontroller.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/share/kservices6)
    install(FILES kcm_rgbcontroller.json DESTINATION ${CMAKE_INSTALL_PREFIX}/share/kservices6)
endif()
