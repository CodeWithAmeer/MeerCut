include(GoogleTest)

get_filename_component(FA_REPO_ROOT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set(FA_TESTMEDIA_DIR "${FA_REPO_ROOT}/testmedia" CACHE PATH "Stage 1 test media directory")
set(FA_PYTHON "${FA_REPO_ROOT}/tools/.venv/Scripts/python.exe" CACHE FILEPATH "Python with numpy for e2e checks")

add_executable(fa_unit_tests
  unit/test_time.cpp
  unit/test_animatable.cpp
  unit/test_model.cpp
  unit/test_commands.cpp
  unit/test_render_eval.cpp
  unit/test_media.cpp
  unit/test_edit_ops.cpp
  unit/test_audio.cpp
  unit/test_playback.cpp
  unit/test_visual.cpp
  unit/test_gpu.cpp
  unit/test_subtitles.cpp
  unit/test_effects.cpp
  unit/test_lut.cpp
  unit/test_hdr.cpp
  unit/test_effects_library.cpp
  unit/test_time_map.cpp
  unit/test_retime_audio.cpp
  unit/test_flow.cpp
  unit/test_track.cpp
  unit/test_stabilize.cpp
  unit/test_render_cache.cpp
  unit/test_audio_dsp.cpp unit/test_audio_mix_m6.cpp unit/test_audio_cleanup.cpp unit/test_audio_ducking.cpp unit/test_export_formats.cpp
  unit/test_ai_infra.cpp unit/test_ai_cutout.cpp unit/test_ai_speech.cpp
  unit/test_scene_detect.cpp unit/test_ai_reframe.cpp
  unit/test_ai_enhance.cpp unit/test_ai_integration.cpp unit/test_s3_robustness.cpp
  unit/test_m9_editing.cpp unit/test_m9_library.cpp unit/test_m9_motion.cpp unit/test_m10_editing.cpp unit/test_m10_rename.cpp)
target_link_libraries(fa_unit_tests PRIVATE fa_playback fa_vision fa_ai ZLIB::ZLIB nlohmann_json::nlohmann_json
  GTest::gtest_main)
target_compile_definitions(fa_unit_tests PRIVATE FA_TESTMEDIA_DIR="${FA_TESTMEDIA_DIR}" FA_REPO_ROOT="${FA_REPO_ROOT}"
  FA_MODELS_DIR="${FA_MODELS_DIR}")
gtest_discover_tests(fa_unit_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)

# End-to-end: project -> fa-render -> MP4 -> ffprobe + per-frame index/colour verification.
if(EXISTS "${FA_PYTHON}")
  add_test(NAME e2e_media_setup
           COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/make_media.py" "${FA_TESTMEDIA_DIR}")
  set_tests_properties(e2e_media_setup PROPERTIES FIXTURES_SETUP e2e_media)
  foreach(case cut_30fps cut_2997fps mixed_2997_from_30 mixed_24_from_30 dissolve_30fps)
    add_test(NAME e2e_${case}
             COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_e2e.py" $<TARGET_FILE:fa-render> ${case}
                     "${CMAKE_BINARY_DIR}/e2e" --testmedia "${FA_TESTMEDIA_DIR}")
    set_tests_properties(e2e_${case} PROPERTIES FIXTURES_REQUIRED e2e_media TIMEOUT 600 LABELS e2e)
  endforeach()
  # Negative control: a render that is one source frame off after the cut must be rejected.
  add_test(NAME e2e_negative_control_off_by_one
           COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_e2e.py" $<TARGET_FILE:fa-render> cut_2997fps
                   "${CMAKE_BINARY_DIR}/e2e" --testmedia "${FA_TESTMEDIA_DIR}" --perturb)
  set_tests_properties(e2e_negative_control_off_by_one PROPERTIES FIXTURES_REQUIRED e2e_media TIMEOUT 600
                       LABELS e2e WILL_FAIL TRUE)
  # M2: transforms + keyframes, mask + blend mode, wipe transition; CPU and GPU backends; plus a negative control.
  add_test(NAME e2e_m2_visual
           COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/m2_visual_e2e.py" $<TARGET_FILE:fa-render>
                   "${CMAKE_BINARY_DIR}/e2e" --testmedia "${FA_TESTMEDIA_DIR}")
  set_tests_properties(e2e_m2_visual PROPERTIES FIXTURES_REQUIRED e2e_media TIMEOUT 600 LABELS e2e)
  add_test(NAME e2e_m2_negative_control_mask_offset
           COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/m2_visual_e2e.py" $<TARGET_FILE:fa-render>
                   "${CMAKE_BINARY_DIR}/e2e" --testmedia "${FA_TESTMEDIA_DIR}" --perturb)
  set_tests_properties(e2e_m2_negative_control_mask_offset PROPERTIES FIXTURES_REQUIRED e2e_media TIMEOUT 600
                       LABELS e2e WILL_FAIL TRUE)
  # M3: title + Type Writer, SRT burn-in through fa-render --subtitles; CPU and GPU; plus a negative control.
  add_test(NAME e2e_m3_text
           COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/m3_text_e2e.py" $<TARGET_FILE:fa-render>
                   "${CMAKE_BINARY_DIR}/e2e")
  set_tests_properties(e2e_m3_text PROPERTIES TIMEOUT 600 LABELS e2e)
  add_test(NAME e2e_m3_negative_control_caption_offset
           COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/m3_text_e2e.py" $<TARGET_FILE:fa-render>
                   "${CMAKE_BINARY_DIR}/e2e" --perturb)
  set_tests_properties(e2e_m3_negative_control_caption_offset PROPERTIES TIMEOUT 600 LABELS e2e WILL_FAIL TRUE)
  # M6: every export format checked with ffprobe / decoded frames / tones (EXP-007), plus a negative control.
  add_test(NAME e2e_m6_export
           COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/m6_export_e2e.py" $<TARGET_FILE:fa-render>
                   "${CMAKE_BINARY_DIR}/e2e" --testmedia "${FA_TESTMEDIA_DIR}")
  set_tests_properties(e2e_m6_export PROPERTIES TIMEOUT 1800 LABELS e2e)
  add_test(NAME e2e_m6_negative_control_untagged
           COMMAND "${FA_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/m6_export_e2e.py" $<TARGET_FILE:fa-render>
                   "${CMAKE_BINARY_DIR}/e2e" --negative-control)
  set_tests_properties(e2e_m6_negative_control_untagged PROPERTIES TIMEOUT 600 LABELS e2e WILL_FAIL TRUE)
else()
  message(WARNING "FA_PYTHON not found (${FA_PYTHON}); end-to-end tests are not registered")
endif()

# Text engine tests (M3): need a QGuiApplication, so they have their own main().
if(FA_BUILD_GUI)
  add_executable(fa_text_tests text/test_text.cpp text/test_stickers.cpp)
  target_link_libraries(fa_text_tests PRIVATE fa_text GTest::gtest)
  add_dependencies(fa_text_tests fa-editor)   # deploys the Qt platform plugin into bin/
  gtest_discover_tests(fa_text_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300 LABELS text)
endif()

# GUI integration tests (real MainWindow driven with QTest mouse/keyboard events).
if(FA_BUILD_GUI)
  add_executable(fa_gui_tests gui/test_gui.cpp)
  set_target_properties(fa_gui_tests PROPERTIES AUTOMOC ON)
  target_link_libraries(fa_gui_tests PRIVATE fa_app Qt6::Test)
  target_compile_definitions(fa_gui_tests PRIVATE FA_TESTMEDIA_DIR="${FA_TESTMEDIA_DIR}" FA_MODELS_DIR="${FA_MODELS_DIR}"
                             FA_REPO_ROOT="${FA_REPO_ROOT}")
  add_dependencies(fa_gui_tests fa-editor)   # deploys the Qt platform plugin into bin/
  # One CTest test per GUI test function (its own process, timeout and log): a slow machine does not time out the
  # whole suite, and a failure names its test. The functions are read from the GuiTests slot list.
  # Each result log is also written to <build>/logs/gui_<name>.txt: QTest flushes it per line, so a hard crash
  # still shows where it happened. The GUI tests drive one real window each: they run one at a time.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_gui.cpp")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_gui.cpp" _gui_slots REGEX "^    void [A-Za-z0-9_]+\\(\\);$")
  foreach(_line IN LISTS _gui_slots)
    string(REGEX REPLACE "^    void ([A-Za-z0-9_]+)\\(\\);$" "\\1" _fn "${_line}")
    add_test(NAME gui_${_fn} COMMAND fa_gui_tests ${_fn} -o ${CMAKE_BINARY_DIR}/logs/gui_${_fn}.txt,txt -o -,txt)
    # QTEST_FUNCTION_TIMEOUT: QtTest's own limit per function (300 s by default) follows the test's 900 s (M7: the
    # integration test prepares RIFE frames through ncnn's unoptimised Debug code)
    set_tests_properties(gui_${_fn} PROPERTIES TIMEOUT 900 LABELS gui RUN_SERIAL TRUE
                         ENVIRONMENT "FA_SCREENSHOT_DIR=${CMAKE_BINARY_DIR}/screenshots;QTEST_FUNCTION_TIMEOUT=900000")   # screenshots copied to docs/screenshots deliberately
  endforeach()
endif()
