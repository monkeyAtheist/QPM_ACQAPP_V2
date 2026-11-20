# qacq_dsp: the signal-processing core. Pure C++20, no Qt dependency, ever.
add_library(qacq_dsp STATIC
    src/fft.cpp
    src/filter_design.cpp
    src/filtering.cpp
    src/spectrum.cpp
    src/tone.cpp
    src/window.cpp
)
add_library(qacq::dsp ALIAS qacq_dsp)

target_include_directories(qacq_dsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(qacq_dsp PUBLIC cxx_std_20)
qacq_set_warnings(qacq_dsp)
qacq_set_fp_model(qacq_dsp)

# Guard rail: the core must stay Qt-free. Fails the configure step if someone links Qt here.
get_target_property(_qacq_dsp_libs qacq_dsp LINK_LIBRARIES)
if(_qacq_dsp_libs MATCHES "Qt[0-9]*::")
    message(FATAL_ERROR "qacq_dsp must not depend on Qt")
endif()
