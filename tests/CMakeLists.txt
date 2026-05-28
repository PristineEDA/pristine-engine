add_executable(
	pristine_unit_tests
	unit/CompilationServiceTests.cpp
	unit/MessageStreamTests.cpp
	unit/SemanticEngineTests.cpp
	unit/SemanticWorkspaceTests.cpp
	unit/ServerLifecycleTests.cpp
	unit/SymbolIndexTests.cpp)
target_link_libraries(pristine_unit_tests PRIVATE pristine_core Catch2::Catch2WithMain)

add_test(NAME pristine_unit_tests COMMAND pristine_unit_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
	NAME pristine_lsp_core_e2e
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/lsp_core_smoke.py
		$<TARGET_FILE:pristine-lsp>)
