# qas — unit tests.
#
# One executable per suite, each linking qas_core and registered with CTest. A
# helper function keeps this DRY: there is no per-test boilerplate to drift, and
# adding a suite is a single line (no hardcoded repetition — coding-standard.md).

function(qas_add_test name)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE qas_core)
    # The test harness header (qtest.h) lives alongside the test sources.
    target_include_directories(test_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

qas_add_test(status)
qas_add_test(source)
qas_add_test(token)
qas_add_test(lexer)
qas_add_test(reg)
qas_add_test(buf)
qas_add_test(elf)
qas_add_test(arena)
qas_add_test(parser)
qas_add_test(encoder)
