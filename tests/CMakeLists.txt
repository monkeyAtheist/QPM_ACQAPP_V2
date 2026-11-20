# Signal-processing regression tests. One executable, one CTest entry per group (tag filter).
add_executable(qacq_tests
    main.cpp
    test_fft.cpp
    test_spectrum.cpp
    test_tone.cpp
    test_filters.cpp
)
target_link_libraries(qacq_tests PRIVATE qacq::dsp)
target_include_directories(qacq_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
qacq_set_warnings(qacq_tests)

function(qacq_add_test_group name filter)
    add_test(NAME ${name} COMMAND qacq_tests "${filter}")
    set_tests_properties(${name} PROPERTIES LABELS "${ARGN}")
endfunction()

qacq_add_test_group(fft      "[fft]"      fast)
qacq_add_test_group(window   "[window]"   fast)
qacq_add_test_group(spectrum "[spectrum]" fast)
qacq_add_test_group(tone     "[tone]"     fast)
qacq_add_test_group(iir      "[iir]"      fast)
qacq_add_test_group(fir      "[fir]"      fast)
qacq_add_test_group(audit    "[audit]"    slow)
