# qacq_model: the signals of a session, undo/redo, the session file and CSV/WAV import and export.
# Pure C++20, no Qt dependency: the Qt list model of phase 1 (app/) wraps it, and it is tested on its own.
add_library(qacq_model STATIC
    src/csv.cpp
    src/document.cpp
    src/file_names.h
    src/json.cpp
    src/session_file.cpp
    src/signal.cpp
    src/wav.cpp
)
add_library(qacq::model ALIAS qacq_model)

target_include_directories(qacq_model PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(qacq_model PUBLIC cxx_std_20)
target_link_libraries(qacq_model PUBLIC qacq::io)
qacq_set_warnings(qacq_model)

get_target_property(_qacq_model_libs qacq_model LINK_LIBRARIES)
if(_qacq_model_libs MATCHES "Qt[0-9]*::")
    message(FATAL_ERROR "qacq_model must not depend on Qt")
endif()
