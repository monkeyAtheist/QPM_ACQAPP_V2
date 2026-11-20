# qacq_acquisition: what an acquisition needs besides Qt: source settings and presets, the decoder of the
# source, the lock-free pipe to the interface, the recorder, the rate measurement, the trigger, the
# simulated device, the firmware examples and the continuous recording to disk. Pure C++20, no Qt:
# the Qt sources of phase 2 (serial port, sockets, sound card) only feed it bytes.
add_library(qacq_acquisition STATIC
    src/firmware_example.cpp
    src/rate_trigger.cpp
    src/recorder.cpp
    src/settings.cpp
    src/simulator.cpp
    src/stream_writer.cpp
)
add_library(qacq::acquisition ALIAS qacq_acquisition)

target_include_directories(qacq_acquisition PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(qacq_acquisition PUBLIC cxx_std_20)
target_link_libraries(qacq_acquisition PUBLIC qacq::io qacq::model qacq::dsp)
qacq_set_warnings(qacq_acquisition)

get_target_property(_qacq_acquisition_libs qacq_acquisition LINK_LIBRARIES)
if(_qacq_acquisition_libs MATCHES "Qt[0-9]*::")
    message(FATAL_ERROR "qacq_acquisition must not depend on Qt")
endif()
