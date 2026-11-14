find_package(Qt6 REQUIRED COMPONENTS Test)

# Command line tool used to compare the parsers against reference output.
add_executable(dumptool dumptool.cpp)
target_link_libraries(dumptool PRIVATE swdcore)

# Small freestanding C++ program with debug info: the input of the ELF parser tests.
add_executable(fixture_elf fixture/fixture_a.cpp fixture/fixture_b.cpp)
# (it is only data for the tests: no moc, and never sanitised even when the rest is)
target_compile_options(fixture_elf PRIVATE -g -O0 -ffreestanding -fno-exceptions -fno-rtti -fno-pie -fno-sanitize=all)
target_link_options(fixture_elf PRIVATE -nostdlib -nostartfiles -static -no-pie -fno-sanitize=all -Wl,-e,_start)
set_target_properties(fixture_elf PROPERTIES AUTOMOC OFF)

function(swd_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE swdcore Qt6::Test)
    target_compile_definitions(${name} PRIVATE FIXTURE_ELF="$<TARGET_FILE:fixture_elf>")
    add_dependencies(${name} fixture_elf)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

swd_test(test_processing)
swd_test(test_values)
swd_test(test_stream)
swd_test(test_poller)
swd_test(test_elf)
swd_test(test_svd)
swd_test(test_gui)
