add_executable(
	pristine_unit_tests
	unit/AstIndexTests.cpp
	unit/CodeActionProviderTests.cpp
	unit/CompletionProviderTests.cpp
	unit/CompilationServiceTests.cpp
	unit/DiagnosticProviderTests.cpp
	unit/DesignGraphProviderTests.cpp
	unit/GoldenSemanticTests.cpp
	unit/MessageStreamTests.cpp
	unit/NavigationProviderTests.cpp
	unit/ProviderCompletenessMatrixTests.cpp
	unit/QueryCacheTests.cpp
	unit/SemanticEngineTests.cpp
	unit/SignatureInlayProviderTests.cpp
	unit/SnapshotBuilderTests.cpp
	unit/SemanticWorkspaceTests.cpp
	unit/SourceUtilTests.cpp
	unit/SyntaxDocumentCacheTests.cpp
	unit/ServerLifecycleTests.cpp
	unit/WorkspaceDiscoveryIndexTests.cpp)
target_link_libraries(pristine_unit_tests PRIVATE pristine_core Catch2::Catch2WithMain)

add_test(NAME pristine_unit_tests COMMAND pristine_unit_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
	NAME pristine_lsp_core_e2e
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/lsp_core_smoke.py
		$<TARGET_FILE:pristine-engine>)

add_test(
	NAME pristine_differential_slang_server
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/differential/slang_server_differential.py
		$<TARGET_FILE:pristine-engine>)
set_tests_properties(pristine_differential_slang_server PROPERTIES
	ENVIRONMENT "SLANG_SERVER_ROOT=$ENV{SLANG_SERVER_ROOT}"
	LABELS "differential"
	SKIP_REGULAR_EXPRESSION "SKIP:")

if(PRISTINE_BUILD_PERF_TESTS)
	add_executable(pristine_perf_tests perf/SemanticPerfSmoke.cpp)
	target_link_libraries(pristine_perf_tests PRIVATE pristine_core)

	add_executable(pristine_rtl_e2e_stress perf/RtlE2eDesignStress.cpp)
	target_link_libraries(pristine_rtl_e2e_stress PRIVATE pristine_core)
	add_test(
		NAME pristine_rtl_e2e_stress
		COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/perf/run_rtl_e2e_stress.py
			$<TARGET_FILE:pristine_rtl_e2e_stress>)
	set_tests_properties(pristine_rtl_e2e_stress PROPERTIES
		LABELS "perf;rtl-e2e;stress"
		SKIP_RETURN_CODE 77
		SKIP_REGULAR_EXPRESSION "SKIP:")

	add_executable(pristine_rtl_e2e_lsp_stress perf/RtlE2eLspStressClient.cpp)
	target_link_libraries(pristine_rtl_e2e_lsp_stress PRIVATE lsp)
	add_test(
		NAME pristine_rtl_e2e_smoke
		COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/perf/run_lsp_framework_client_smoke.py
			$<TARGET_FILE:pristine_rtl_e2e_lsp_stress>
			$<TARGET_FILE:pristine-engine>
			${CMAKE_CURRENT_BINARY_DIR})
	set_tests_properties(pristine_rtl_e2e_smoke PROPERTIES
		LABELS "perf;rtl-e2e;smoke;lsp")

	add_test(
		NAME pristine_rtl_e2e_lsp_stress
		COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/perf/run_rtl_e2e_lsp_stress.py
			$<TARGET_FILE:pristine_rtl_e2e_lsp_stress>
			$<TARGET_FILE:pristine-engine>
			${CMAKE_CURRENT_BINARY_DIR})
	set_tests_properties(pristine_rtl_e2e_lsp_stress PROPERTIES
		LABELS "perf;rtl-e2e;lsp"
		SKIP_RETURN_CODE 77
		SKIP_REGULAR_EXPRESSION "SKIP:")
endif()
