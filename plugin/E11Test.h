#include "EditorBridge.h"
static int testE11Processor() {
    VerbProcessor p;p.setPlayConfigDetails(2,2,48000,256);p.setTailSettings({8000,false});p.prepareToPlay(48000,256);
    check(waitReady(p,90),"E11 processor initial ready");
    const auto rooms=verbp::builtinRooms();const auto& room=rooms[0];
    const auto dirty=[&]{setP(p,"mode",1);setP(p,"absorption",1.4);setP(p,"decayLow",1.2);setP(p,"decayMid",1.3);setP(p,"decayHigh",1.4);setP(p,"mix",37);setP(p,"doppler",.7);};
    const auto neutral=[&]{for(const char* id:{"absorption","decayLow","decayMid","decayHigh"})check(std::abs(p.apvts.getRawParameterValue(id)->load()-1)<1e-5,"E11 per-room overrides neutral");check(std::abs(p.apvts.getRawParameterValue("mix")->load()-37)<.001&&std::abs(p.apvts.getRawParameterValue("doppler")->load()-.7)<.001,"E11 mix/doppler retained");};
    dirty();p.loadRoomWithSetup(room.text,room.name,room.setup,room.micPattern);check(waitReady(p,90),"E11 library room ready");neutral();
    dirty();p.loadSceneText(room.text,"E11 geometry.txt",true);check(waitReady(p,90),"E11 geometry room ready");neutral();
    dirty();const auto full=p.getRoomPresetText();juce::MemoryBlock state;p.getStateInformation(state);
    for(bool keep:{false,true}){p.setKeepRoutingEnabled(keep);setP(p,"absorption",.5);setP(p,"decayMid",.5);juce::String why;
        check(p.loadRoomPresetText(full,why)&&waitReady(p,90),"E11 full preset ready with Keep I/O on/off");
        check(std::abs(p.apvts.getRawParameterValue("absorption")->load()-1.4)<.001&&std::abs(p.apvts.getRawParameterValue("decayMid")->load()-1.3)<.001,"E11 full preset restores decay values");
        verbweb::EditorBridge bridge(p);const auto snapshot=verbweb::prop(bridge.snapshot(),"payload");const auto params=verbweb::prop(snapshot,"parameters");
        check(std::abs((double)verbweb::prop(verbweb::prop(params,"values"),"decayMid")-1.3)<.001,"E11 bridge knob follows full preset host parameter");
        check(std::abs(p.webAppliedAbsorption()-1.4)<.001,"E11 engine absorption agrees with full preset knob");
    }
    setP(p,"absorption",.5);p.setStateInformation(state.getData(),(int)state.getSize());check(waitReady(p,90)&&std::abs(p.apvts.getRawParameterValue("absorption")->load()-1.4)<.001,"E11 project state restore unchanged");
    p.editScene([](verb::Scene& s){s.ceilingInfinite=true;});const auto infinite=p.getRoomPresetText();juce::String why;
    check(p.loadRoomPresetText(infinite,why)&&waitReady(p,90)&&p.getDisplayScene().ceilingInfinite,"E11 full verbroom ceiling extent roundtrip");
    p.releaseResources();return fails?1:0;
}
