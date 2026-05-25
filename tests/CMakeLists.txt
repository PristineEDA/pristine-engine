add_executable(
	pristine_unit_tests
	unit/CompilationServiceTests.cpp
	unit/MessageStreamTests.cpp
	unit/ServerLifecycleTests.cpp)
target_link_libraries(pristine_unit_tests PRIVATE pristine_core Catch2::Catch2WithMain)

add_test(NAME pristine_unit_tests COMMAND pristine_unit_tests)