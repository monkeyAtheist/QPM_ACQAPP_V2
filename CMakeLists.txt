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

# Layer 1 - signal processing, acquisition decoding, session model, acquisition logic, plot engine
# (no Qt, ever).
add_subdirectory(dsp)
add_subdirectory(io)
add_subdirectory(model)
add_subdirectory(acquisition)
add_subdirectory(plot)
add_subdirectory(analysis)

# Layer 2 - application logic (QtCore, phase 1) and layer 3 - Qt Quick interface (phase 3).
# Without Qt (CI runners, core-only work) the core libraries and their tests still build.
if(QACQ_BUILD_APP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app/CMakeLists.txt")
    # The application layer: sockets, serial ports and the sound card for the acquisition.
    set(_qacq_qt_components Core Network SerialPort Multimedia)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui/CMakeLists.txt")
        list(APPEND _qacq_qt_components Gui Concurrent Qml Quick QuickControls2)
    endif()
    find_package(Qt6 6.8 QUIET COMPONENTS ${_qacq_qt_components})
    if(Qt6_FOUND)
        qt_standard_project_setup(REQUIRES 6.8)
        add_subdirectory(app)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui/CMakeLists.txt")
            add_subdirectory(ui)
            message(STATUS "QTacquisitionApp: Qt ${Qt6_VERSION} found, building the application")
        else()
            message(STATUS "QTacquisitionApp: Qt ${Qt6_VERSION} found, building the application layer (no interface yet)")
        endif()
    else()
        message(STATUS "QTacquisitionApp: Qt 6.8+ (Core, Network, SerialPort, Multimedia) not found, building the core libraries and their tests only")
    endif()
else()
    message(STATUS "QTacquisitionApp: building the core libraries and their tests")
endif()

if(QACQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
