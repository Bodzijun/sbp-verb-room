
static int testE12Meters(){
 for(int channels:{2,6,16}){VerbProcessor p;p.setPlayConfigDetails(channels,channels,48000,512);p.setTailSettings({8000,false});p.prepareToPlay(48000,512);check(waitReady(p,90),"E12 meters processor ready");setP(p,"spatialDry",0);setP(p,"mix",0);setP(p,"gain",-6);setP(p,"makeup",12);juce::AudioBuffer<float> buffer(channels,512);juce::MidiBuffer midi;
 auto feed=[&](float value,int blocks){for(int k=0;k<blocks;++k){for(int c=0;c<channels;++c)for(int i=0;i<512;++i)buffer.setSample(c,i,value);p.processBlock(buffer,midi);}};
 feed(.125f,250);const double expected=.125*std::pow(10.,12./20.);double worst=0;for(int c=0;c<channels;++c){double peak=p.takeWebOutputPeak(c),rms=p.outputRms(c);worst=std::max({worst,std::abs(20*std::log10(std::max(1e-30,peak)/expected)),std::abs(20*std::log10(std::max(1e-30,rms)/expected))});check(!p.outputClipped(c),"E12 unclipped known signal");}printf("E12_METERS channels%d max_error_db%.9f\n",channels,worst);check(worst<=.1,"E12 post gain/makeup peak and RMS within0.1dB");

 // The legacy raw dry bus bypasses wet Gain. Verify the metered wet bus too
 // against independently accumulated final PCM after both Gain and Makeup.
 setP(p,"mix",100);setP(p,"soloDirect",1);size_t phase=0;auto sine=[&](int blocks,double& squares,float& peak){for(int k=0;k<blocks;++k){for(int c=0;c<channels;++c)for(int i=0;i<512;++i)buffer.setSample(c,i,(float)(.1*std::sin(2*verb::kPi*(phase+i)/48.)));phase+=512;p.processBlock(buffer,midi);for(int i=0;i<512;++i){auto value=buffer.getSample(0,i);squares+=(double)value*value;peak=std::max(peak,std::abs(value));}}};
 double squares=0;float pcmPeak=0;sine(250,squares,pcmPeak);p.takeWebOutputPeak(0);squares=0;pcmPeak=0;sine(96,squares,pcmPeak);const double pcmRms=std::sqrt(squares/(96*512));
 check(pcmPeak>0&&std::abs(20*std::log10(p.takeWebOutputPeak(0)/pcmPeak))<=.1&&std::abs(20*std::log10(p.outputRms(0)/pcmRms))<=.1,"E12 wet Gain/Makeup meter agrees with independent final PCM within0.1dB");
 setP(p,"soloDirect",0);setP(p,"mix",0);feed(0,2);
 feed(1.2f,2);feed(0,250);check(p.outputClipped(0),"E12 clip persists after silence");p.clearOutputClip(0);check(!p.outputClipped(0)&&p.outputClipped(1),"E12 clicked clip clears one channel only");feed(1.2f,1);check(p.outputClipped(0),"E12 new overload relatches clip");
 if(channels==2){feed(.01f,200);double sum[2]={};double maximum[2]={};int overruns[2]={};for(int pair=0;pair<4000;++pair)for(int turn=0;turn<2;++turn){int enabled=(pair+turn)&1;p.setTestOutputMeteringEnabled(enabled!=0);auto start=std::chrono::steady_clock::now();feed(.01f,1);double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();sum[enabled]+=us;maximum[enabled]=std::max(maximum[enabled],us);overruns[enabled]+=us>512e6/48000.;}printf("E12_METER_COST pairs4000 baseline_us%.6f meters_us%.6f delta_pct%.6f max_us%.3f/%.3f overruns%d/%d\n",sum[0]/4000,sum[1]/4000,100*(sum[1]/sum[0]-1),maximum[0],maximum[1],overruns[0],overruns[1]);}
 }return fails?1:0;
}
