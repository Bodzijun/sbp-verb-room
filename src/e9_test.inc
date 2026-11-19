static void testE9Paths() {
    check(bakedProgressPercent(33,33,0,0)==45,"E9b initial stations reserve progress for validation (old96 plateau)");
    check(bakedProgressPercent(33,33,16,32)==72&&bakedProgressPercent(33,33,32,32)==99,"E9b midpoint validation advances progress before publication");
    {size_t nodes=2,at=0,residual=0;std::vector<std::pair<double,double>>segments{{0,1}};while(at<segments.size()&&at<256){auto ab=segments[at++];auto decision=bakedRefinement(2.,nodes,ab.first,ab.second);if(decision==BakedRefinement::Split){++nodes;double m=(ab.first+ab.second)*.5;segments.push_back({ab.first,m});segments.push_back({m,ab.second});}else if(decision==BakedRefinement::Approximate)++residual;}check(nodes==64&&residual>0&&at==segments.size()&&at<128,"E9b cap64 terminates refinement with explicit approximate residuals");}
    {   // P2: honest bake progress on a synthetic refinement of 33 initial nodes with several split rates (64-node cap)
        bool monotone=true,capped=true,reachesTop=true;int worstRun=0;
        for(double splitRate:{0.,.05,.1,.2,.3,.5,1.}){
            size_t initial=33,nodes=initial,decided=0,done=initial;std::vector<std::pair<double,double>>segments;for(size_t j=1;j<initial;++j)segments.push_back({double(j-1),double(j)});
            double shown=bakedAdvanceProgress(0,bakedHonestProgressPercent(initial,bakedStationEstimate(initial,0,0,initial)));int lastInt=(int)shown,run=0;unsigned rng=7;
            while(decided<segments.size()){
                auto ab=segments[decided++];++done;rng=rng*1664525u+1013904223u;const bool split=((rng>>8)&0xffff)/65536.0<splitRate&&nodes<64;
                if(split){++nodes;double m=(ab.first+ab.second)*.5;segments.push_back({ab.first,m});segments.push_back({m,ab.second});}
                const double next=bakedAdvanceProgress(shown,bakedHonestProgressPercent(done,bakedStationEstimate(initial,segments.size(),decided,nodes)));
                monotone&=next>=shown;capped&=next<=99.;shown=next;if((int)shown==lastInt)worstRun=std::max(worstRun,++run);else run=0;lastInt=(int)shown;
            }
            reachesTop&=shown>=90.;
        }
        check(monotone&&capped,"P2 honest bake progress is monotone and stays below 100 until the table is published");
        check(worstRun<=6&&reachesTop,"P2 honest bake progress never stands still for more than 6 completed stations and approaches the end");
        check(bakedHonestProgressPercent(0,0.)==0&&bakedStationEstimate(33,0,0,33)>33.&&bakedStationEstimate(33,94,94,64)==33.+94.,"P2 progress estimate: empty input, initial phase, finished refinement");
    }
    {   // P3: per-band split thresholds (1 dB 250-2000 Hz, 2 dB 125/4k/8k) and the node cap
        std::array<double,NB> e{};
        e[3]=1.0;check(bakedRefinementBands(e,2,0.,1.)==BakedRefinement::Accept,"P3 band error exactly at the 1 dB mid-band threshold is accepted");
        e[3]=1.01;check(bakedRefinementBands(e,2,0.,1.)==BakedRefinement::Split,"P3 mid-band error above 1 dB splits");
        e={};e[0]=1.9;e[5]=1.9;e[6]=1.9;check(bakedRefinementBands(e,2,0.,1.)==BakedRefinement::Accept,"P3 edge bands (125/4k/8k) below 2 dB are accepted");
        e[6]=2.01;check(bakedRefinementBands(e,2,0.,1.)==BakedRefinement::Split,"P3 edge band above 2 dB splits");
        e={};e[2]=3.;check(bakedRefinementBands(e,kBakeNodeCap,0.,1.)==BakedRefinement::Approximate,"P3 band rule at the node cap marks approximate, never splits");
        check(bakedRefinementBands(e,2,0.5,0.5)==BakedRefinement::Approximate,"P3 degenerate segment cannot split");
        // P3: in-flight progress credit is monotone, bounded, and keeps the bar below 100
        bool mono=true,capped=true;double prev=0.;
        for(double credit=0.;credit<=4.;credit+=.1){const double v=bakedHonestProgressActive(30.,credit,40.);mono&=v>=prev;capped&=v<=99.;prev=v;}
        check(mono&&capped,"P3 active-station progress credit is monotone and capped below 100");
        check(bakedHonestProgressActive(40.,0.,40.)==99.&&bakedHonestProgressActive(0.,0.,0.)==0.,"P3 active progress: full completion reads 99, empty estimate reads 0");
        check(bakedAdvanceProgressD(50.,40.)==50.+(99.-50.)*.02,"P3 double advance keeps the 2%-of-remaining nudge and never goes back");
    }
    std::printf("E9 production listener bake, publication, fallback and invalidation\n");
    for(bool shared:{false,true}) {
        auto sc=withClutter(withRoughness(builtinSceneNamed("Vocal booth")),128);sc.clutter.count=0;sc.roughness=0;
        if(shared){auto extra=sc.sources.front();extra.pos.x+=.1;sc.sources.push_back(extra);extra.pos.x+=.1;sc.sources.push_back(extra);}
        sc=withListenerAt(sc,{1.2,.8,1.5});sc.listenerYaw=0;auto cfg=e4aConfig(shared);cfg.tailRays=8000;
        Engine a(sc,cfg),b(sc,cfg);BakedPath path;path.object=-1;path.poses={{{1.2,.8,1.5,0}},{{1.6,.8,1.5,0}}};path.distance={0,.4};
        a.setBakedPaths({path});b.setBakedPaths({path});
        check(a.bakeStats().state==Engine::BakeState::NotBaked,"E9b path descriptors do not start automatic baking");
        a.requestPathBake(1);b.requestPathBake(27);
        std::vector<std::vector<float>> input(a.numSources(),std::vector<float>(128,.01f)),output(a.numReceivers(),std::vector<float>(128));
        std::vector<const float*>ip;std::vector<float*>op;for(auto&v:input)ip.push_back(v.data());for(auto&v:output)op.push_back(v.data());
        auto step=[&](Engine&e){e.process(ip.data(),op.data(),128);e.runWorkerStep();std::this_thread::sleep_for(std::chrono::milliseconds(1));};
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        while((a.bakeStats().percent<100||b.bakeStats().percent<100)&&std::chrono::steady_clock::now()<deadline){step(a);step(b);}
        check(a.bakeStats().percent==100&&b.bakeStats().percent==100,"E9 listener bake completes in both convolver modes");
        check(a.bakeStats().hash!=0&&a.bakeStats().hash==b.bakeStats().hash,"E9 independently traced deterministic table hash");
        auto table=a.bakedTableSnapshot();check(table&&table->u.size()>=2&&table->u.size()<=64,"E9 immutable snapshot has bounded adaptive nodes");
        if(table){std::vector<double>e(table->stride());double err=0;for(size_t j=0;j<table->u.size();++j){table->lookup(table->u[j],e.data());for(size_t k=0;k<e.size();++k)if(table->energy[j*e.size()+k]>0)err=std::max(err,std::abs(10*std::log10(e[k]/table->energy[j*e.size()+k])));}check(err<1e-9,"E9 station lookup matches traced node energy");}
        for(int j=0;j<120;++j){a.setListener(1.2+.4*j/119.,.8,1.5,0);a.process(ip.data(),op.data(),128);if(j%19==0)a.runWorkerStep();}
        auto old=a.bakeStats().lookups;check(old>0,"E9 real process consumes valid listener table");a.notifyJump();a.setListener(1.3,.8,1.5,0);a.process(ip.data(),op.data(),128);check(a.bakeStats().lookups>old,"E9 jump looks up baked target before worker rebuild");
        // A second enabled path selects the legacy estimator for the whole coupled case.
        BakedPath source=path;source.object=0;a.setBakedPaths({path,source});old=a.bakeStats().lookups;a.process(ip.data(),op.data(),128);check(a.bakeStats().lookups==old&&a.bakeStats().percent==0,"E9 both-path case cleanly selects fallback");
        a.requestPathBake();step(a);check(a.bakeStats().state==Engine::BakeState::Blocked&&a.bakeStats().waitReason==10,"E9b both-moving request returns explicit blocked reason");
        // A static source change rejects the old coverage immediately on the audio thread.
        a.setBakedPaths({path});a.requestPathBake(3);deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);while(a.bakeStats().percent<100&&std::chrono::steady_clock::now()<deadline)step(a);check(a.bakeStats().percent==100,"E9b rebake before static invalidation test is ready");old=a.bakeStats().lookups;
        a.setSourcePosition(0,sc.sources[0].pos.x+.1,sc.sources[0].pos.y,sc.sources[0].pos.z);a.process(ip.data(),op.data(),128);check(a.bakeStats().lookups==old,"E9 static-object edit invalidates table before worker runs");
        check(a.bakeStats().state==Engine::BakeState::Outdated,"E9b baked -> outdated on static edit");a.requestPathBake(3);check(a.bakeStats().state==Engine::BakeState::Baking,"E9b outdated -> baking requires action");a.cancelPathBake();check(a.bakeStats().state==Engine::BakeState::Cancelled,"E9b explicit cancel stops a requested bake");
        a.clearPathBake();check(a.bakeStats().state==Engine::BakeState::NotBaked&&a.bakeStats().percent==0,"E24 clearPathBake returns to notBaked");a.process(ip.data(),op.data(),128);
        // Fresh source path, then material invalidation while it is covered.
        Engine sourceEngine(sc,cfg);BakedPath moving;moving.object=0;auto pos=sc.sources[0].pos;moving.poses={{{pos.x,pos.y,pos.z,sc.sources[0].azimuthDeg}},{{pos.x+.2,pos.y,pos.z,sc.sources[0].azimuthDeg}}};moving.distance={0,.2};sourceEngine.setBakedPaths({moving});sourceEngine.requestPathBake(3);deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);while(sourceEngine.bakeStats().percent<100&&std::chrono::steady_clock::now()<deadline)step(sourceEngine);check(sourceEngine.bakeStats().percent==100,"E9 source path bake completes");
        sourceEngine.setSourcePosition(0,pos.x+.1,pos.y,pos.z);sourceEngine.process(ip.data(),op.data(),128);check(sourceEngine.bakeStats().lookups>0,"E9 source path is consumed in process");
        auto changed=sc;for(auto&material:changed.mats)for(int band=0;band<NB;++band)material.alpha[band]=std::min(std::max(0.,1.-material.tau[band]),material.alpha[band]*1.05);check(sourceEngine.setScene(changed),"E9 material edit accepted");old=sourceEngine.bakeStats().lookups;sourceEngine.process(ip.data(),op.data(),128);check(sourceEngine.bakeStats().lookups==old&&sourceEngine.bakeStats().percent<100,"E9 material edit invalidates published table immediately");
        sourceEngine.requestPathBake(1);deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);while(sourceEngine.bakeStats().done==0&&std::chrono::steady_clock::now()<deadline)step(sourceEngine);check(sourceEngine.bakeStats().state==Engine::BakeState::Baking,"E9b regression edits a genuinely active batch");sourceEngine.setListener(1.3,.8,1.5,0);sourceEngine.process(ip.data(),op.data(),128);check(sourceEngine.bakeStats().state==Engine::BakeState::Outdated,"E9b static edit during batch immediately reports outdated");sourceEngine.runWorkerStep();check(sourceEngine.bakeStats().percent<100,"E9b invalidated active batch cannot publish stale table");
        // A closed physical lap returns to exactly the same source pose.
        BakedPath loop=moving;loop.closed=true;loop.poses.push_back(loop.poses.front());loop.distance={0,.2,.4};Engine closed(sc,cfg);closed.setBakedPaths({loop});closed.requestPathBake(3);deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);while(closed.bakeStats().percent<100&&std::chrono::steady_clock::now()<deadline)step(closed);check(closed.bakeStats().percent==100,"E9 closed source loop bakes once");auto ct=closed.bakedTableSnapshot();if(ct){std::vector<double>x(ct->stride()),y(ct->stride());ct->lookup(0,x.data());ct->lookup(1,y.data());check(x==y,"E9 closed wrap reuses the same station values");}
    }
}
