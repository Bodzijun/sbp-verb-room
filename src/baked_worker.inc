    // Audio is sole writer. Ready -> reading CAS excludes re-arming while control copies.
    static constexpr size_t passCap=4096;
    std::vector<double> passBuffer;
    std::atomic<int> passCommand{0},passState{0},passCount{0}; // 0 empty,1 armed,2 recording,3 ready,4 reading
    std::atomic<uint64_t> passGeneration{0};
    uint64_t passConsumed=0;
    std::vector<BakedPath> recordedPaths,declaredPaths;
    bool bakeForcedBlock=false,bakeForcedApplied=false; // E19: a Look at dependency the tables cannot represent (control thread, under bakeRequestMx)
    std::vector<double> bakeJointPose;
    std::atomic<bool> passOff{false};
    uint32_t passProjectTick=0;
    void applyBakedPathsLocked(const std::vector<BakedPath>& declared);
    void recordPassAudio(bool playing) noexcept {
        if(passState.load(std::memory_order_acquire)==4)return;
        int cmd=passCommand.load(std::memory_order_acquire);
        if(cmd==1){
            int expected=passState.load(std::memory_order_acquire);
            if(expected==4||!passState.compare_exchange_strong(expected,1,std::memory_order_acq_rel))return;
            passCount.store(0,std::memory_order_release);
            passGeneration.fetch_add(1,std::memory_order_acq_rel);
            passOff.store(false,std::memory_order_relaxed);
            int arm=1;passCommand.compare_exchange_strong(arm,0,std::memory_order_acq_rel);
        }else cmd=passCommand.exchange(0,std::memory_order_acq_rel);
        const int state=passState.load(std::memory_order_relaxed);
        if(cmd==2||(!playing&&state==2)){
            if(state==1||state==2){
                const size_t cnt=(size_t)passCount.load(std::memory_order_relaxed);
                if(cnt<2)passCount.store(0,std::memory_order_relaxed);
                passGeneration.fetch_add(1,std::memory_order_acq_rel);
                passState.store(cnt>=2?3:0,std::memory_order_release);
            }
            return;
        }
        if(!playing||(state!=1&&state!=2))return;
        const size_t stride=4*((size_t)S+1),n=(size_t)passCount.load(std::memory_order_relaxed);
        if(n>=passCap||passBuffer.size()<passCap*stride){
            passGeneration.fetch_add(1,std::memory_order_acq_rel);
            passState.store(n>=2?3:0,std::memory_order_release);
            return;
        }
        double distance=0;
        if(n)for(int j=0;j<=S;++j)for(int k=0;k<4;++k){
            double v=params[(j==0?S*4:(j-1)*4)+k].load(std::memory_order_relaxed)-passBuffer[(n-1)*stride+4*j+k];
            if(k==3)v=std::remainder(v,360.)*.01;distance+=v*v;
        }
        if(n&&distance<.005*.005)return;
        for(int j=0;j<=S;++j)for(int k=0;k<4;++k)passBuffer[n*stride+4*j+k]=params[(j==0?S*4:(j-1)*4)+k].load(std::memory_order_relaxed);
        passCount.store((int)n+1,std::memory_order_release);passState.store(2,std::memory_order_release);
        if(n+1==passCap){passGeneration.fetch_add(1,std::memory_order_acq_rel);passState.store(3,std::memory_order_release);}
    }
    // Caller must hold bakeRequestMx.
    void consumeRecordedPass(){
        const uint64_t gen=passGeneration.load(std::memory_order_acquire);
        if(gen==passConsumed)return;
        const int st=passState.load(std::memory_order_acquire);
        if(st==0){recordedPaths.clear();passConsumed=gen;return;}
        int ready=3;if(!passState.compare_exchange_strong(ready,4,std::memory_order_acq_rel))return;
        // Always release ownership, including on allocation failure.
        struct Release { std::atomic<int>& state;~Release(){state.store(3,std::memory_order_release);} } release{passState};
        const size_t n=(size_t)passCount.load(std::memory_order_acquire),stride=4*((size_t)S+1);
        std::vector<BakedPath> next;
        if(n>=2&&n<=passCap&&passBuffer.size()>=n*stride){
            std::vector<double> distance(n,0.);
            for(size_t i=1;i<n;++i){double e=0;for(size_t k=0;k<stride;++k){double v=passBuffer[i*stride+k]-passBuffer[(i-1)*stride+k];if(k%4==3)v=std::remainder(v,360.)*.01;e+=v*v;}distance[i]=distance[i-1]+std::sqrt(e);}
            if(distance.back()>0)for(int j=0;j<=S;++j){BakedPath p;p.object=j-1;p.group=2000;p.recorded=true;p.recording=gen;p.distance=distance;
                for(size_t i=0;i<n;++i){std::array<double,4> v;for(int k=0;k<4;++k)v[k]=passBuffer[i*stride+4*j+k];p.poses.push_back(v);}next.push_back(std::move(p));}
        }
        recordedPaths=std::move(next);passConsumed=gen;
    }
    std::mutex bakeRequestMx;
    std::vector<BakedPath> bakePaths;
    struct BakedGroupSpec {
        int group=-1;
        bool movesListener=false;
        std::vector<BakedPath> paths;
        std::vector<int> groupSources;
        std::vector<int> objects;
    };
    struct BakedGroupRuntime {
        BakedGroupSpec spec;
        BakeState state=BakeState::NotBaked;
        int percent=0,discontinuities=0,done=0,total=0;
        bool dirty=false,touched=false;
        std::shared_ptr<const BakedTable> table;
    };
    std::vector<BakedGroupRuntime> bakeGroups;
    std::vector<std::atomic<uint64_t>> bakeObjectRevision;
    std::vector<uint8_t> bakeObjectTouched;
    std::atomic<uint64_t> bakeRevision{0},bakeAction{0},bakePreempt{0};
    std::atomic<bool> bakeEnabled{false},bakeRequested{false};
    std::atomic<BakeState> bakeState{BakeState::NotBaked};
    std::atomic<unsigned> bakeWantedWorkers{0};
    std::atomic<int> bakePercent{0},bakeDiscontinuities{0},bakeWaitReason{0},bakeWorkers{0},bakeDone{0},bakeTotal{0};
    std::atomic<uint64_t> bakeHash{0},bakeLookups{0},bakeStations{0},bakeResets{0},bakeCancelled{0};
    std::atomic<double> bakeMilliseconds{0};
    struct BakedTableSlot { std::vector<BakedTable> tables; };
    std::array<BakedTableSlot,3> bakeSlots;
    std::atomic<int> bakeMiddle{1};int bakeBack=2,bakeFront=0;
    std::shared_ptr<const BakedTable> bakeDiagnostic,bakeInput,bakeRunningDiagnostic;
    std::shared_ptr<const std::vector<BakedTable>> bakeDiagnosticList,bakeRunningInputs;
    std::shared_ptr<World> bakeWorld;
    std::shared_ptr<std::atomic<bool>> bakeStop;
    std::shared_ptr<BakePool> bakePool;
    unsigned bakePoolCount=0;
    struct BakeBatch {
        double u=0;int rays=0;
        uint64_t preempt=0;
        std::vector<std::future<TailHist>> futures;
        std::vector<TailHist> hist;
    };
    struct GroupBakeJob {
        size_t groupIndex=0;
        BakedTable table;
        std::map<double,std::shared_ptr<BakeBatch>> active;
        std::map<double,std::vector<double>> cache;
        std::vector<double> nodes,initial;
        std::vector<std::pair<double,double>> segments;
        size_t initialAt=0,segmentAt=0;
        int baseRays=0;
        bool finished=false;
    };
    std::vector<GroupBakeJob> bakeJobs;
    std::vector<BakedTable> bakeKeptTables;
    size_t bakeActiveLimit=1;
    std::vector<size_t> bakeProjectHints;
    std::vector<uint8_t> bakeAudioSourceValid;
    std::vector<double> bakeLookupScratch;
    uint64_t bakeWorkerAction=0,bakeFallbackAction=0;
    uint64_t bakeQueuedAction=0,bakeQueuedStream=0;
    std::vector<double> bakeQueuedPose;
    std::chrono::steady_clock::time_point bakeQueuedWall;
    std::chrono::steady_clock::time_point bakeStarted;
    std::vector<double> bakeAudioEnergy,bakeAudioNorm;
    std::vector<float> bakeReferenceGain;
    bool bakeSharedApplied=false,bakePendingJump=false;
    std::array<std::vector<double>,2> bakeNormBuffers;
    std::vector<double> bakeTransNorm;
    std::vector<double> bakeAudioRefPose,bakeTransRefPose;
    std::array<std::vector<double>,2> bakeRefPoseBuffers;
    bool bakeAudioValid=false,bakeWasUsed=false;

    bool bakeContextTable(const BakedTable& t,bool requirePublishedU=true) const noexcept {
        if(requirePublishedU && t.u.size()<2)return false;
        if(t.path.recorded&&(t.path.recording!=passGeneration.load(std::memory_order_acquire)||passCommand.load(std::memory_order_acquire)==1||passState.load(std::memory_order_acquire)==1||passState.load(std::memory_order_acquire)==2))return false;
        if(t.geometry!=geomSerial.load(std::memory_order_relaxed)||t.quality!=qualitySerial.load(std::memory_order_relaxed))return false;
        if(t.objRevisions.size()!=t.movingPaths.size())return false;
        for(size_t i=0;i<t.movingPaths.size();++i){
            const int idx=t.movingPaths[i].object<0?0:t.movingPaths[i].object+1;
            if(idx<0||idx>S||t.objRevisions[i]!=bakeObjectRevision[(size_t)idx].load(std::memory_order_acquire))return false;
        }
        if(!t.movesListener){
            double lst[4];for(int k=0;k<4;++k)lst[k]=params[S*4+k].load(std::memory_order_relaxed);
            if(poseFar(lst,t.staticPose.data(),cfg.motionDeadbandM,cfg.motionDeadbandDeg))return false;
        }else{
            for(int j=1;j<=S;++j){
                if(t.moves(j))continue;
                double p[4];for(int k=0;k<4;++k)p[k]=params[(j-1)*4+k].load(std::memory_order_relaxed);
                if(poseFar(p,t.staticPose.data()+j*4,cfg.motionDeadbandM,cfg.motionDeadbandDeg))return false;
            }
        }
        for(int s:t.groupSources)if(s>=0&&s<S&&t.patterns[(size_t)s]!=params[S*4+8+s].load(std::memory_order_relaxed))return false;
        return true;
    }
    static uint64_t combinedTableHash(const std::vector<BakedTable>& tables) {
        if(tables.empty())return 0;
        if(tables.size()==1)return tables.front().hash();
        uint64_t h=1469598103934665603ull;
        auto mix=[&](const void* v,size_t n){auto p=(const unsigned char*)v;for(size_t j=0;j<n;++j){h^=p[j];h*=1099511628211ull;}};
        for(size_t i=0;i<tables.size();++i){
            const uint64_t th=tables[i].hash();
            const int grp=tables[i].path.group;
            const int obj=tables[i].path.object;
            mix(&grp,sizeof grp);mix(&obj,sizeof obj);mix(&th,sizeof th);
        }
        return h;
    }
    std::vector<BakedGroupSpec> partitionBakeGroups(const std::vector<BakedPath>& paths) const {
        std::vector<BakedGroupSpec> specs;
        std::map<int64_t,size_t> byKey;
        for(size_t idx=0;idx<paths.size();++idx){
            const auto&p=paths[idx];
            const int64_t key=p.group>=0?(int64_t)p.group:-(int64_t)(idx+1);
            auto it=byKey.find(key);
            if(it==byKey.end()){
                byKey[key]=specs.size();
                BakedGroupSpec sp;sp.group=p.group;specs.push_back(std::move(sp));
            }
            auto&sp=specs[byKey[key]];
            sp.paths.push_back(p);
            sp.objects.push_back(p.object);
            if(p.object<0)sp.movesListener=true;
        }
        for(auto&sp:specs){
            if(sp.movesListener){
                for(int s=0;s<S;++s)sp.groupSources.push_back(s);
            }else{
                for(const auto&p:sp.paths)if(p.object>=0)sp.groupSources.push_back(p.object);
                std::sort(sp.groupSources.begin(),sp.groupSources.end());
                sp.groupSources.erase(std::unique(sp.groupSources.begin(),sp.groupSources.end()),sp.groupSources.end());
            }
        }
        return specs;
    }
    int bakePriority() const {
        if(jumpSerial.load()!=workerJumpSeen)return 1;
        if(shared){if(sLastGeom!=geomSerial.load()||sLastQuality!=workerQuality)return 2;}
        else for(auto&sp:srcs)if(sp->lastGeom!=geomSerial.load()||sp->lastQuality!=workerQuality)return 5;
        if(!anchorValid)return 0;
        // Match the normal worker's resolved anchor; raw wall-line coordinates can differ permanently.
        bool moved=false;for(int j=0;j<=S;++j){const double*ref=j==0?(shared?sLastLst:srcs.front()->lastLst):srcs[(size_t)j-1]->lastPose;moved|=poseFar(anchor.data()+4*j,ref,cfg.motionDeadbandM,cfg.motionDeadbandDeg);}
        return moved&&((double)(streamPos.load()-anchorStream)>=cfg.motionSettleSec*sr || (!cfg.manualWorker&&std::chrono::duration<double>(std::chrono::steady_clock::now()-anchorWall).count()>=cfg.motionSettleSec))?7:0;
    }
    static double bakeError(const std::vector<double>& a,const std::vector<double>& mid,const std::vector<double>& b,int ns,int nr,const std::vector<double>*noise=nullptr,const std::vector<double>*sumNoise=nullptr) {
        double worst=0;
        for(int r=0;r<nr;++r)for(int band=0;band<NB;++band){double peak=0,sum=0,predSum=0;
            for(int s=0;s<ns;++s)peak=std::max(peak,mid[((size_t)s*nr+r)*NB+band]);
            for(int s=0;s<ns;++s){size_t k=((size_t)s*nr+r)*NB+band;double amp=.5*(std::sqrt(std::max(0.,a[k]))+std::sqrt(std::max(0.,b[k]))),pred=amp*amp;sum+=mid[k];predSum+=pred;if(mid[k]>=peak*1e-4&&peak>0)worst=std::max(worst,std::max(0.,std::abs(10*std::log10(std::max(1e-30,pred)/std::max(1e-30,mid[k])))-(noise?(*noise)[k]:0.)));}
            if(sum>0)worst=std::max(worst,std::max(0.,std::abs(10*std::log10(std::max(1e-30,predSum)/sum))-(sumNoise?(*sumNoise)[(size_t)r*NB+band]:0.)));
        }return worst;
    }
    bool bakeActiveEmpty() const {for(const auto&jb:bakeJobs)if(!jb.active.empty())return false;return true;}
    void stopBakeTasks(){if(bakeStop)bakeStop->store(true);for(auto&jb:bakeJobs)jb.active.clear();bakeJobs.clear();bakeKeptTables.clear();}
    void endBake(BakeState state,int reason,uint64_t expected){
        std::lock_guard<std::mutex>lock(bakeRequestMx);
        if(bakeAction.load()!=expected)return;
        bakeRequested=false;bakePercent=0;bakeState=state;bakeWaitReason=reason;lastEstEpoch=~0ull;
        for(auto&gr:bakeGroups){
            if(state==BakeState::Blocked||state==BakeState::Failed||state==BakeState::Cancelled)gr.state=state;
            else if(gr.table && !bakeContextTable(*gr.table,true)){gr.state=BakeState::Outdated;gr.percent=0;gr.dirty=true;}
        }
    }
    void invalidateBake(){stopBakeTasks();endBake(BakeState::Outdated,11,bakeWorkerAction);}
    void queueGroupStation(GroupBakeJob&jb,double u,int rays){
        auto job=std::make_shared<BakeBatch>();job->u=u;job->rays=rays;job->hist.resize(jb.table.groupSources.size());
        auto pose=jb.table.staticPose;
        for(const auto&path:jb.table.movingPaths){auto v=path.at(u);int offset=path.object<0?0:4+4*path.object;for(int k=0;k<4;++k)pose[(size_t)offset+k]=v[k];}
        SurfaceSideHist lh;double listener[4];resolvePose4(bakeWorld->geo,bakeWorld->serial,lh,pose.data(),listener);
        auto receivers=std::make_shared<std::vector<Transducer>>();resolvedReceivers(bakeWorld->geo,listener,*receivers,!shared);
        auto world=bakeWorld;auto stop=bakeStop;const auto preempt=bakePreempt.load();job->preempt=preempt;const bool pooled=shared;
        const int order=cfg.erOrder,limit=cfg.adaptiveTail?(int)std::min<int64_t>(2000000,4ll*rays):rays;
        const auto jobId=jb.table.job;
        const auto refTable=std::make_shared<BakedTable>(jb.table);
        for(size_t si=0;si<jb.table.groupSources.size();++si){
            const int s=jb.table.groupSources[si];
            Transducer source=currentSource(s);SurfaceSideHist sh;double eff[4];
            resolvePose4(world->geo,world->serial,sh,pose.data()+4+4*s,eff);
            source.pos={eff[0],eff[1],eff[2]};source.azimuthDeg=eff[3];source.pattern=workerPattern(s);
            job->futures.push_back(bakePool->submit([this,world,refTable,stop,receivers,source,preempt,pooled,order,rays,limit,s,jobId]{
                auto cancelled=[&]{return !running||stop->load(std::memory_order_relaxed)||bakePreempt.load(std::memory_order_relaxed)!=preempt||jobId!=bakeAction.load(std::memory_order_relaxed)||!bakeContextTable(*refTable,false);};
                if(cancelled())throw std::runtime_error("Bake superseded");
                if(!cfg.adaptiveTail){
                    auto e=traceTailEnergy(world->sc,source,*receivers,order,rays,world->sc.seed+131*s,cancelled);
                    TailHist hist;hist.nBins=1;hist.binDur=.001;hist.h.resize(e.size());
                    for(size_t r=0;r<e.size();++r)for(int b=0;b<NB;++b)hist.h[r][b].assign(1,e[r][b]);
                    return hist;
                }
                if(pooled)return traceTail(world->sc,source,*receivers,order,rays,world->sc.seed+131*s,false,cancelled);
                int used=0;return traceTailAdaptive(world->sc,source,*receivers,order,rays,limit,world->sc.seed+131*s,used,nullptr,cancelled);
            }));
        }
        jb.active.emplace(u,std::move(job));
    }
    void bakeWorkerStepImpl() {
        const auto state=bakeState.load();
        if(state==BakeState::Baked||state==BakeState::Approximate||state==BakeState::Outdated||state==BakeState::NotBaked){
            std::lock_guard<std::mutex>lock(bakeRequestMx);
            bool anyPublishedInvalid=false;
            for(auto&gr:bakeGroups){
                if((gr.state==BakeState::Baked||gr.state==BakeState::Approximate)&&(!gr.table||!bakeContextTable(*gr.table,true))){
                    gr.state=BakeState::Outdated;gr.percent=0;gr.dirty=true;anyPublishedInvalid=true;
                }
            }
            if(anyPublishedInvalid){
                bakePercent=0;bakeState=BakeState::Outdated;bakeWaitReason=11;lastEstEpoch=~0ull;
                if(!bakeRequested.load()){stopBakeTasks();return;}
            }
        }
        if(!bakeRequested.load()){stopBakeTasks();if(bakeFallbackAction!=bakeAction.load()){lastEstEpoch=~0ull;bakeFallbackAction=bakeAction.load();}return;}
        if(!bakeEnabled.load()||cfg.motionMode<2){const auto action=bakeAction.load();stopBakeTasks();endBake(BakeState::Blocked,10,action);return;}
        bakeWaitReason=bakePriority();if(bakeWaitReason.load())return;
        const auto action=bakeAction.load();
        if(action!=bakeWorkerAction){
            // Wait for the audio tracker to apply edits that preceded the button.
            const auto pose=livePose();bool changed=action!=bakeQueuedAction;
            if(!changed){std::lock_guard<std::mutex>lock(bakeRequestMx);for(int j=0;j<=S;++j){bool moving=false;for(const auto&p:bakePaths)moving|=j==(p.object<0?0:p.object+1);if(!moving)changed|=poseFar(pose.data()+4*j,bakeQueuedPose.data()+4*j,cfg.motionDeadbandM,cfg.motionDeadbandDeg);}}
            if(changed){bakeQueuedAction=action;bakeQueuedPose=pose;bakeQueuedStream=streamPos.load();bakeQueuedWall=std::chrono::steady_clock::now();}
            const bool settled=cfg.manualWorker?(double)(streamPos.load()-bakeQueuedStream)>=cfg.motionSettleSec*sr:std::chrono::duration<double>(std::chrono::steady_clock::now()-bakeQueuedWall).count()>=cfg.motionSettleSec;
            if(!settled){bakeWaitReason=14;return;}
            stopBakeTasks();bakeStop=std::make_shared<std::atomic<bool>>(false);++bakeResets;
            std::vector<BakedGroupRuntime> groupsCopy;
            uint64_t rev=0;
            {
                std::lock_guard<std::mutex>lock(bakeRequestMx);
                if(bakePaths.empty()||bakeGroups.empty())return;
                groupsCopy=bakeGroups;
                rev=bakeRevision.load();
            }
            const auto geom=geomSerial.load();const auto qual=qualitySerial.load();
            const auto live=livePose();
            std::vector<double> patterns((size_t)S,0.);for(int s=0;s<S;++s)patterns[(size_t)s]=workerPattern(s);
            std::vector<uint64_t> objRevs((size_t)S+1,0);for(int idx=0;idx<=S;++idx)objRevs[(size_t)idx]=bakeObjectRevision[(size_t)idx].load(std::memory_order_acquire);
            {std::lock_guard<std::mutex>lock(worldMx);bakeWorld=latest;}
            double xmin=1e30,xmax=-1e30,ymin=1e30,ymax=-1e30;
            for(const auto&w:bakeWorld->sc.walls){xmin=std::min({xmin,w.ax,w.bx});xmax=std::max({xmax,w.ax,w.bx});ymin=std::min({ymin,w.ay,w.by});ymax=std::max({ymax,w.ay,w.by});}
            const double roomMin=xmax>xmin&&ymax>ymin?std::min({xmax-xmin,ymax-ymin,bakeWorld->sc.height}):5.;
            bool allValid=true;
            for(const auto&gr:groupsCopy)if(!(gr.table&&gr.table->movingPaths==gr.spec.paths&&gr.table->movesListener==gr.spec.movesListener&&bakeContextTable(*gr.table,true))){allValid=false;break;}
            bakeJobs.clear();bakeKeptTables.clear();
            auto runningList=std::make_shared<std::vector<BakedTable>>();
            size_t maxGroupS=1;
            for(size_t gi=0;gi<groupsCopy.size();++gi){
                const auto&gr=groupsCopy[gi];
                if(!allValid && gr.table && gr.table->movingPaths==gr.spec.paths && gr.table->movesListener==gr.spec.movesListener && bakeContextTable(*gr.table,true)){
                    bakeKeptTables.push_back(*gr.table);
                    continue;
                }
                GroupBakeJob jb;
                jb.groupIndex=gi;
                const int nGrpSrc=std::max(1,(int)gr.spec.groupSources.size());
                jb.baseRays=shared?std::max(1500,cfg.tailRays/nGrpSrc):cfg.tailRays;
                jb.table.path=gr.spec.paths.front();
                jb.table.movingPaths=gr.spec.paths;
                jb.table.groupSources=gr.spec.groupSources;
                jb.table.movesListener=gr.spec.movesListener;
                jb.table.sources=(int)gr.spec.groupSources.size();
                jb.table.receivers=R;
                jb.table.request=rev;jb.table.job=action;jb.table.geometry=geom;jb.table.quality=qual;
                jb.table.staticPose=live;jb.table.patterns=patterns;
                for(const auto&mp:gr.spec.paths){const int idx=mp.object<0?0:mp.object+1;jb.table.objRevisions.push_back(objRevs[(size_t)idx]);}
                bool useJointCurve=false;
                for(const auto&mp:gr.spec.paths)if(!mp.recorded&&(mp.hasTiming()||mp.uIndexed)){useJointCurve=true;break;}
                if(useJointCurve){
                    jb.table.buildJointCurve();
                    jb.initial=jb.table.initialStationsForJoint(roomMin);
                    jb.nodes=jb.initial;
                }else{
                    double maxDist=0;for(const auto&mp:gr.spec.paths)maxDist=std::max(maxDist,mp.distance.back());
                    int intervals=std::clamp((int)std::ceil(maxDist/std::clamp(.1*roomMin,.1,.5)),1,31);
                    jb.initial={0.,.5,1.};for(int j=1;j<intervals;++j)if((double)j/intervals!=.5)jb.initial.push_back((double)j/intervals);
                    std::sort(jb.initial.begin(),jb.initial.end());jb.nodes=jb.initial;
                }
                maxGroupS=std::max(maxGroupS,std::max<size_t>(1,jb.table.groupSources.size()));
                runningList->push_back(jb.table);
                {
                    std::lock_guard<std::mutex>lock(bakeRequestMx);
                    if(gi<bakeGroups.size()&&bakeAction.load()==action){bakeGroups[gi].state=BakeState::Baking;bakeGroups[gi].percent=0;bakeGroups[gi].done=0;bakeGroups[gi].total=(int)jb.initial.size();}
                }
                bakeJobs.push_back(std::move(jb));
            }
            bakeInput=!runningList->empty()?std::make_shared<BakedTable>(runningList->front()):std::shared_ptr<BakedTable>{};
            std::atomic_store(&bakeRunningDiagnostic,bakeInput);
            std::atomic_store(&bakeRunningInputs,std::shared_ptr<const std::vector<BakedTable>>(runningList));
            bakeWorkerAction=action;bakeHash=0;bakeDiscontinuities=0;bakeStations=0;
            const auto hw=std::thread::hardware_concurrency();const unsigned available=hw>1?hw-1:1;unsigned count=bakeWantedWorkers.load();count=count?std::clamp(count,1u,available):defaultPoolWorkers(hw);
            ensureRebuildPool(count);bakeWorkers=(int)count;
            if(!cfg.adaptiveTail){
                bakeActiveLimit=std::max<size_t>(1,std::min<size_t>(count,(size_t)std::max<unsigned>(std::min(count,2u),(count+(unsigned)maxGroupS-1)/(unsigned)std::max<size_t>(1,maxGroupS))));
            }else{
                const double histBytes=std::max(1.,std::ceil(bakeWorld->sc.length/.001)+2)*R*NB*16*maxGroupS;
                bakeActiveLimit=std::max<size_t>(1,std::min<size_t>(count,(size_t)(128.*1024*1024/histBytes)));
            }
            bakeStarted=std::chrono::steady_clock::now();bakeState=BakeState::Baking;
        }
        for(const auto&jb:bakeJobs)if(!bakeContextTable(jb.table,false)){invalidateBake();return;}
        for(auto&jb:bakeJobs){
            for(auto it=jb.active.begin();it!=jb.active.end();){
                auto&job=*it->second;bool ready=true;
                for(auto&f:job.futures)if(f.valid()&&f.wait_for(std::chrono::seconds(0))!=std::future_status::ready){ready=false;break;}
                if(!ready){++it;continue;}
                bool retry=false;
                try{for(size_t s=0;s<job.futures.size();++s)job.hist[s]=job.futures[s].get();}
                catch(const std::exception&){
                    const bool cancelled=!running||bakeStop->load()||job.preempt!=bakePreempt.load()||!bakeContextTable(jb.table,false);
                    if(!cancelled){stopBakeTasks();endBake(BakeState::Failed,15,bakeWorkerAction);return;}
                    ++bakeCancelled;retry=true;
                }
                double u=job.u;int rays=job.rays;
                if(!retry&&shared&&cfg.adaptiveTail&&!job.hist.empty()){
                    const size_t nSrc=job.hist.size();
                    auto avg=job.hist[0];
                    for(int r=0;r<R;++r)for(int b=0;b<NB;++b)for(size_t k=0;k<avg.nBins;++k){
                        avg.h[r][b][k]=0;avg.h2[r][b][k]=0;
                        for(auto&h:job.hist){avg.h[r][b][k]+=h.h[r][b][k]/(double)nSrc;avg.h2[r][b][k]+=h.h2[r][b][k]/((double)nSrc*(double)nSrc);}
                    }
                    auto raw=avg;finalizeTail(avg);
                    if(tailNeedsMoreRays(raw,avg)&&rays<rayLimit(jb.baseRays)){retry=true;rays=std::min(rayLimit(jb.baseRays),rays*2);}
                }
                if(!retry){
                    const int ns=jb.table.sources;
                    std::vector<double> energy((size_t)ns*R*NB,0.);
                    for(int s=0;s<ns;++s)for(int r=0;r<R;++r)for(int b=0;b<NB;++b)for(double v:job.hist[(size_t)s].h[r][b])energy[((size_t)s*R+r)*NB+b]+=v;
                    bool finite=true;for(double v:energy)finite&=std::isfinite(v)&&v>=0;
                    if(!finite){stopBakeTasks();endBake(BakeState::Failed,12,bakeWorkerAction);return;}
                    jb.cache[u]=std::move(energy);++bakeStations;
                }
                it=jb.active.erase(it);if(retry){queueGroupStation(jb,u,rays);break;}
            }
            jb.initialAt=0;for(double u:jb.initial)if(jb.cache.count(u))++jb.initialAt;
            if(jb.initialAt==jb.initial.size()){
                if(jb.segments.empty())for(size_t j=1;j<jb.nodes.size();++j)jb.segments.push_back({jb.nodes[j-1],jb.nodes[j]});
                while(jb.segmentAt<jb.segments.size()){
                    auto ab=jb.segments[jb.segmentAt];double mid=(ab.first+ab.second)*.5;auto found=jb.cache.find(mid);
                    if(found==jb.cache.end())break;
                    ++jb.segmentAt;
                    double error=bakeError(jb.cache.at(ab.first),found->second,jb.cache.at(ab.second),jb.table.sources,R);
                    auto decision=bakedRefinement(error,jb.nodes.size(),ab.first,ab.second);
                    if(decision==BakedRefinement::Split){jb.nodes.push_back(mid);jb.segments.push_back({ab.first,mid});jb.segments.push_back({mid,ab.second});}
                    else if(decision==BakedRefinement::Approximate){++jb.table.discontinuities;jb.table.residualSegments.push_back({ab.first,ab.second,error});}
                }
            }
            jb.finished=(jb.initialAt==jb.initial.size()&&jb.segmentAt==jb.segments.size()&&jb.active.empty());
        }
        bool allFinished=true;for(const auto&jb:bakeJobs)allFinished&=jb.finished;
        if(allFinished){
            if(!bakeRequested.load())return;
            for(const auto&jb:bakeJobs)if(!bakeContextTable(jb.table,false))return;
            for(auto&jb:bakeJobs){
                jb.table.u=jb.nodes;std::sort(jb.table.u.begin(),jb.table.u.end());
                for(double u:jb.table.u){const auto&e=jb.cache.at(u);jb.table.energy.insert(jb.table.energy.end(),e.begin(),e.end());}
                if(jb.table.hasDeclaredJoint()&&!jb.table.u.empty()){
                    if(jb.table.u.front()>0.){
                        const auto firstE=jb.cache.at(jb.table.u.front());
                        jb.table.u.insert(jb.table.u.begin(),0.);
                        jb.table.energy.insert(jb.table.energy.begin(),firstE.begin(),firstE.end());
                    }
                    if(jb.table.u.back()<1.){
                        const auto lastE=jb.cache.at(jb.table.u.back());
                        jb.table.u.push_back(1.);
                        jb.table.energy.insert(jb.table.energy.end(),lastE.begin(),lastE.end());
                    }
                }
                for(double v:jb.table.energy)jb.table.amplitude.push_back(std::sqrt(v));
                jb.table.selectedRays=jb.baseRays;
            }
            BakedTableSlot published;
            std::map<size_t,BakedTable> byIndex;
            for(const auto&jb:bakeJobs)byIndex[jb.groupIndex]=jb.table;
            size_t totalGroups=byIndex.size()+bakeKeptTables.size();
            published.tables.reserve(totalGroups);
            {
                std::lock_guard<std::mutex>lock(bakeRequestMx);
                if(!bakeRequested.load()||bakeAction.load()!=bakeWorkerAction)return;
                size_t keptCursor=0;
                for(size_t gi=0;gi<bakeGroups.size();++gi){
                    auto it=byIndex.find(gi);
                    if(it!=byIndex.end()){
                        if(!bakeContextTable(it->second,true))return;
                        published.tables.push_back(it->second);
                    }else if(keptCursor<bakeKeptTables.size()){
                        if(!bakeContextTable(bakeKeptTables[keptCursor],true))return;
                        published.tables.push_back(bakeKeptTables[keptCursor++]);
                    }
                }
                if(published.tables.empty())return;
                int totalDiscont=0;
                for(const auto&t:published.tables)totalDiscont+=t.discontinuities;
                auto listPtr=std::make_shared<std::vector<BakedTable>>(published.tables);
                auto firstPtr=std::make_shared<BakedTable>(published.tables.front());
                std::atomic_store(&bakeDiagnostic,std::shared_ptr<const BakedTable>(firstPtr));
                std::atomic_store(&bakeDiagnosticList,std::shared_ptr<const std::vector<BakedTable>>(listPtr));
                size_t ti=0;
                for(size_t gi=0;gi<bakeGroups.size()&&ti<published.tables.size();++gi){
                    auto&gr=bakeGroups[gi];
                    const auto&t=published.tables[ti++];
                    gr.table=std::make_shared<BakedTable>(t);
                    gr.discontinuities=t.discontinuities;
                    gr.done=(int)t.u.size();gr.total=gr.done;gr.percent=100;gr.dirty=false;gr.touched=true;
                    gr.state=t.discontinuities?BakeState::Approximate:BakeState::Baked;
                    for(int o:gr.spec.objects){const int idx=o<0?0:o+1;if(idx>=0&&idx<(int)bakeObjectTouched.size())bakeObjectTouched[(size_t)idx]=1;}
                }
                bakeHash=combinedTableHash(published.tables);
                bakeDiscontinuities=totalDiscont;
                bakeSlots[(size_t)bakeBack]=std::move(published);
                bakeBack=bakeMiddle.exchange(bakeBack|4,std::memory_order_acq_rel)&3;
                bakeMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-bakeStarted).count();
                bakeDone=(int)0;bakeTotal=(int)0;for(const auto&t:listPtr->empty()?std::vector<BakedTable>{}:*listPtr){bakeDone+=(int)t.u.size();bakeTotal+=(int)t.u.size();}
                bakePercent=100;bakeRequested=false;bakeState=totalDiscont?BakeState::Approximate:BakeState::Baked;
                return;
            }
        }
        auto activeTotal=[&]{size_t c=0;for(const auto&jb:bakeJobs)c+=jb.active.size();return c;};
        bool progressed=true;
        while(progressed&&activeTotal()<bakeActiveLimit){
            progressed=false;
            for(auto&jb:bakeJobs){
                if(activeTotal()>=bakeActiveLimit)break;
                bool queued=false;
                for(double u:jb.initial)if(!jb.cache.count(u)&&!jb.active.count(u)){queueGroupStation(jb,u,jb.baseRays);queued=true;break;}
                if(!queued&&jb.initialAt==jb.initial.size()){
                    for(size_t j=jb.segmentAt;j<jb.segments.size();++j){auto ab=jb.segments[j];double u=(ab.first+ab.second)*.5;if(!jb.cache.count(u)&&!jb.active.count(u)){queueGroupStation(jb,u,jb.baseRays);queued=true;break;}}
                }
                progressed|=queued;
            }
        }
        size_t initDoneSum=0,initTotalSum=0,segDoneSum=0,segTotalSum=0,doneSum=0,totalSum=0;
        for(const auto&jb:bakeJobs){
            initDoneSum+=jb.initialAt;initTotalSum+=jb.initial.size();
            segDoneSum+=jb.segmentAt;segTotalSum+=jb.segments.size();
            doneSum+=jb.cache.size();totalSum+=jb.initial.size()+jb.segments.size();
        }
        {
            std::lock_guard<std::mutex>lock(bakeRequestMx);
            for(const auto&jb:bakeJobs)if(jb.groupIndex<bakeGroups.size()){
                auto&gr=bakeGroups[jb.groupIndex];
                gr.done=(int)jb.cache.size();gr.total=(int)(jb.initial.size()+jb.segments.size());
                gr.percent=jb.finished?100:bakedProgressPercent(jb.initialAt,jb.initial.size(),jb.segmentAt,jb.segments.size());
            }
        }
        bakeDone=(int)doneSum;bakeTotal=(int)totalSum;
        bakePercent.store(std::max(bakePercent.load(),bakedProgressPercent(initDoneSum,initTotalSum,segDoneSum,segTotalSum)));
    }
    void bakeWorkerStep(){const auto action=bakeAction.load();try{bakeWorkerStepImpl();}catch(const std::exception&){stopBakeTasks();endBake(BakeState::Failed,15,action);}}

    void bakeAudioStep(bool jumped) {
        bakeAudioValid=false;
        std::fill(bakeAudioSourceValid.begin(),bakeAudioSourceValid.end(),0);
        if(!bakeEnabled.load(std::memory_order_acquire)){
            passOff.store(false,std::memory_order_relaxed);
            if(shared&&bakeSharedApplied&&!motionExact&&transState.load()==0){
                std::copy(bakeReferenceGain.begin(),bakeReferenceGain.end(),lateGlocal.begin());
                lateGtarget=lateGlocal.data();gainMode=1;
            }
            bakeSharedApplied=false;return;
        }
        if(bakeMiddle.load(std::memory_order_acquire)&4){
            bakeFront=bakeMiddle.exchange(bakeFront,std::memory_order_acq_rel)&3;
            std::fill(bakeProjectHints.begin(),bakeProjectHints.end(),0);
        }
        const auto&tables=bakeSlots[(size_t)bakeFront].tables;
        bool anyApplied=false,sawRecorded=false,recordedOff=false;
        for(size_t ti=0;ti<tables.size();++ti){
            const auto&t=tables[ti];
            if(!bakeContextTable(t,true))continue;
            double pose[4];const int offset=t.path.object<0?S*4:t.path.object*4;
            for(int k=0;k<4;++k)pose[k]=params[offset+k].load(std::memory_order_relaxed);
            double u=0;size_t*hint=ti<bakeProjectHints.size()?&bakeProjectHints[ti]:nullptr;
            bool match=true;
            if(t.path.recorded){
                sawRecorded=true;
                if(bakeJointPose.size()<(size_t)(S+1)*4)continue;
                for(int j=0;j<=S;++j)for(int k=0;k<4;++k)bakeJointPose[4*(size_t)j+(size_t)k]=params[(j==0?S*4:(j-1)*4)+k].load(std::memory_order_relaxed);
                const bool on=t.projectJoint(bakeJointPose.data(),u,(++passProjectTick%64)?hint:nullptr);
                if(!on){recordedOff=true;continue;}
            }else if(t.hasDeclaredJoint()){
                if(bakeJointPose.size()<(size_t)(S+1)*4)continue;
                for(int j=0;j<=S;++j)for(int k=0;k<4;++k)bakeJointPose[4*(size_t)j+(size_t)k]=params[(j==0?S*4:(j-1)*4)+k].load(std::memory_order_relaxed);
                if(!t.projectDeclared(bakeJointPose.data(),u,hint))continue;
            }else{
                if(!t.path.project(pose,u,hint))continue;
                for(size_t j=1;j<t.movingPaths.size();++j){
                    const auto&p=t.movingPaths[j];auto expected=p.at(u);double actual[4];
                    const int moff=p.object<0?S*4:p.object*4;
                    for(int k=0;k<4;++k)actual[k]=params[moff+k].load(std::memory_order_relaxed);
                    if(poseFar(actual,expected.data(),.02,1.)){match=false;break;}
                }
            }
            if(!match)continue;
            if(!t.lookup(u,bakeLookupScratch.data()))continue;
            const size_t srcStride=(size_t)R*NB;
            for(size_t si=0;si<t.groupSources.size();++si){
                const int s=t.groupSources[si];
                if(s>=0&&s<S){
                    std::copy_n(bakeLookupScratch.data()+si*srcStride,srcStride,bakeAudioEnergy.data()+(size_t)s*srcStride);
                    bakeAudioSourceValid[(size_t)s]=1;
                }
            }
            anyApplied=true;
        }
        passOff.store(sawRecorded&&recordedOff,std::memory_order_relaxed);
        if(!anyApplied){
            bakePendingJump=false;
            if(shared&&bakeSharedApplied&&!motionExact&&transState.load(std::memory_order_relaxed)==0){
                std::copy(bakeReferenceGain.begin(),bakeReferenceGain.end(),lateGlocal.begin());
                lateGtarget=lateGlocal.data();gainMode=1;bakeSharedApplied=false;
            }
            return;
        }
        bakeAudioValid=true;bakeWasUsed=true;++bakeLookups;
        if(!shared||motionExact||transState.load(std::memory_order_relaxed)!=0){bakePendingJump|=jumped;return;}
        const bool snap=jumped||bakePendingJump;bakePendingJump=false;
        double lst[4];for(int k=0;k<4;++k)lst[k]=params[S*4+k].load(std::memory_order_relaxed);
        const bool lstAtRef=bakeAudioRefPose.size()>=4&&!poseFar(lst,bakeAudioRefPose.data(),cfg.motionDeadbandM,cfg.motionDeadbandDeg);
        for(int s=0;s<S;++s){
            if(!bakeAudioSourceValid[(size_t)s])continue;
            double srcPose[4];for(int k=0;k<4;++k)srcPose[k]=params[s*4+k].load(std::memory_order_relaxed);
            const bool srcAtRef=lstAtRef&&bakeAudioRefPose.size()>=(size_t)(8+4*s)&&!poseFar(srcPose,bakeAudioRefPose.data()+4+4*s,cfg.motionDeadbandM,cfg.motionDeadbandDeg);
            for(int r=0;r<R;++r)for(int b=0;b<NB;++b){
                const size_t k=((size_t)r*S+s)*8+b;
                if(srcAtRef){
                    lateGlocal[k]=bakeReferenceGain[k];
                    if(snap)lateGuse[k]=lateGlocal[k];
                }else{
                    const double n=bakeAudioNorm[(size_t)r*NB+b];
                    if(n>0){lateGlocal[k]=(float)std::sqrt(bakeAudioEnergy[((size_t)s*R+r)*NB+b]/n);if(snap)lateGuse[k]=lateGlocal[k];}
                }
            }
        }
        lateGtarget=lateGlocal.data();gainMode=1;bakeSharedApplied=true;
    }

    // E9/E16 per-source: modify only the old bank's spectrum. The fresh exact bank remains untouched.
    void bakeEqualize(const TailConv& t,const Slot& sl,int r,float* re,float* im) const {
        if(shared||t.bakeSource<0||(!bakeEnabled.load(std::memory_order_relaxed)&&!bakeWasUsed))return;
        const bool srcValid=t.bakeSource<S && bakeAudioSourceValid[(size_t)t.bakeSource]!=0;
        double source[4],listener[4];for(int k=0;k<4;++k){source[k]=params[t.bakeSource*4+k].load(std::memory_order_relaxed);listener[k]=params[S*4+k].load(std::memory_order_relaxed);}
        const bool atRef=!poseFar(source,sl.bakePose.data(),cfg.motionDeadbandM,cfg.motionDeadbandDeg)&&!poseFar(listener,sl.bakePose.data()+4,cfg.motionDeadbandM,cfg.motionDeadbandDeg);
        auto& gains=sl.bakeEqGain[(size_t)r];bool active=false;
        const bool jumped=sl.bakeJumpSeen!=audioJumpSeen;if(r+1==t.R)sl.bakeJumpSeen=audioJumpSeen;
        for(int b=0;b<NB;++b){float target=1;size_t k=(size_t)r*NB+b;if(srcValid&&!atRef&&sl.bakeEnergy[k]>0)target=(float)std::clamp(std::sqrt(bakeAudioEnergy[((size_t)t.bakeSource*R+r)*NB+b]/sl.bakeEnergy[k]),1e-6,1e6);
            float old=gains[(size_t)b];float next=jumped&&srcValid?target:std::clamp(old+(target-old)*.25f,old*.944060876f,old*1.05925373f);
            if(std::abs(next-target)<1e-6f*target)next=target;gains[(size_t)b]=next;active|=next!=1.f;
        }
        if(!active)return;
        float power[NB];for(int b=0;b<NB;++b)power[b]=gains[(size_t)b]*gains[(size_t)b];int bin=0;
       #if VERB_REQUIRE_AVX2
        for(;bin+8<=t.bins;bin+=8){auto sum=_mm256_setzero_ps();for(int b=0;b<NB;++b)sum=_mm256_add_ps(sum,_mm256_mul_ps(_mm256_loadu_ps(t.bakeWeights[b].data()+bin),_mm256_set1_ps(power[b])));auto g=_mm256_sqrt_ps(_mm256_max_ps(sum,_mm256_setzero_ps()));_mm256_storeu_ps(re+bin,_mm256_mul_ps(_mm256_loadu_ps(re+bin),g));_mm256_storeu_ps(im+bin,_mm256_mul_ps(_mm256_loadu_ps(im+bin),g));}
       #endif
        for(;bin<t.bins;++bin){float square=0;for(int b=0;b<NB;++b)square+=t.bakeWeights[b][(size_t)bin]*power[b];float gain=std::sqrt(std::max(0.f,square));re[bin]*=gain;im[bin]*=gain;}
    }
