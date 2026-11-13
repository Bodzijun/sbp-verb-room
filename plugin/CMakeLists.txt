set(VERB_PLUGIN_SOURCES PluginProcessor.cpp PluginEditor.cpp EnergyMapView.cpp EditorSession.cpp SceneCommands.cpp WebParameterBindings.cpp EditorBridge.cpp)

# ---- Opt-in WebView2 editor shell (task T1, docs/WebView_build_uk.md) -------------------------------------------------
# VERB_WEB_UI=ON builds the WebView editor code into the VST3 (Windows only). The native editor stays the default; the
# web editor is chosen at run time (env VERB_UI=web or the stored preference). VERB_WEB_UI=OFF builds exactly as before.
option(VERB_WEB_UI "Build the opt-in WebView2 editor shell into the plugin (Windows only)" ON)
set(VERB_WEBVIEW2_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/webview2" CACHE PATH
    "Directory that contains the unpacked Microsoft.Web.WebView2.<version> NuGet package")
set(VERB_WEB_DIR "${CMAKE_SOURCE_DIR}/web-ui" CACHE PATH
    "Directory embedded into the plugin as the web UI (T1: the shell page; later the built web-ui)")
set(VERB_WEB_UI_ACTIVE OFF)
if(VERB_WEB_UI AND WIN32)
  file(GLOB VERB_WEBVIEW2_HEADERS "${VERB_WEBVIEW2_SDK_DIR}/*Microsoft.Web.WebView2*/build/native/include/WebView2.h")
  if(VERB_WEBVIEW2_HEADERS)
    set(VERB_WEB_UI_ACTIVE ON)
    set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${VERB_WEBVIEW2_SDK_DIR}")
    message(STATUS "Verb Room: WebView2 editor shell enabled (SDK in ${VERB_WEBVIEW2_SDK_DIR})")
  else()
    message(WARNING "VERB_WEB_UI is ON but the WebView2 SDK was not found in ${VERB_WEBVIEW2_SDK_DIR} "
                    "(expected Microsoft.Web.WebView2.<version>/build/native/include/WebView2.h). The web editor is NOT built. "
                    "See docs/WebView_build_uk.md for how to obtain the NuGet package, or pass -DVERB_WEB_UI=OFF.")
  endif()
endif()

set(VERB_WEB_SOURCES "")
set(VERB_WEBVIEW_PLUGIN_ARGS "")
if(VERB_WEB_UI_ACTIVE)
  file(GLOB_RECURSE VERB_WEB_FILES CONFIGURE_DEPENDS "${VERB_WEB_DIR}/src/*" "${VERB_WEB_DIR}/assets/*" "${VERB_WEB_DIR}/schema/*")
  list(APPEND VERB_WEB_FILES "${VERB_WEB_DIR}/index.html")
  set(VERB_WEB_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/WebEmbeddedFiles.cpp")
  add_custom_command(
      OUTPUT "${VERB_WEB_GENERATED}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
      COMMAND "${CMAKE_COMMAND}" "-DWEB_ROOT=${VERB_WEB_DIR}" "-DOUT_CPP=${VERB_WEB_GENERATED}"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateEmbedded.cmake"
      DEPENDS ${VERB_WEB_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/GenerateEmbedded.cmake"
      COMMENT "Embedding ${VERB_WEB_DIR} into the plugin")
  set(VERB_WEB_SOURCES VerbWebEditor.cpp WebResources.cpp WebRuntimeProbe.cpp "${VERB_WEB_GENERATED}")
  set(VERB_WEBVIEW_PLUGIN_ARGS NEEDS_WEBVIEW2 TRUE)
endif()
# Definitions shared by every target that contains the web editor (JUCE modules must see one consistent setting).
set(VERB_WEB_DEFINITIONS JUCE_WEB_BROWSER=1 JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1 VERB_WEB_UI_ENABLED=1
    "VERB_WEB_PLUGIN_VERSION=\"${PROJECT_VERSION}\"")

juce_add_plugin(VerbPlugin
    COMPANY_NAME "SBP"
    PLUGIN_MANUFACTURER_CODE Ntsh
    PLUGIN_CODE Vrb1
    FORMATS VST3
    PRODUCT_NAME "Verb Room"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    ${VERB_WEBVIEW_PLUGIN_ARGS}
    VST3_CATEGORIES Fx Reverb Spatial)

target_sources(VerbPlugin PRIVATE ${VERB_PLUGIN_SOURCES} ${VERB_WEB_SOURCES})
target_compile_definitions(VerbPlugin PUBLIC
    JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
if(VERB_WEB_UI_ACTIVE)
  target_compile_definitions(VerbPlugin PUBLIC ${VERB_WEB_DEFINITIONS})
  target_include_directories(VerbPlugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
else()
  target_compile_definitions(VerbPlugin PUBLIC JUCE_WEB_BROWSER=0)
endif()
target_link_libraries(VerbPlugin PRIVATE
    verbcore
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)

# Stress / robustness test (no plugin host needed)
juce_add_console_app(VerbPluginStress PRODUCT_NAME "VerbPluginStress")
target_sources(VerbPluginStress PRIVATE PluginStress.cpp ${VERB_PLUGIN_SOURCES})
target_compile_definitions(VerbPluginStress PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(VerbPluginStress PRIVATE verbcore juce::juce_audio_utils juce::juce_recommended_config_flags)

# Headless processor test (no plugin host needed)
juce_add_console_app(VerbPluginTest PRODUCT_NAME "VerbPluginTest")
target_sources(VerbPluginTest PRIVATE PluginTest.cpp ${VERB_PLUGIN_SOURCES})
target_compile_definitions(VerbPluginTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(VerbPluginTest PRIVATE
    verbcore
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)

add_test(NAME portable_presets COMMAND VerbPluginTest)
add_test(NAME spatial_editor COMMAND VerbPluginTest)
add_test(NAME energy_editor COMMAND VerbPluginTest)
set_tests_properties(energy_editor PROPERTIES
    ENVIRONMENT "VERB_ENERGY_ONLY=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 360)
set_tests_properties(spatial_editor PROPERTIES
    ENVIRONMENT "VERB_SPATIAL_EDITOR_ONLY=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 360)
set_tests_properties(portable_presets PROPERTIES
    ENVIRONMENT "VERB_PRESETS_ONLY=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 240)
add_test(NAME vsl_hall_presets COMMAND VerbPluginTest)
set_tests_properties(vsl_hall_presets PROPERTIES
    ENVIRONMENT "VERB_VSL_HALL_ONLY=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 360)

# Command model / undo test (fast: tiny rooms, no long traces).
juce_add_console_app(VerbEditorCommandTest PRODUCT_NAME "VerbEditorCommandTest")
target_sources(VerbEditorCommandTest PRIVATE tests/EditorCommandTest.cpp ${VERB_PLUGIN_SOURCES})
target_compile_definitions(VerbEditorCommandTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(VerbEditorCommandTest PRIVATE
    verbcore
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)
add_test(NAME editor_commands COMMAND VerbEditorCommandTest)
set_tests_properties(editor_commands PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 240)

# Stage 2 regression fixtures and DSP verification (all artifacts stay in the development tree).
add_executable(stage2_test ../src/stage2_test.cpp)
target_link_libraries(stage2_test PRIVATE verbcore)
add_executable(stage2_fixes_test ../src/stage2_fixes_test.cpp)
target_link_libraries(stage2_fixes_test PRIVATE verbcore)
add_test(NAME stage2_fixes COMMAND stage2_fixes_test)
set_tests_properties(stage2_fixes PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 2378)
# Explicit release-only suite: ctest --test-dir build-msvc -C Exhaustive -R ^stage2_fixes_full$ --output-on-failure
# CONFIGURATIONS keeps it out of ordinary Release/default ctest (a label alone would not).
add_test(NAME stage2_fixes_full COMMAND stage2_fixes_test CONFIGURATIONS Exhaustive)
set_tests_properties(stage2_fixes_full PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "VERB_STAGE2_ROOMS=all" LABELS "long" TIMEOUT 2378)
add_executable(tail_transition_test ../src/tail_transition_test.cpp)
target_link_libraries(tail_transition_test PRIVATE verbcore)
add_test(NAME tail_transition COMMAND tail_transition_test)
set_tests_properties(tail_transition PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 900)
add_test(NAME stage2_engine COMMAND stage2_test)
add_test(NAME stage2_processor COMMAND VerbPluginTest)
set_tests_properties(stage2_processor PROPERTIES ENVIRONMENT "VERB_STAGE2_ONLY=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 900)
set_tests_properties(stage2_engine PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 900)

# WebView shell test (T1): resource provider, handshake, fallback, lifecycle. Built only with the web editor.
if(VERB_WEB_UI_ACTIVE)
  juce_add_console_app(VerbWebShellTest PRODUCT_NAME "VerbWebShellTest" NEEDS_WEBVIEW2 TRUE)
  # Preserve all T1 assertions against the original shell, independently of the production frontend.
  set(VERB_SHELL_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/ShellEmbeddedFiles.cpp")
  file(GLOB_RECURSE VERB_SHELL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/webshell/*")
  add_custom_command(OUTPUT "${VERB_SHELL_GENERATED}"
    COMMAND "${CMAKE_COMMAND}" "-DWEB_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/webshell" "-DOUT_CPP=${VERB_SHELL_GENERATED}" -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateEmbedded.cmake"
    DEPENDS ${VERB_SHELL_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/GenerateEmbedded.cmake")
  target_sources(VerbWebShellTest PRIVATE tests/WebShellTest.cpp ${VERB_PLUGIN_SOURCES} VerbWebEditor.cpp WebResources.cpp WebRuntimeProbe.cpp "${VERB_SHELL_GENERATED}")
  target_compile_definitions(VerbWebShellTest PRIVATE ${VERB_WEB_DEFINITIONS} JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
  target_include_directories(VerbWebShellTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(VerbWebShellTest PRIVATE
      verbcore
      juce::juce_audio_utils
      juce::juce_recommended_config_flags)
  add_test(NAME web_shell COMMAND VerbWebShellTest)
  set_tests_properties(web_shell PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 240)

  # Optional smoke test of the BUILT VST3 as a host (not in "all", not in CTest):
  #   cmake --build build-msvc --config Release --target VerbWebVst3Smoke
  #   build-msvc/plugin/VerbWebVst3Smoke_artefacts/Release/VerbWebVst3Smoke.exe "<path to Verb Room.vst3>" web
  juce_add_console_app(VerbWebVst3Smoke PRODUCT_NAME "VerbWebVst3Smoke")
  set_target_properties(VerbWebVst3Smoke PROPERTIES EXCLUDE_FROM_ALL TRUE)
  target_sources(VerbWebVst3Smoke PRIVATE tests/WebVst3Smoke.cpp)
  target_compile_definitions(VerbWebVst3Smoke PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(VerbWebVst3Smoke PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()

# T4 JS-less bridge harness: same JSON entrypoint as the live WebView.
juce_add_console_app(VerbWebBridgeTest PRODUCT_NAME "VerbWebBridgeTest" ${VERB_WEBVIEW_PLUGIN_ARGS})
target_sources(VerbWebBridgeTest PRIVATE tests/WebBridgeTest.cpp ${VERB_PLUGIN_SOURCES} ${VERB_WEB_SOURCES})
if(VERB_WEB_UI_ACTIVE)
  target_compile_definitions(VerbWebBridgeTest PRIVATE ${VERB_WEB_DEFINITIONS})
  target_include_directories(VerbWebBridgeTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
else()
  target_compile_definitions(VerbWebBridgeTest PRIVATE JUCE_WEB_BROWSER=0)
endif()
target_compile_definitions(VerbWebBridgeTest PRIVATE JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(VerbWebBridgeTest PRIVATE verbcore juce::juce_audio_utils juce::juce_recommended_config_flags)
add_test(NAME web_bridge COMMAND VerbWebBridgeTest)
set_tests_properties(web_bridge PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 120)
add_test(NAME e1_processor COMMAND VerbPluginTest)
set_tests_properties(e1_processor PROPERTIES ENVIRONMENT "VERB_E1_ONLY=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 600)
add_test(NAME e1_neutral_null COMMAND VerbPluginTest)
set_tests_properties(e1_neutral_null PROPERTIES ENVIRONMENT "VERB_E1_NULL=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 600)
