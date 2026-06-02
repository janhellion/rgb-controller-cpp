cmake_minimum_required(VERSION 3.20)
project(rgb-controller-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── OpenRGB-cppSDK (protocol layer, replaces raw sockets) ──
add_subdirectory(external/OpenRGB-cppSDK EXCLUDE_FROM_ALL)
# orgbsdk compiles as static lib; we need its public headers
# The include path is already set via target_include_directories in the SDK's CMakeLists

# ── Dependencies ──
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Svg)

# ── Engine (effects only, protocol now handled by SDK) ──
set(ENGINE_SRC
    src/engine/effects.cpp
)

# ── Standalone application ──
add_executable(rgb-controller
    src/main_standalone.cpp
    ${ENGINE_SRC}
    src/engine/wave_math.h
)

target_link_libraries(rgb-controller PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Svg
    orgbsdk
    cppnet
    cppbase
)

target_include_directories(rgb-controller PRIVATE
    ${CMAKE_SOURCE_DIR}/external/OpenRGB-cppSDK/include
)

set_target_properties(rgb-controller PROPERTIES
    AUTOMOC_MACRO_FILENAME "main_standalone.moc"
)

# Install
include(GNUInstallDirs)
install(TARGETS rgb-controller RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES rgb-controller.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# ── KCM Plugin (optional, requires KF6) ──
option(BUILD_KCM "Build KDE System Settings KCM plugin" OFF)
if(BUILD_KCM)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Svg)
    find_package(KF6KCMUtils REQUIRED)
    find_package(KF6CoreAddons REQUIRED)
    find_package(KF6Kirigami REQUIRED)
    find_package(KF6I18n REQUIRED)

    add_library(rgb_controller_kcm MODULE
        src/main.cpp
        ${ENGINE_SRC}
    )

    target_link_libraries(rgb_controller_kcm PRIVATE
        Qt6::Core
        Qt6::Widgets
        KF6::KCMUtils
        KF6::CoreAddons
        KF6::Kirigami
        KF6::I18n
        orgbsdk
        cppnet
        cppbase
    )

    target_include_directories(rgb_controller_kcm PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/external/OpenRGB-cppSDK/include
    )

    set_target_properties(rgb_controller_kcm PROPERTIES
        AUTOMOC ON
        PREFIX ""
    )

    install(TARGETS rgb_controller_kcm
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/qt6/plugins/plasma/kcms
    )
    install(FILES kcm_rgbcontroller.desktop
        DESTINATION ${CMAKE_INSTALL_PREFIX}/share/kservices6
    )
    install(FILES kcm_rgbcontroller.json
        DESTINATION ${CMAKE_INSTALL_PREFIX}/share/kservices6
    )
endif()
