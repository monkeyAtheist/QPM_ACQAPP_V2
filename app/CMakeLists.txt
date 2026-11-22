# Layer 2 - application logic on QtCore (no widgets, no QML): the list of signals for the interface with
# undo, the session (files, import, export), the remembered settings, and the acquisition: the sources on
# their threads (serial port, TCP, UDP, SCPI, sound card, simulator), the list of serial ports and the
# controller; the generator (its preview apart); the filters (designed, tried on a signal, its preview
# apart); quantities for the fields (Units). Testable without any window.
qt_add_library(qacq_app STATIC
    include/qacq/app/acquisition_controller.h
    include/qacq/app/analysis_controller.h
    include/qacq/app/curves_controller.h
    include/qacq/app/acquisition_source.h
    include/qacq/app/app_settings.h
    include/qacq/app/audio_format.h
    include/qacq/app/audio_player.h
    include/qacq/app/filter_controller.h
    include/qacq/app/format.h
    include/qacq/app/generator_controller.h
    include/qacq/app/qt_convert.h
    include/qacq/app/serial_port_list.h
    include/qacq/app/session.h
    include/qacq/app/signal_list_model.h
    include/qacq/app/units.h
    src/acquisition_controller.cpp
    src/analysis_controller.cpp
    src/curves_controller.cpp
    src/acquisition_source.cpp
    src/app_settings.cpp
    src/audio_format.cpp
    src/audio_player.cpp
    src/filter_controller.cpp
    src/format.cpp
    src/generator_controller.cpp
    src/qt_convert.cpp
    src/serial_port_list.cpp
    src/session.cpp
    src/signal_list_model.cpp
    src/units.cpp
)
target_include_directories(qacq_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(qacq_app PUBLIC
    qacq::acquisition qacq::analysis qacq::model qacq::dsp qacq::plot
    Qt6::Core Qt6::Network Qt6::SerialPort Qt6::Multimedia)
target_compile_definitions(qacq_app PRIVATE QACQ_VERSION="${PROJECT_VERSION}")
qacq_set_warnings(qacq_app)
