add_executable(
	pristine_unit_tests
	unit/AffectedDependencyGraphTests.cpp
	unit/AstIndexTests.cpp
	unit/BackgroundDiagnosticsWorkerTests.cpp
	unit/CodeActionProviderTests.cpp
	unit/CompletionProviderTests.cpp
	unit/CompilationServiceTests.cpp
	unit/DiagnosticProviderTests.cpp
	unit/DesignGraphProviderTests.cpp
	unit/DebugTraceCompileTests.cpp
	unit/GoldenSemanticTests.cpp
	unit/LayoutProviderTests.cpp
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
	unit/WaveformProviderTests.cpp
	unit/ServerLifecycleTests.cpp
	unit/WorkspaceDiscoveryIndexTests.cpp)
target_link_libraries(pristine_unit_tests PRIVATE pristine_core Catch2::Catch2WithMain)
target_compile_definitions(pristine_unit_tests
	PRIVATE PRISTINE_WELLEN_INPUTS_DIR="${PROJECT_SOURCE_DIR}/.deps/src/wellen/wellen/inputs")

add_test(NAME pristine_unit_tests COMMAND pristine_unit_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
	NAME pristine_script_runner_tests
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/test_runner_scripts.py)

add_test(
	NAME pristine_lsp_core_e2e
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/lsp_core_smoke.py
		$<TARGET_FILE:pristine-engine>)

add_test(
	NAME pristine_waveform_pipe_e2e
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/waveform_pipe_smoke.py
		$<TARGET_FILE:pristine-engine>)

add_test(
	NAME pristine_layout_pipe_e2e
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/layout_pipe_smoke.py
		$<TARGET_FILE:pristine-engine>)

add_test(
	NAME pristine_ihp_lef_corpus
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/ihp_lef_corpus.py
		$<TARGET_FILE:pristine-engine>
		${PROJECT_SOURCE_DIR}/.deps/src/ihp-open-pdk)
set_tests_properties(pristine_ihp_lef_corpus PROPERTIES
	LABELS "layout;lef;ihp"
	SKIP_RETURN_CODE 77
	SKIP_REGULAR_EXPRESSION "SKIP:")

add_test(
	NAME pristine_ihp_gds_corpus
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/ihp_gds_corpus.py
		$<TARGET_FILE:pristine-engine>
		${PROJECT_SOURCE_DIR}/.deps/src/ihp-open-pdk)
set_tests_properties(pristine_ihp_gds_corpus PROPERTIES
	LABELS "layout;gds;ihp"
	TIMEOUT 3600
	SKIP_RETURN_CODE 77
	SKIP_REGULAR_EXPRESSION "SKIP:")

add_test(
	NAME pristine_tt_tinyqv_gds_interaction
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/tt_tinyqv_gds_interaction.py
		$<TARGET_FILE:pristine-engine>
		${PROJECT_SOURCE_DIR}/.deps/src/tinytapeout-sky-25a/projects/tt_um_tt_tinyQV/tt_um_tt_tinyQV.gds)
set_tests_properties(pristine_tt_tinyqv_gds_interaction PROPERTIES
	LABELS "layout;gds;tt;perf"
	TIMEOUT 900
	SKIP_RETURN_CODE 77
	SKIP_REGULAR_EXPRESSION "SKIP:")

add_test(
	NAME pristine_lefdef_corpus
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/e2e/lefdef_corpus.py
		$<TARGET_FILE:pristine-engine>
		${PROJECT_SOURCE_DIR}/.deps/src/lefdef)
set_tests_properties(pristine_lefdef_corpus PROPERTIES
	LABELS "layout;lefdef"
	SKIP_RETURN_CODE 77
	SKIP_REGULAR_EXPRESSION "SKIP:")

add_test(
	NAME pristine_differential_slang_server
	COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/differential/slang_server_differential.py
		$<TARGET_FILE:pristine-engine>)
set_tests_properties(pristine_differential_slang_server PROPERTIES
	LABELS "differential"
	SKIP_REGULAR_EXPRESSION "SKIP:")

if(PRISTINE_BUILD_PERF_TESTS)
	add_executable(pristine_perf_tests perf/SemanticPerfSmoke.cpp)
	target_link_libraries(pristine_perf_tests PRIVATE pristine_core)
	add_test(NAME pristine_perf_tests COMMAND pristine_perf_tests)
	set_tests_properties(pristine_perf_tests PROPERTIES
		LABELS "perf;semantic;completion"
		TIMEOUT 1200)

	add_executable(pristine_wellen_fst_perf perf/WellenFstPerf.cpp)
	target_link_libraries(pristine_wellen_fst_perf PRIVATE pristine_core)
	add_test(
		NAME pristine_wellen_fst_perf
		COMMAND pristine_wellen_fst_perf
			${PROJECT_SOURCE_DIR}/.deps/src/wellen/wellen/inputs)
	set_tests_properties(pristine_wellen_fst_perf PROPERTIES
		LABELS "perf;waveform;fst")

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
		NAME pristine_rtl_e2e_large_workspace
		COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/perf/run_lsp_large_workspace_baseline.py
			$<TARGET_FILE:pristine_rtl_e2e_lsp_stress>
			$<TARGET_FILE:pristine-engine>
			${CMAKE_CURRENT_BINARY_DIR})
	set_tests_properties(pristine_rtl_e2e_large_workspace PROPERTIES
		LABELS "perf;rtl-e2e;lsp;large-workspace;debug-order"
		TIMEOUT 180)

	add_test(
		NAME pristine_rtl_e2e_real_retrosoc
		COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/perf/run_rtl_e2e_real_retrosoc.py
			$<TARGET_FILE:pristine_rtl_e2e_lsp_stress>
			$<TARGET_FILE:pristine-engine>
			${CMAKE_CURRENT_BINARY_DIR})
	set_tests_properties(pristine_rtl_e2e_real_retrosoc PROPERTIES
		LABELS "perf;rtl-e2e;lsp;real;retrosoc;required"
		TIMEOUT 240)

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
