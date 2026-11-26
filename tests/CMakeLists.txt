# Signal-processing regression tests. One executable, one CTest entry per group (tag filter).
add_executable(qacq_tests
    main.cpp
    test_fft.cpp
    test_spectrum.cpp
    test_tone.cpp
    test_filters.cpp
    test_filter_module.cpp
    test_filter_code.cpp
    test_generator.cpp
    test_modulation.cpp
    test_link.cpp
    test_sync.cpp
    test_equalizer.cpp
    test_io.cpp
    test_model.cpp
    test_acquisition.cpp
    test_plot.cpp
    test_analysis.cpp
    test_calculus.cpp
    test_curves.cpp
    test_expression.cpp
    test_generation.cpp
    test_frames.cpp
    test_playback.cpp
    qt_keywords_check.cpp
)
target_link_libraries(qacq_tests PRIVATE qacq::dsp qacq::io qacq::model qacq::acquisition qacq::plot qacq::analysis)
target_include_directories(qacq_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
qacq_set_warnings(qacq_tests)

function(qacq_add_test_group name filter)
    add_test(NAME ${name} COMMAND qacq_tests "${filter}")
    set_tests_properties(${name} PROPERTIES LABELS "${ARGN}")
endfunction()

qacq_add_test_group(fft        "[fft]"        fast)
qacq_add_test_group(window     "[window]"     fast)
qacq_add_test_group(spectrum   "[spectrum]"   fast)
qacq_add_test_group(tone       "[tone]"       fast)
qacq_add_test_group(iir        "[iir]"        fast)
qacq_add_test_group(fir        "[fir]"        fast)
qacq_add_test_group(generator  "[generator]"  fast)
qacq_add_test_group(modulation "[modulation]" fast)
qacq_add_test_group(link       "[link]"       fast)
qacq_add_test_group(sync       "[sync]"       fast)
qacq_add_test_group(equalizer  "[equalizer]"  fast)
qacq_add_test_group(crc        "[crc]"        fast)
qacq_add_test_group(text       "[text]"       fast)
qacq_add_test_group(binary     "[binary]"     fast)
qacq_add_test_group(scpi       "[scpi]"       fast)
qacq_add_test_group(ring       "[ring]"       fast)
qacq_add_test_group(json       "[json]"       fast)
qacq_add_test_group(model      "[model]"      fast)
qacq_add_test_group(session    "[session]"    fast)
qacq_add_test_group(csv        "[csv]"        fast)
qacq_add_test_group(plot       "[plot]"       fast)
qacq_add_test_group(analysis   "[analysis]"   fast)
qacq_add_test_group(frames     "[frames]"     fast)
qacq_add_test_group(code       "[code]"       fast)
qacq_add_test_group(wav        "[wav]"        fast)
qacq_add_test_group(acquisition "[acquisition]" fast)
qacq_add_test_group(ber        "[ber]"        slow)
qacq_add_test_group(audit      "[audit]"      slow)

# The C written for microcontrollers (module Filtres): qacq_codegen writes it for a few filters in every number
# format, the C compiler of the build compiles it all as C99 with strict warnings (one translation unit, so that
# no two names clash), and the self-tests run: bit for bit in fixed point. Left out where no C compiler is found.
include(CheckLanguage)
check_language(C)
if(CMAKE_C_COMPILER)
    enable_language(C)
    add_executable(qacq_codegen codegen/generate.cpp)
    target_link_libraries(qacq_codegen PRIVATE qacq::analysis)
    qacq_set_warnings(qacq_codegen)
    set(_qacq_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_custom_command(
        OUTPUT "${_qacq_generated}/codegen_main.c"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_qacq_generated}"
        COMMAND qacq_codegen "${_qacq_generated}"
        DEPENDS qacq_codegen
        COMMENT "C des filtres pour microcontrôleur"
        VERBATIM)
    add_executable(qacq_generated_c "${_qacq_generated}/codegen_main.c")
    target_include_directories(qacq_generated_c PRIVATE "${_qacq_generated}")
    set_target_properties(qacq_generated_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(qacq_generated_c PRIVATE /W4 /utf-8)
        if(QACQ_WARNINGS_AS_ERRORS)
            target_compile_options(qacq_generated_c PRIVATE /WX)
        endif()
    else()
        target_compile_options(qacq_generated_c PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wdouble-promotion)
        if(QACQ_WARNINGS_AS_ERRORS)
            target_compile_options(qacq_generated_c PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME codegen COMMAND qacq_generated_c)
    set_tests_properties(codegen PROPERTIES LABELS fast)
endif()

# Application layer (needs Qt): signal list model, session files, remembered settings.
if(TARGET qacq_app)
    add_executable(qacq_app_tests app_main.cpp test_app.cpp test_acquisition_app.cpp test_analysis_app.cpp
                               test_generator_app.cpp test_filter_app.cpp test_modulation_app.cpp)
    target_link_libraries(qacq_app_tests PRIVATE qacq_app)
    target_include_directories(qacq_app_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    qacq_set_warnings(qacq_app_tests)
    add_test(NAME app COMMAND qacq_app_tests "[app]")
    set_tests_properties(app PROPERTIES LABELS "fast;app")
    # Sources on their threads against a simulator, local TCP/UDP servers and a fake SCPI instrument.
    add_test(NAME sources COMMAND qacq_app_tests "[sources]")
    set_tests_properties(sources PROPERTIES LABELS "app" TIMEOUT 120)
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        # Run against the Qt the build used, without a deployment step.
        if(TARGET Qt6::qmake)
            get_target_property(_qacq_qmake Qt6::qmake IMPORTED_LOCATION)
            get_filename_component(_qacq_qt_bin "${_qacq_qmake}" DIRECTORY)
        else()
            get_filename_component(_qacq_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
        endif()
        set_tests_properties(app sources PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qacq_qt_bin}")
    endif()
endif()
