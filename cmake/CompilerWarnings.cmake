# Compiler options shared by every first-party target: strict warnings and reproducible floating point.
# Same rules as QTsignalApp V2: the core builds clean with these flags (QACQ_WARNINGS_AS_ERRORS=ON in CI).
option(QACQ_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(qacq_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(QACQ_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
            -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
        if(QACQ_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Reproducible floating point: no fused multiply-add contraction (GCC on ARM64, Clang everywhere),
# so the reference tests give the same last bits on every machine.
function(qacq_set_fp_model target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PUBLIC -ffp-contract=off)
    elseif(MSVC)
        target_compile_options(${target} PUBLIC /fp:precise)
    endif()
endfunction()
