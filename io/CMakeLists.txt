# qacq_io: decoding of acquired data (text lines, binary frames, SCPI answers) and the lock-free buffer
# between the acquisition thread and the interface. Pure C++20, no Qt dependency: the Qt sources
# (QSerialPort, sockets) of phase 2 only feed it bytes.
add_library(qacq_io STATIC
    src/binary_decoder.cpp
    src/crc.cpp
    src/scpi.cpp
    src/text_decoder.cpp
)
add_library(qacq::io ALIAS qacq_io)

target_include_directories(qacq_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(qacq_io PUBLIC cxx_std_20)
qacq_set_warnings(qacq_io)

find_package(Threads REQUIRED)
target_link_libraries(qacq_io PUBLIC Threads::Threads)

get_target_property(_qacq_io_libs qacq_io LINK_LIBRARIES)
if(_qacq_io_libs MATCHES "Qt[0-9]*::")
    message(FATAL_ERROR "qacq_io must not depend on Qt")
endif()
