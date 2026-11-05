set(VERB_PLUGIN_SOURCES PluginProcessor.cpp PluginEditor.cpp)

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
