set(VERB_PLUGIN_SOURCES PluginProcessor.cpp PluginEditor.cpp EnergyMapView.cpp)

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
    VST3_CATEGORIES Fx Reverb Spatial)

target_sources(VerbPlugin PRIVATE ${VERB_PLUGIN_SOURCES})
target_compile_definitions(VerbPlugin PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
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

# Stage 2 regression fixtures and DSP verification (all artifacts stay in the development tree).
add_executable(stage2_test ../src/stage2_test.cpp)
target_link_libraries(stage2_test PRIVATE verbcore)
add_executable(stage2_fixes_test ../src/stage2_fixes_test.cpp)
target_link_libraries(stage2_fixes_test PRIVATE verbcore)
add_test(NAME stage2_fixes COMMAND stage2_fixes_test)
set_tests_properties(stage2_fixes PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 3600)
add_executable(tail_transition_test ../src/tail_transition_test.cpp)
target_link_libraries(tail_transition_test PRIVATE verbcore)
add_test(NAME tail_transition COMMAND tail_transition_test)
set_tests_properties(tail_transition PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 900)
add_test(NAME stage2_engine COMMAND stage2_test)
add_test(NAME stage2_processor COMMAND VerbPluginTest)
set_tests_properties(stage2_processor PROPERTIES ENVIRONMENT "VERB_STAGE2_ONLY=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 900)
set_tests_properties(stage2_engine PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 900)
