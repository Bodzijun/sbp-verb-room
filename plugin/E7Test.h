#pragma once
#include "EditorSession.h"
struct E7Gesture : juce::AudioProcessorParameter::Listener {
 int begin=0,end=0,changes=0;
 void parameterValueChanged(int,float) override {++changes;}
 void parameterGestureChanged(int,bool starting) override {if(starting)++begin;else ++end;}
};
static int testE7Makeup() {
 VerbProcessor p; p.setPlayConfigDetails(2,2,48000,128);p.prepareToPlay(48000,128);
 check(waitReady(p,90),"E7 processor ready");
 auto* makeup=p.apvts.getParameter("makeup"); auto* gain=p.apvts.getParameter("gain");
 check(makeup&&makeup->getParameterIndex()==249&&p.getParameters().size()==250,"Makeup appended at index249; all legacy indices retained");
 check(gain->getNormalisableRange().start==-36&&gain->getNormalisableRange().end==12&&makeup->getNormalisableRange().start==0&&makeup->getNormalisableRange().end==36,"Output range unchanged; Makeup 0..36");
 check(p.apvts.getRawParameterValue("makeup")->load()==0,"Makeup defaults to zero");
 setP(p,"makeup",18);juce::MemoryBlock saved;p.getStateInformation(saved);
 setP(p,"makeup",3);p.setStateInformation(saved.getData(),(int)saved.getSize());check(waitReady(p,90)&&p.apvts.getRawParameterValue("makeup")->load()==18,"project state roundtrip retains Makeup");
 auto xml=juce::AudioProcessor::getXmlFromBinary(saved.getData(),(int)saved.getSize());auto old=juce::ValueTree::fromXml(*xml);old.removeChild(old.getChildWithProperty("id","makeup"),nullptr);juce::MemoryBlock oldData;juce::AudioProcessor::copyXmlToBinary(*old.createXml(),oldData);
 p.setStateInformation(oldData.getData(),(int)oldData.getSize());check(waitReady(p,90)&&p.apvts.getRawParameterValue("makeup")->load()==0,"old state missing Makeup resets previous 18 to zero");
 setP(p,"makeup",24);auto preset=p.getRoomPresetText();juce::String why;
 setP(p,"makeup",0);check(p.loadRoomPresetText(preset,why)&&waitReady(p,90)&&std::abs(p.apvts.getRawParameterValue("makeup")->load()-24)<.001,"verbroom optional Makeup saves/reloads");
 auto px=juce::parseXML(preset);auto* state=px->getChildByName("VerbState");for(auto* child=state->getFirstChildElement();child;){auto* next=child->getNextElement();if(child->getStringAttribute("id")=="makeup")state->removeChildElement(child,true);child=next;}
 check(p.loadRoomPresetText(px->toString(),why)&&waitReady(p,90)&&p.apvts.getRawParameterValue("makeup")->load()==0,"old verbroom without Makeup resets to zero");
 // Settled complete model and one host gesture / one local undo entry.
 juce::MidiBuffer midi;juce::AudioBuffer<float> audio(2,128);
 for(int k=0;k<400;++k){audio.clear();p.processBlock(audio,midi);}
 float target=0;const bool estimated=p.autoMakeupDb(target);check(estimated&&std::isfinite(target)&&target>=0&&target<=36,"Auto computes finite clamped result from completed model");
 std::printf("  Auto initial %.6f dB\n",target);
 setP(p,"s1g",-12);float a=0,b=0;const bool gotA=p.autoMakeupDb(a);setP(p,"s1g",-18);const bool gotB=p.autoMakeupDb(b);
 check(gotA&&gotB&&a>0&&b<36&&std::abs((b-a)-6)<.002,"Auto energy formula tracks 6 dB source attenuation by +6 dB");
 setP(p,"s1mute",1);check(!p.autoMakeupDb(target),"Auto rejects no enabled sources");setP(p,"s1mute",0);setP(p,"s1g",0);p.autoMakeupDb(target);
 setP(p,"makeup",target<18?36:0);const float before=p.apvts.getRawParameterValue("makeup")->load();
 auto& session=p.getEditorSession();const int depth=session.undoDepth();E7Gesture listener;makeup->addListener(&listener);
 auto command=juce::JSON::parse(R"({"op":"output.autoMakeup"})");auto result=session.run(command);makeup->removeListener(&listener);
 check(result.ok&&std::abs(p.apvts.getRawParameterValue("makeup")->load()-target)<.002,"Auto applies exact estimate");
 check(listener.begin==1&&listener.end==1&&listener.changes==1&&session.undoDepth()==depth+1,"one begin/change/end and one undo entry");
 check(session.undo().ok&&p.apvts.getRawParameterValue("makeup")->load()==before,"Auto undo restores previous Makeup");
 setP(p,"lx",p.apvts.getRawParameterValue("lx")->load()+.1);audio.clear();p.processBlock(audio,midi);check(!p.autoMakeupDb(target),"Auto refused during movement");
 // Post-mix gain includes unpositioned dry even in Physical; output knob remains independent.
 setP(p,"spatialDry",0);setP(p,"mix",0);setP(p,"mode",0);setP(p,"makeup",0);
 for(int k=0;k<200;++k){audio.clear();p.processBlock(audio,midi);}
 audio.clear();audio.setSample(0,0,.01f);p.processBlock(audio,midi);const float baseline=audio.getSample(0,0);
 setP(p,"makeup",24);audio.clear();audio.setSample(0,0,.01f);p.processBlock(audio,midi);
 check(std::abs(audio.getSample(0,0)/baseline-std::pow(10.,24./20.))<1e-4,"Makeup applies after dry/wet in Physical mode");
 return fails?1:0;
}
