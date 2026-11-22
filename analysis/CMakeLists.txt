# qacq_analysis: the "Analyse" module without Qt (spectrum of a zone of a signal as the plot shows it,
# measurements of every channel, curves computed from the channels, export) and the recipes of the
# generator. Built on qacq_dsp, qacq_model and qacq_plot; tested on its own.
add_library(qacq_analysis STATIC
    src/analysis.cpp
    src/calculus.cpp
    src/curves.cpp
    src/expression.cpp
    src/filter_code.cpp
    src/filters.cpp
    src/generation.cpp
    src/measures.cpp
    src/playback.cpp
)
add_library(qacq::analysis ALIAS qacq_analysis)

target_include_directories(qacq_analysis PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(qacq_analysis PUBLIC qacq::dsp qacq::model qacq::plot)
target_compile_features(qacq_analysis PUBLIC cxx_std_20)
qacq_set_warnings(qacq_analysis)

get_target_property(_qacq_analysis_libs qacq_analysis LINK_LIBRARIES)
if(_qacq_analysis_libs MATCHES "Qt[0-9]*::")
    message(FATAL_ERROR "qacq_analysis must not depend on Qt")
endif()
