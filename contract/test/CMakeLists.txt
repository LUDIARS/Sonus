snd_add_test(contract_test
  SOURCES
    hash_test.cpp
    api_types_test.cpp
    c_api_check.c
    schema_test.cpp
    commands_test.cpp
    format_test.cpp
    contract11_test.cpp
    contract11_internal_test.cpp
  LINK sonus_contract)

# The C file must really be compiled as C11 with all warnings.
set_source_files_properties(c_api_check.c PROPERTIES LANGUAGE C)

# Compile-time ownership detection: this target must NOT compile.
add_executable(contract_schema_violation EXCLUDE_FROM_ALL schema_violation.cpp)
target_link_libraries(contract_schema_violation PRIVATE sonus_contract)
snd_apply_options(contract_schema_violation)
add_test(NAME contract_schema_violation_rejected
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target contract_schema_violation)
set_tests_properties(contract_schema_violation_rejected PROPERTIES WILL_FAIL TRUE TIMEOUT 300)

add_executable(contract_undeclared_read EXCLUDE_FROM_ALL schema_undeclared_read.cpp)
target_link_libraries(contract_undeclared_read PRIVATE sonus_contract)
snd_apply_options(contract_undeclared_read)
add_test(NAME contract_undeclared_read_rejected
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target contract_undeclared_read)
set_tests_properties(contract_undeclared_read_rejected PROPERTIES WILL_FAIL TRUE TIMEOUT 300)

# Independent self-check of contract 1.1 (test owner).
snd_add_test(contract_indep11_test SOURCES indep_contract11_test.cpp LINK sonus_contract)
