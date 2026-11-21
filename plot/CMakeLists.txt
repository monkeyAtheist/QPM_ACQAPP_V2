# qacq_plot: the plot engine shared by every view (graduations, labels, quantities typed and shown,
# traces at any zoom). Pure C++20, no Qt dependency: the Qt Quick items of the interface draw what it
# computes, and it is tested on its own.
add_library(qacq_plot STATIC
    src/axis.cpp
    src/trace.cpp
)
add_library(qacq::plot ALIAS qacq_plot)

target_include_directories(qacq_plot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(qacq_plot PUBLIC cxx_std_20)
qacq_set_warnings(qacq_plot)

get_target_property(_qacq_plot_libs qacq_plot LINK_LIBRARIES)
if(_qacq_plot_libs MATCHES "Qt[0-9]*::")
    message(FATAL_ERROR "qacq_plot must not depend on Qt")
endif()
