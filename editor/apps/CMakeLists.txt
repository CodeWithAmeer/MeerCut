# The Windows version resource of an executable (and the application icon for the editor): apps/version.rc.in.
# An optional fourth argument is the file name it is distributed under (the editor ships as MeerCut.exe).
function(fa_version_resource target description icon)
  set(FA_RC_NAME "${target}")
  set(FA_RC_FILE "${target}.exe")
  if(ARGC GREATER 3)
    set(FA_RC_FILE "${ARGV3}")
  endif()
  set(FA_RC_DESCRIPTION "${description}")
  set(FA_RC_ICON_LINE "")
  if(icon AND EXISTS "${icon}")
    set(FA_RC_ICON_LINE "IDI_ICON1 ICON \"${icon}\"")
  endif()
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc" @ONLY)
  target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
endfunction()

add_executable(fa-render render/main.cpp utf8.manifest)
fa_version_resource(fa-render "MeerCut headless renderer" "")
target_link_libraries(fa-render PRIVATE fa_render fa_ai)
if(TARGET fa_text)   # titles and captions need the Qt text engine
  target_link_libraries(fa-render PRIVATE fa_text)
  target_compile_definitions(fa-render PRIVATE FA_HAS_TEXT)
endif()

add_executable(fa-probe probe/main.cpp utf8.manifest)
fa_version_resource(fa-probe "MeerCut media and AI device probe" "")
target_link_libraries(fa-probe PRIVATE fa_media fa_ai nlohmann_json::nlohmann_json)

add_executable(fa-bench bench/main.cpp)
target_link_libraries(fa-bench PRIVATE fa_render fa_playback fa_vision nlohmann_json::nlohmann_json)
if(TARGET fa_text)   # M3 text engine benchmarks
  target_link_libraries(fa-bench PRIVATE fa_text)
  target_compile_definitions(fa-bench PRIVATE FA_HAS_TEXT)
endif()

install(TARGETS fa-render fa-probe RUNTIME DESTINATION bin)

if(FA_BUILD_GUI)
  add_executable(fa-editor WIN32 editor/main.cpp utf8.manifest)
  target_link_libraries(fa-editor PRIVATE fa_app)
  fa_version_resource(fa-editor "MeerCut video editor" "${CMAKE_CURRENT_SOURCE_DIR}/editor/meercut.ico" "MeerCut.exe")
  # the application icon is drawn by the editor itself; fa-icongen writes it as editor/meercut.ico (committed)
  add_executable(fa-icongen icongen/main.cpp)
  target_link_libraries(fa-icongen PRIVATE fa_app)
  # Qt loads its platform plugin (qwindows) at run time: copy it next to the executable.
  set(FA_QT_PLUGINS_REL "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}")
  set(FA_QT_PLUGINS_DBG "${QT6_INSTALL_PREFIX}/debug/${QT6_INSTALL_PLUGINS}")
  add_custom_command(TARGET fa-editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<IF:$<CONFIG:Debug>,${FA_QT_PLUGINS_DBG},${FA_QT_PLUGINS_REL}>/platforms"
            "$<TARGET_FILE_DIR:fa-editor>/platforms"
    COMMENT "Deploying Qt platform plugins next to fa-editor"
    VERBATIM)
  # M9: MeerCut's own library (audio) next to the program, as in the packages
  add_custom_command(TARGET fa-editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:fa-editor>/assets"
    COMMENT "Copying the library assets next to fa-editor"
    VERBATIM)
  install(TARGETS fa-editor RUNTIME DESTINATION bin)
endif()

# fa-ai-bench: M7 local AI benchmarks (inference time per frame, device, peak RAM)
add_executable(fa-ai-bench ai_bench/main.cpp)
target_link_libraries(fa-ai-bench PRIVATE fa_ai fa_vision fa_render nlohmann_json::nlohmann_json psapi dxgi)
target_compile_definitions(fa-ai-bench PRIVATE FA_MODELS_DIR="${FA_MODELS_DIR}")
