# fa_core: rational time, frame rates, image buffer (no dependencies)
add_library(fa_core STATIC core/rational.cpp core/time.cpp core/app_data.cpp)
target_include_directories(fa_core PUBLIC core/include)
# fa/core/version.h: generated from the project version (editor/CMakeLists.txt)
configure_file(core/version.h.in "${CMAKE_BINARY_DIR}/generated/fa/core/version.h" @ONLY)
target_include_directories(fa_core PUBLIC "${CMAKE_BINARY_DIR}/generated")

# fa_anim: Animatable<T> (header-only, kept as an INTERFACE target for clarity)
add_library(fa_anim INTERFACE)
target_include_directories(fa_anim INTERFACE anim/include)
target_link_libraries(fa_anim INTERFACE fa_core)

# fa_model: project/timeline model, JSON serialization, undoable commands
add_library(fa_model STATIC model/project.cpp model/motion_presets.cpp model/time_map.cpp model/stabilize_plan.cpp model/serialize.cpp model/commands.cpp model/edit_ops.cpp model/keyframes.cpp
  model/subtitles.cpp)
target_include_directories(fa_model PUBLIC model/include)
target_link_libraries(fa_model PUBLIC fa_core fa_anim PRIVATE nlohmann_json::nlohmann_json)

# fa_media: FFmpeg wrappers (probe, decode, encode/mux). FFmpeg types never leak into headers.
add_library(fa_media STATIC media/probe.cpp media/video_decoder.cpp media/audio_decoder.cpp media/video_encoder.cpp media/export_format.cpp
  media/hdr.cpp)
target_include_directories(fa_media PUBLIC media/include)
target_include_directories(fa_media SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})   # no warnings from FFmpeg headers
target_link_directories(fa_media PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(fa_media PUBLIC fa_core PRIVATE ${FFMPEG_LIBRARIES})
if(WIN32)
  target_link_libraries(fa_media PRIVATE dxgi)   # adapter name of the D3D11VA decoder
endif()

# fa_vision: computer vision on decoded pictures (M5: optical-flow frame interpolation, tracking, stabilisation).
# OpenCV (Apache-2.0) stays private: no OpenCV type appears in its headers.
add_library(fa_vision STATIC vision/flow.cpp vision/track.cpp vision/stabilize.cpp vision/scene_detect.cpp)
target_include_directories(fa_vision PUBLIC vision/include)
target_link_libraries(fa_vision PUBLIC fa_core PRIVATE fa_media opencv_core opencv_imgproc opencv_video opencv_calib3d
  opencv_features2d)

# fa_render: timeline evaluation, CPU and GPU (D3D11) compositing backends, headless render pipeline
add_library(fa_render STATIC render/timeline_eval.cpp render/compositing.cpp render/cpu_backend.cpp render/transitions.cpp render/text_animation.cpp
  render/backend_select.cpp render/audio_mix.cpp render/renderer.cpp
  render/effects_registry.cpp render/effects_cpu.cpp render/curves.cpp render/lut.cpp render/audio_retime.cpp
  render/audio_dsp.cpp render/audio_cleanup.cpp render/matte_cache.cpp render/frame_store.cpp)
target_include_directories(fa_render PUBLIC render/include)
target_link_libraries(fa_render PUBLIC fa_model fa_media PRIVATE fa_vision ZLIB::ZLIB)
if(WIN32)   # Direct3D 11 GPU compositor (system libraries; shaders are compiled at run time by d3dcompiler_47)
  target_sources(fa_render PRIVATE render/d3d11_backend.cpp)
  target_compile_definitions(fa_render PRIVATE FA_HAS_D3D11)
  target_link_libraries(fa_render PRIVATE d3d11 dxgi d3dcompiler)
endif()

# fa_ai: M7 local AI - model catalogue and integrity-checked installer, device probing, ONNX Runtime sessions (CPU /
# DirectML), ncnn (Vulkan) and whisper.cpp runners. No ONNX Runtime, ncnn or ggml type appears in its headers.
add_library(fa_ai STATIC ai/models.cpp ai/inference.cpp ai/matting.cpp ai/speech.cpp ai/captions.cpp ai/faces.cpp ai/reframe.cpp ai/interpolate.cpp ai/upscale.cpp ai/prepare.cpp)
target_include_directories(fa_ai PUBLIC ai/include)
target_link_libraries(fa_ai PUBLIC fa_core fa_render PRIVATE fa_vision onnxruntime ncnn whisper ZLIB::ZLIB winhttp bcrypt)

# fa_text: title/caption text engine (Qt GUI text stack: QTextLayout + HarfBuzz shaping, QPainter)
if(FA_BUILD_GUI)
  add_library(fa_text STATIC text/title_engine.cpp text/title_templates.cpp text/stickers.cpp)
  target_include_directories(fa_text PUBLIC text/include)
  target_link_libraries(fa_text PUBLIC fa_render Qt6::Gui)
endif()

# fa_playback: background preview rendering + audio playback/clock (no Qt). miniaudio is header-only.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
add_library(fa_playback STATIC playback/preview_renderer.cpp playback/audio_player.cpp playback/miniaudio_impl.cpp
  playback/frame_codec.cpp playback/render_cache.cpp playback/render_preview.cpp)
target_include_directories(fa_playback PUBLIC playback/include PRIVATE playback)
target_include_directories(fa_playback SYSTEM PRIVATE ${MINIAUDIO_INCLUDE_DIR})
target_link_libraries(fa_playback PUBLIC fa_render)

# fa_app: the Qt Widgets editor (document, panels, timeline, dialogs)
if(FA_BUILD_GUI)
  add_library(fa_app STATIC
    app/theme.h app/theme.cpp
    app/legacy_rename.h app/legacy_rename.cpp
    app/document.h app/document.cpp
    app/media_service.h app/media_service.cpp
    app/playback_controller.h app/playback_controller.cpp
    app/edit_controller.h app/edit_controller.cpp
    app/speed_panel.h app/speed_panel.cpp
    app/speed_ramp_editor.h app/speed_ramp_editor.cpp
    app/analysis_service.h app/analysis_service.cpp
    app/tracking_panel.h app/tracking_panel.cpp
    app/stabilization_panel.h app/stabilization_panel.cpp
    app/render_preview_controller.h app/render_preview_controller.cpp
    app/timeline_view.h app/timeline_view.cpp
    app/panels.h app/timeline_panel.cpp app/resource_panels.cpp app/preview_panel.cpp app/inspector_panel.cpp
    app/inspector_panel.h app/preview_canvas.h app/preview_canvas.cpp
    app/title_library.h app/title_library.cpp app/titles_panel.cpp
    app/title_inspector.h app/title_inspector.cpp
    app/effect_editor.h app/effect_editor.cpp
    app/color_panel.h app/color_panel.cpp app/color_presets.h app/color_presets.cpp app/lut_library.h app/lut_library.cpp app/effect_presets.h app/effect_presets.cpp
    app/effects_browser.h app/effects_browser.cpp
    app/level_meter.h app/level_meter.cpp
    app/dialogs.h app/dialogs.cpp
    app/model_manager.h app/model_manager.cpp
    app/cutout_panel.h app/cutout_panel.cpp
    app/speech_dialog.h app/speech_dialog.cpp
    app/scene_dialog.h app/scene_dialog.cpp
    app/reframe_dialog.h app/reframe_dialog.cpp
    app/upscale_panel.h app/upscale_panel.cpp
    app/autosave.h app/autosave.cpp
    app/library_pages.h app/library_pages.cpp app/templates.h app/templates.cpp
    app/animation_panel.h app/animation_panel.cpp
    app/main_window.h app/main_window.cpp)
  set_target_properties(fa_app PROPERTIES AUTOMOC ON)
  target_include_directories(fa_app PUBLIC app)
  target_link_libraries(fa_app PUBLIC fa_playback fa_text fa_ai fa_vision Qt6::Widgets)
endif()
