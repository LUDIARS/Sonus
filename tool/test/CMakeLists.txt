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
        event_editor_test mixer_editor_test curve_editor_test
        spatial_preview_test runtime_compare_test music_editor_test
        material_editor_test patch_editor_test decisions_test model_fix_test)
  _snd_tool_test(${_t} "${_tool_core_test_link}")
endforeach()

# Group P (live reflection, preview engine, auditions, item revert, profiler streams, replay).
# The preview engine and the replay host are in Sonus::tool_preview (tool/preview).
set(_tool_live_link sonus_tool_preview ${_tool_core_test_link})
foreach(_t live_preview_test live_sync_test live_revert_test profiler_streams_test replay_test
        live_concurrency_test live_v10_test)
  _snd_tool_test(${_t} "${_tool_live_link}")
endforeach()
# The 1.0 game of live_v10_test is the live link tests' hand-driven game (livelink/test).
target_include_directories(tool_live_v10_test PRIVATE ${PROJECT_SOURCE_DIR}/livelink/test)
# sonus-replay on the bug report replay_test writes (FIXTURES: replay_test first).
set_tests_properties(tool_replay_test PROPERTIES FIXTURES_SETUP tool_replay_report
                     ENVIRONMENT "SND_TOOL_REPLAY_REPORT_DIR=${CMAKE_CURRENT_BINARY_DIR}/replay-report")
add_test(NAME tool_replay_cli COMMAND sonus-replay ${CMAKE_CURRENT_BINARY_DIR}/replay-report)
set_tests_properties(tool_replay_cli PROPERTIES FIXTURES_REQUIRED tool_replay_report)
add_test(NAME tool_replay_cli_bad_input COMMAND sonus-replay ${CMAKE_CURRENT_BINARY_DIR}/no-such-report)
set_tests_properties(tool_replay_cli_bad_input PROPERTIES WILL_FAIL TRUE)

# Independent tests of group P (tool/test/indep_s7_live_properties.md, written from the specs
# before reading the code). The Defect_* tests are in their own executable, one CTest test each;
# they fail on purpose (each documents a defect).
foreach(_t indep_s7_live_sync_test indep_s7_live_preview_test indep_s7_live_replay_test indep_s7_live_threads_test indep_s7_live_revert_test
        indep_s7_live_recheck_test)
  _snd_tool_test(${_t} "${_tool_live_link}")
endforeach()
_snd_tool_test(indep_s7_live_defects_test "${_tool_live_link}")
set_tests_properties(tool_indep_s7_live_defects_test PROPERTIES DISABLED TRUE)  # run per defect below
foreach(_d Defect_ReconnectKeepsCellsTheToolPutBackWhileOffline Defect_ResentBusyValueOvertakesANewerValue
        Defect_ReserveStaysTakenAfterDocumentUndoOfALiveEffect Defect_PhaseIsLiveAfterAFailedReload
        Defect_ReloadWriterFailureIsNotAFailedPhase Defect_BugReportRoundTripLosesNotesAndComparison
        Defect_ConfigLineAcceptsNegativeAndOutOfRangeIntegers Defect_TransitionAuditionNearABarLineDisagreesWithThePreview
        Defect_TransitionAuditionAtTimeZeroSkipsTheFromState)
  add_test(NAME tool_indep_s7_live_${_d} COMMAND tool_indep_s7_live_defects_test --filter ${_d})
endforeach()
# sonus-replay's exit status on the reports indep_s7_live_replay_test leaves (0 ok, 1 mismatch,
# 2 input error).
set(_indep_live_reports ${CMAKE_CURRENT_BINARY_DIR}/indep-live-reports)
set_tests_properties(tool_indep_s7_live_replay_test PROPERTIES FIXTURES_SETUP tool_indep_live_reports
                     ENVIRONMENT "SND_INDEP_LIVE_REPORTS=${_indep_live_reports}")
function(_snd_indep_live_cli name expect)
  add_test(NAME tool_indep_s7_live_cli_${name}
           COMMAND ${CMAKE_COMMAND} -DCMD=$<TARGET_FILE:sonus-replay> "-DARGS=${ARGN}" -DEXPECT=${expect}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/indep_s7_live_exit.cmake)
endfunction()
_snd_indep_live_cli(ok 0 ${_indep_live_reports}/ok)
_snd_indep_live_cli(mismatch 1 ${_indep_live_reports}/mismatch)
_snd_indep_live_cli(broken 2 ${_indep_live_reports}/broken)
_snd_indep_live_cli(missing 2 ${_indep_live_reports}/missing)
_snd_indep_live_cli(replayfail 2 ${_indep_live_reports}/replayfail)
set_tests_properties(tool_indep_s7_live_cli_ok tool_indep_s7_live_cli_mismatch tool_indep_s7_live_cli_broken
                     tool_indep_s7_live_cli_missing tool_indep_s7_live_cli_replayfail PROPERTIES FIXTURES_REQUIRED tool_indep_live_reports)
_snd_indep_live_cli(usage 2 --recording)
_snd_indep_live_cli(no_recording 2 --recording ${_indep_live_reports}/none.snrec --banks ${_indep_live_reports})
# A recording with absurd block numbers must not replay for ever (POSIX: the test forks).
if(UNIX)
  _snd_tool_test(indep_s7_live_hang_test "${_tool_live_link}")
  set_tests_properties(tool_indep_s7_live_hang_test PROPERTIES DISABLED TRUE)  # per test below
  add_test(NAME tool_indep_s7_live_hang_small COMMAND tool_indep_s7_live_hang_test --filter SmallBlockNumbers)
  add_test(NAME tool_indep_s7_live_Defect_ReplayOfAHugeBlockNumberDoesNotEnd
           COMMAND tool_indep_s7_live_hang_test --filter Defect_ReplayOfAHugeBlockNumberDoesNotEnd)
  set_tests_properties(tool_indep_s7_live_Defect_ReplayOfAHugeBlockNumberDoesNotEnd PROPERTIES TIMEOUT 300)
endif()

# The runtime comparisons read the music module's time line and rule lookup (music/src).
target_link_libraries(tool_runtime_compare_test PRIVATE sonus_music)
target_include_directories(tool_runtime_compare_test PRIVATE ${PROJECT_SOURCE_DIR}/music/src)

# Independent tests of group M (tool/test/indep_s7_model_properties.md, written from the
# specs before reading the code). Core only. The Defect_* tests are in their own
# executable, one CTest test each; they fail on purpose (each documents a defect).
foreach(_t indep_s7_document_test indep_s7_editors_test indep_s7_compute_test indep_s7_build_test
        indep_s7_robust_test)
  _snd_tool_test(${_t} "${_tool_core_test_link}")
endforeach()
# Re-check after D-176..D-180: the recheck test compares with music's Timeline (music/src);
# the threads test is the D-178d concurrency test (TSan, repeat 20).
_snd_tool_test(indep_s7_recheck_test "${_tool_core_test_link}")
target_link_libraries(tool_indep_s7_recheck_test PRIVATE sonus_music)
target_include_directories(tool_indep_s7_recheck_test PRIVATE ${PROJECT_SOURCE_DIR}/music/src)
_snd_tool_test(indep_s7_threads_test "${_tool_core_test_link}")
_snd_tool_test(indep_s7_defects_test "${_tool_core_test_link}")
set_tests_properties(tool_indep_s7_defects_test PROPERTIES DISABLED TRUE)  # run per defect below
foreach(_d Defect_NegativeZeroLosesItsSign Defect_SetSameValueReturnsFalse
        Defect_StemChannelsOutsideTheMaterialAccepted
        Defect_LocateFieldMissesMusicSegmentAndDistanceCurveMembers Defect_LocateFieldLiveWritableForAbsentColumn
        Defect_SaveIgnoresAFailedDeletion Defect_EmptyMaterialTableBuildIsUndefinedBehaviour
        Defect_SameValueSettersRewriteExplicitDefaults Defect_SaveToSameDirectoryOtherSpellingLeavesStaleFiles)
  add_test(NAME tool_indep_s7_${_d} COMMAND tool_indep_s7_defects_test --filter ${_d})
endforeach()

if(NOT SND_BUILD_TOOL)
  return()
endif()

foreach(_t ui_declaration_test ui_gesture_test ui_raster_test ui_concurrency_test ui_snapshot_test)
  _snd_tool_test(${_t} "${_tool_test_link}")
endforeach()

# Group U: the stage 5 screens (declarations, gestures, CPU-rendered pixels, large projects). The
# replay and preview screens use the real replay service and preview engine (Sonus::tool_preview).
set(_tool_ui_screen_link sonus_tool_ui_replay sonus_tool_preview ${_tool_test_link})
foreach(_t ui_event_screen_test ui_buses_screen_test ui_params_screen_test ui_spatial_screen_test ui_music_screen_test ui_material_screen_test ui_patch_screen_test ui_live_screens_test ui_editor_limits_test ui_editor_input_test ui_editor_concurrency_test ui_editor_raster_test
        ui_editor_fix_test)
  _snd_tool_test(${_t} "${_tool_ui_screen_link}")
endforeach()

# The executable itself, headless, on the build tool's sample project (its WAV files are
# not in the repository: the build reports errors, the tool still shows the project).
add_test(NAME tool_app_headless
         COMMAND sonus-tool --headless --no-connect --frames 3 --frame-ms 1 --text-placeholders
                 --project ${PROJECT_SOURCE_DIR}/buildtool/test/data/sample
                 --build-dir ${CMAKE_CURRENT_BINARY_DIR}/sample-build
                 --snapshot ${CMAKE_CURRENT_BINARY_DIR}/headless-mixer.ppm)
# The limits per screen, each in its own process (a Tela exception would end the process: the
# test's name says which screen), down to 320 x 240 logical pixels.
foreach(_screen mixer buses event params spatial music material patch profiler replay link problems)
  add_test(NAME tool_ui_editor_limits_${_screen}
           COMMAND tool_ui_editor_limits_test --filter EveryScreenOfALargeProjectAtEveryDpi)
  set_tests_properties(tool_ui_editor_limits_${_screen} PROPERTIES ENVIRONMENT "SND_UI_LIMITS_SCREEN=${_screen}" TIMEOUT 1200)
endforeach()

# Every screen, headless, on the sample project (group U): each writes its image.
foreach(_screen mixer buses event params spatial music material patch profiler replay link problems)
  add_test(NAME tool_app_screen_${_screen}
           COMMAND sonus-tool --headless --no-connect --frames 2 --frame-ms 1 --text-placeholders --screen ${_screen}
                   --project ${PROJECT_SOURCE_DIR}/buildtool/test/data/sample
                   --build-dir ${CMAKE_CURRENT_BINARY_DIR}/screen-${_screen}-build
                   --snapshot ${CMAKE_CURRENT_BINARY_DIR}/screen-${_screen}.ppm)
endforeach()
add_test(NAME tool_app_bad_screen COMMAND sonus-tool --headless --screen nope)
set_tests_properties(tool_app_bad_screen PROPERTIES WILL_FAIL TRUE)
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

# Independent tests of group U (tool/test/indep_s7_ui_properties.md, written from the specs and
# the public headers before reading the screens' code). The Defect_* tests are in their own
# executable, one CTest test each; they fail on purpose (each documents a defect).
foreach(_t indep_s7_ui_common_test indep_s7_ui_limits_test indep_s7_ui_screens_test indep_s7_ui_gesture_test indep_s7_ui_raster_test indep_s7_ui_threads_test)
  _snd_tool_test(${_t} "${_tool_ui_screen_link}")
endforeach()
# I1 / I2: every screen headless on an empty and a large generated project (the limits test writes
# them first: FIXTURES), and an unknown screen name.
add_test(NAME tool_indep_s7_ui_write_projects COMMAND tool_indep_s7_ui_limits_test --filter WriteLargeProjectForHeadless)
set_tests_properties(tool_indep_s7_ui_write_projects PROPERTIES FIXTURES_SETUP tool_indep_s7_ui_projects
                     ENVIRONMENT "SND_INDEP_LARGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/indep-ui-large")
foreach(_screen mixer buses event params spatial music material patch profiler replay link problems)
  foreach(_proj large empty)
    if(_proj STREQUAL "large")
      set(_dir ${CMAKE_CURRENT_BINARY_DIR}/indep-ui-large)
    else()
      set(_dir ${CMAKE_CURRENT_BINARY_DIR}/indep-ui-large-empty)
    endif()
    add_test(NAME tool_indep_s7_ui_screen_${_proj}_${_screen}
             COMMAND ${CMAKE_COMMAND} -E env
                     sh -c "rm -f \"$2\" && \"$0\" --headless --no-connect --frames 2 --frame-ms 1 --text-placeholders --screen $1 --project \"$3\" --build-dir \"$4\" --snapshot \"$2\" && test -s \"$2\""
                     $<TARGET_FILE:sonus-tool> ${_screen} ${CMAKE_CURRENT_BINARY_DIR}/indep-ui-${_proj}-${_screen}.ppm
                     ${_dir} ${CMAKE_CURRENT_BINARY_DIR}/indep-ui-${_proj}-${_screen}-build)
    set_tests_properties(tool_indep_s7_ui_screen_${_proj}_${_screen} PROPERTIES FIXTURES_REQUIRED tool_indep_s7_ui_projects)
  endforeach()
endforeach()
foreach(_bad "" "Mixer" "mixer2" "--snapshot")
  string(MAKE_C_IDENTIFIER "x${_bad}" _n)
  add_test(NAME tool_indep_s7_ui_bad_screen_${_n} COMMAND sonus-tool --headless --no-connect --frames 1 --screen "${_bad}")
  set_tests_properties(tool_indep_s7_ui_bad_screen_${_n} PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME tool_indep_s7_ui_screen_missing_name COMMAND sonus-tool --headless --no-connect --frames 1 --screen)
set_tests_properties(tool_indep_s7_ui_screen_missing_name PROPERTIES WILL_FAIL TRUE)
set_tests_properties(tool_indep_s7_ui_limits_test PROPERTIES TIMEOUT 3600)
_snd_tool_test(indep_s7_ui_defects_test "${_tool_ui_screen_link}")
set_tests_properties(tool_indep_s7_ui_defects_test PROPERTIES DISABLED TRUE)  # run per test below
foreach(_d Defect_TypedNumberOutsideTheFieldRangeIsClampedNotRefused Defect_NumberFieldWheelNotchesBetweenFramesAreLost
        Defect_PickWithAnItemOfTheOpenListReadsFreedMemory
        Defect_ClipVerticalDragTrackComesFromThePressPointNotTheMotion Defect_NumberFieldValueHasNoRoom
        WriteBackAfterStaleMixerKeepsNewerDocumentValues SpecGap_StructuralEditDropsLiveOverridesSilently
        SpecGap_ClipSelectionIsAnIndexAcrossUndo SpecGap_UndoKeyDuringATextEntryUndoesTheDocument)
  add_test(NAME tool_indep_s7_ui_${_d} COMMAND tool_indep_s7_ui_defects_test --filter ${_d})
endforeach()
