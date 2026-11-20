# Tool tests (stage 2): models without a game, models against a real core System
# and live link server over the in-memory transport, the Tela UI (declarations,
# gestures, CPU-rendered pixels) and the UI / network thread interplay (TSan).
# The model tests need only the core (SND_BUILD_TOOL_CORE, no Tela, D-101); the UI
# tests and the executable's tests only exist with SND_BUILD_TOOL.
set(_tool_core_test_link sonus_tool sonus_livelink sonus_core sonus_buildtool sonus_bank sonus_codec
    sonus_dsp sonus_io sonus_jobs sonus_platform sonus_base sonus_mocks)
set(_tool_test_link sonus_tool_ui ${_tool_core_test_link})
function(_snd_tool_test name link)
  snd_add_test(tool_${name} SOURCES ${name}.cpp LINK ${link})
  target_include_directories(tool_${name} PRIVATE ${PROJECT_SOURCE_DIR}/core/test ${PROJECT_SOURCE_DIR}/tests/integration
                             ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(tool_${name} PROPERTIES TIMEOUT 600)
endfunction()

# Core only (no Tela). core_host_test uses the core the way an outside host does (D-101).
foreach(_t model_test live_mixer_test live_profiler_test indep_sync_test core_host_test document_test
        event_editor_test)
  _snd_tool_test(${_t} "${_tool_core_test_link}")
endforeach()

if(NOT SND_BUILD_TOOL)
  return()
endif()

foreach(_t ui_declaration_test ui_gesture_test ui_raster_test ui_concurrency_test ui_snapshot_test)
  _snd_tool_test(${_t} "${_tool_test_link}")
endforeach()

# The executable itself, headless, on the build tool's sample project (its WAV files are
# not in the repository: the build reports errors, the tool still shows the project).
add_test(NAME tool_app_headless
         COMMAND sonus-tool --headless --no-connect --frames 3 --frame-ms 1 --text-placeholders
                 --project ${PROJECT_SOURCE_DIR}/buildtool/test/data/sample
                 --build-dir ${CMAKE_CURRENT_BINARY_DIR}/sample-build
                 --snapshot ${CMAKE_CURRENT_BINARY_DIR}/headless-mixer.ppm)
add_test(NAME tool_app_bad_option COMMAND sonus-tool --size 10x10)
set_tests_properties(tool_app_bad_option PROPERTIES WILL_FAIL TRUE)

# Independent tests (tool/test/indep_properties.md, written from the specs before reading
# the code). Tests named Defect_* fail on purpose: each documents an implementation defect.
# (indep_sync_test needs only the core and is registered above.)
foreach(_t indep_model_test indep_ui_test indep_concurrency_test)
  _snd_tool_test(${_t} "${_tool_test_link}")
endforeach()
# --dpi accepts 0.25 .. 4 (D-67a): both ends run, just outside is refused.
add_test(NAME tool_indep_app_dpi_min
         COMMAND sonus-tool --headless --no-connect --frames 1 --frame-ms 0 --dpi 0.25 --screen profiler
                 --project ${PROJECT_SOURCE_DIR}/buildtool/test/data/sample
                 --build-dir ${CMAKE_CURRENT_BINARY_DIR}/indep-dpi-min-build
                 --snapshot ${CMAKE_CURRENT_BINARY_DIR}/indep-dpi-min.ppm)
add_test(NAME tool_indep_app_dpi_max
         COMMAND sonus-tool --headless --no-connect --frames 1 --frame-ms 0 --dpi 4 --size 4096x2048 --screen profiler
                 --project ${PROJECT_SOURCE_DIR}/buildtool/test/data/sample
                 --build-dir ${CMAKE_CURRENT_BINARY_DIR}/indep-dpi-max-build
                 --snapshot ${CMAKE_CURRENT_BINARY_DIR}/indep-dpi-max.ppm)
add_test(NAME tool_indep_app_dpi_above COMMAND sonus-tool --headless --no-connect --dpi 4.01)
add_test(NAME tool_indep_app_dpi_below COMMAND sonus-tool --headless --no-connect --dpi 0.24)
set_tests_properties(tool_indep_app_dpi_above tool_indep_app_dpi_below PROPERTIES WILL_FAIL TRUE)
