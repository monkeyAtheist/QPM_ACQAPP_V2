cmake_minimum_required(VERSION 3.21)

project(QTacquisitionApp
    VERSION 2.0.0
    DESCRIPTION "Signal acquisition and processing for teaching and embedded/RF development"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(QACQ_BUILD_TESTS "Build the signal-processing regression tests" ON)
option(QACQ_BUILD_APP "Build the Qt Quick application (needs Qt 6.8 or later)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Layer 1 - signal-processing core and acquisition decoding (no Qt, ever). Phase 0.
add_subdirectory(dsp)
add_subdirectory(io)

# Layer 2 - application logic (QtCore) and layer 3 - Qt Quick interface: phases 1 to 3.
# Without Qt (CI runners, core-only work) the core and its tests still build.
if(QACQ_BUILD_APP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app/CMakeLists.txt")
    find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Concurrent Qml Quick QuickControls2 SerialPort Network)
    if(Qt6_FOUND)
        qt_standard_project_setup(REQUIRES 6.8)
        add_subdirectory(app)
        add_subdirectory(ui)
        message(STATUS "QTacquisitionApp: Qt ${Qt6_VERSION} found, building the application")
    else()
        message(STATUS "QTacquisitionApp: Qt 6.8+ not found, building the core and tests only")
    endif()
else()
    message(STATUS "QTacquisitionApp: phase 0, building the core libraries and their tests")
endif()

if(QACQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
