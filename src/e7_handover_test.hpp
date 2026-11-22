// E7 acceptance uses a deterministic dense periodic spectrum and paired final-pose
// control. 8192-sample windows are one whole signal period (170.67 ms).
struct E7OctaveMeter {
 double z[2][7][2][2]={};std::array<std::vector<double>,8> ring;std::array<double,8> sum{};size_t at=0;
 double b0[7],a1[7],a2[7];
 E7OctaveMeter(){for(auto& v:ring)v.assign(8192,0.);for(int b=0;b<7;++b){double w=2*verb::kPi*verb::kBandHz[b]/48000,al=sin(w)/(2*sqrt(2.)),a0=1+al;b0[b]=al/a0;a1[b]=-2*cos(w)/a0;a2[b]=(1-al)/a0;}}
 void add(const std::vector<std::vector<float>>& x){for(size_t j=0;j<x[0].size();++j){double p[8]={};for(size_t r=0;r<x.size();++r){double raw=x[r][j];p[7]+=raw*raw;for(int b=0;b<7;++b){double v=raw;for(auto& zz:z[r][b]){double y=b0[b]*v+zz[0];zz[0]=-a1[b]*y+zz[1];zz[1]=-b0[b]*v-a2[b]*y;v=y;}p[b]+=v*v;}}for(int b=0;b<8;++b){sum[b]+=p[b]-ring[b][at];ring[b][at]=p[b];}at=(at+1)%8192;}}
};
static void e7LegacyInterruption(bool shared) {
 Scene s=builtinSceneNamed("Vocal booth");s.length=2.;EngineConfig cfg=e4aConfig(shared);cfg.tailRays=8000;
 Engine e(s,cfg);int S=e.numSources(),R=e.numReceivers();std::vector<std::vector<float>> in(S,std::vector<float>(128)),out(R,std::vector<float>(128));std::vector<const float*> ip(S);std::vector<float*> op(R);for(int k=0;k<S;++k)ip[k]=in[k].data();for(int r=0;r<R;++r)op[r]=out[r].data();
 Vec3 begin=s.listenerPos;bool edited=false,immediate=false;double editedAt=0;unsigned long long previousDone=0;
 for(int frame=0;frame<7*48000;frame+=128){double t=frame/48000.;double dx=.3*std::clamp((t-1)/.5,0.,1.);e.setListener(begin.x-dx,begin.y,begin.z,s.listenerYaw);for(int j=0;j<128;++j)in[0][j]=(float)(.01*sin(2*kPi*997*(frame+j)/48000));e.process(ip.data(),op.data(),128);if((frame+128)%960==0)e.runWorkerStep();auto st=e.motionStats();
 if(!edited&&st.transitionsStarted>0&&st.transitionsDone==0){auto changed=s;changed.tempC+=1;check(e.setScene(changed),"geometry/air edit accepted during settle handover");edited=true;editedAt=t;previousDone=st.transitionsDone;}
 if(edited&&e.tailIsCurrent()&&t-editedAt<1.5){immediate=true;printf("  %s legacy edit consumed %.6f s after request, before old-history drain (settle done %llu)\n",shared?"shared":"per-source",t-editedAt,st.transitionsDone);break;}
 }
 check(edited&&immediate,"geometry/quality legacy path is not blocked until the old tail drains");
}
static void testE7Handover(){
 std::printf("Test 40 (E7): paired periodic octave envelope, handover bounds and exact static tail\n");
 if(std::getenv("E7_LEGACY_ONLY")){e7LegacyInterruption(false);e7LegacyInterruption(true);return;}
 auto grid=[](Vec3 p){return Vec3{round(p.x*4096)/4096,round(p.y*4096)/4096,round(p.z*4096)/4096};};
 std::vector<cd> bins(8192);std::mt19937 rng(171);std::uniform_real_distribution<double> phase(0,2*kPi);
 for(int k=1;k<4096;++k)if(k*48000./8192>=88&&k*48000./8192<=11314){bins[k]=std::polar(1.,phase(rng));bins[8192-k]=std::conj(bins[k]);}verb::fft(bins,true);
 double ss=0;for(auto v:bins)ss+=v.real()*v.real();double scale=.02/sqrt(ss/8192);std::vector<float> signal(8192);for(int j=0;j<8192;++j)signal[j]=(float)(bins[j].real()*scale);
 for(int pass=0;pass<9;++pass){if(const char* one=std::getenv("E7_CASE"))if(pass!=std::atoi(one))continue;int room=pass>=7?2:pass<5?pass:pass-2;const char* names[]={"MotorSynth + EngineSim","MotorSynth + EngineSim","VSL Grand hall - audience","Studio live room","Vocal booth"};
 Scene s=builtinSceneNamed(names[room]);Vec3 begin,end;int source=0;
 if(room<2){for(size_t k=0;k<s.sources.size();++k)if(s.sources[k].name==(room==0?"Engine":"Exhaust"))source=(int)k;begin={-1.5,0,1};end={0,1.5,1};}
 else if(room==2){begin={17,20,1.2};end={17,23,1.2};}else if(room==3){begin={6,3,1.5};end={3,3,1.5};}else{begin={1.5,.8,1.5};end={1+.5*cos(6.),.8+.5*sin(6.),1.5};}
 begin=grid(begin);end=grid(end);Vec3 old=s.hasListener?s.listenerPos:s.receivers[0].pos;for(auto& r:s.receivers)r.pos=begin+grid(r.pos-old);s.hasListener=true;s.listenerPos=begin;
 s=withClutter(withRoughness(s),128);s.clutter.count=0;s.roughness=0;
 EngineConfig cfg=e4aConfig(pass<3||pass>=5);s.length=pass==8?3.2:pass==7?5.643:6.0; // identical allocation at both poses (auto-length is pose-dependent).
 Engine e(s,cfg),ref(withListenerAt(s,end),cfg),held(s,cfg);e.setSoloMask(4);ref.setSoloMask(4);held.setSoloMask(4);
 int S=e.numSources(),R=e.numReceivers();const int start=pass>=7?(int)std::ceil((e.tailSeconds()+1)*48000/128)*128:7*48000,stop=start+3*48000,total=stop+15*48000;
 std::vector<std::vector<float>> in(S,std::vector<float>(128)),a(R,std::vector<float>(128)),b(R,std::vector<float>(128));std::vector<const float*> ip(S);std::vector<float*> ap(R),bp(R);for(int k=0;k<S;++k)ip[k]=in[k].data();for(int r=0;r<R;++r){ap[r]=a[r].data();bp[r]=b[r].data();}
 E7OctaveMeter ma,mb,mh;std::vector<std::vector<float>> h(R,std::vector<float>(128));std::vector<float*> hp(R);for(int r=0;r<R;++r)hp[r]=h[r].data();std::array<double,8> previous{},maxStep{},oldLevel{},peak{};oldLevel.fill(-1e9);peak.fill(-1e9);bool have=false,bitSame=true,completionSame=true;double first=-1;
 for(int frame=0;frame<total;frame+=128){double u=std::clamp((frame-start)/48000.,0.,3.);Vec3 p;
 if(room<2)p=u<1.5?begin+Vec3{0,u,0}:begin+Vec3{u-1.5,1.5,0};else if(room<4)p=begin+(end-begin)*(u/3.);else p={1+.5*cos(2*u),.8+.5*sin(2*u),1.5};if(frame>=stop)p=end;
 e.setListener(p.x,p.y,p.z,s.listenerYaw);held.setListener(p.x,p.y,p.z,s.listenerYaw);for(int j=0;j<128;++j)in[source][j]=signal[(frame+j)%8192];bool wasDone=e.motionStats().transitionsDone>0;e.process(ip.data(),ap.data(),128);ref.process(ip.data(),bp.data(),128);if(wasDone)for(int r=0;r<R;++r)completionSame &= std::memcmp(a[r].data(),b[r].data(),128*sizeof(float))==0;held.process(ip.data(),hp.data(),128);ma.add(a);mb.add(b);mh.add(h);
 if((frame+128)%960==0){e.runWorkerStep();if(frame<stop+int(.3*48000))held.runWorkerStep();}auto st=e.motionStats();double t=(frame-stop)/48000.;if(first<0&&st.transitionsStarted)first=t;
 for(int band=0;band<8;++band){double v=10*log10(std::max(ma.sum[band],1e-35)/std::max(mb.sum[band],1e-35));if(frame>=total-48000)oldLevel[band]=std::max(oldLevel[band],10*log10(std::max(mh.sum[band],1e-35)/std::max(mb.sum[band],1e-35)));if(first>=0){if(have)maxStep[band]=std::max(maxStep[band],fabs(v-previous[band]));peak[band]=std::max(peak[band],v);}previous[band]=v;}if(first>=0)have=true;
 if(frame>=total-48000)for(int r=0;r<R;++r)bitSame &= std::memcmp(a[r].data(),b[r].data(),128*sizeof(float))==0;
 }
 double step=*std::max_element(maxStep.begin(),maxStep.end()),overshoot=-1e9;for(int b=0;b<8;++b)overshoot=std::max(overshoot,peak[b]-std::max(0.,oldLevel[b]));
 printf("  %s / %s / %s: max octave step %.9f dB/128, overshoot %.9f dB, hash %s, last second audio %s\n",names[room],s.sources[source].name.c_str(),cfg.sharedTail?"shared":"per-source",step,overshoot,e.lateStateHash()==ref.lateStateHash()?"same":"DIFFERENT",bitSame?"bit-identical":"DIFFERENT");
 check(step<=.5,"handover step <=0.5 dB/128 per octave/broadband (paired 8192-sample window)");check(overshoot<=.5,"handover never exceeds max(old,static)+0.5 dB per octave/broadband");check(e.lateStateHash()==ref.lateStateHash(),"final IR/gains exactly match cold static build");check(bitSame,"actual late audio bit-identical to cold static on the same periodic input");
 check(completionSame,"actual late audio bit-identical starting immediately after handover completion");
 printf("    final/cold IR seconds %.12g / %.12g\n",e.tailSeconds(),ref.tailSeconds());
 // Flush all causal/filter history, then compare the complete nonzero impulse.
 for(auto& channel:in)std::fill(channel.begin(),channel.end(),0.f);
 int blocks=(int)std::ceil(e.tailSeconds()*48000/128);
 for(int k=0;k<blocks+400;++k){e.process(ip.data(),ap.data(),128);ref.process(ip.data(),bp.data(),128);}
 bool impulseSame=true,finite=true;double impulseEnergy=0;
 for(int k=0;k<blocks+8;++k){in[source][0]=k==0?1.f:0.f;e.process(ip.data(),ap.data(),128);ref.process(ip.data(),bp.data(),128);for(int r=0;r<R;++r){impulseSame &=std::memcmp(a[r].data(),b[r].data(),128*sizeof(float))==0;for(float v:a[r]){finite &=std::isfinite(v);impulseEnergy+=double(v)*v;}}}
 printf("    full-tail impulse energy %.12g, %s\n",impulseEnergy,impulseSame?"bit-identical":"DIFFERENT");
 check(impulseSame&&finite&&impulseEnergy>0,"full post-handover impulse finite, nonzero and bit-identical to cold static");
 }
 e7LegacyInterruption(false);e7LegacyInterruption(true);
}
