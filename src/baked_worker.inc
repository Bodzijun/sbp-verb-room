    // Audio writes the samples. Ready -> reading CAS excludes arm and clear while control copies.
    static constexpr size_t passCap=4096;
    std::vector<double> passBuffer;
    std::atomic<int> passCommand{0},passState{0},passCount{0}; // 0 empty,1 armed,2 recording,3 ready,4 reading
    std::atomic<bool> passClearPending{false};
    std::atomic_flag passWrite = ATOMIC_FLAG_INIT;
    struct PassWriteRelease {
        std::atomic_flag& flag;
        ~PassWriteRelease() { flag.clear(std::memory_order_release); }
    };
    std::atomic<uint64_t> passGeneration{0};
    uint64_t passConsumed=0;
    std::vector<BakedPath> recordedPaths,declaredPaths;
    std::vector<BakedYawMatch> recordedYawMatch;
    bool recordedYawMatchChanged=false;
    bool bakeForcedBlock=false,bakeForcedApplied=false; // E19: a Look at dependency the tables cannot represent (control thread, under bakeRequestMx)
    std::vector<double> bakeJointPose;
    std::atomic<bool> passOff{false};
    uint32_t passProjectTick=0;
    void applyBakedPathsLocked(const std::vector<BakedPath>& declared);
    bool finishPassClear() noexcept {
        // Caller owns passWrite. Defer if exportRecordedPass is reading the buffer.
        int expected=passState.load(std::memory_order_acquire);
        if(expected==4||!passState.compare_exchange_strong(expected,4,std::memory_order_acq_rel))return false;
        passCount.store(0,std::memory_order_relaxed);
        passOff.store(false,std::memory_order_relaxed);
        passGeneration.fetch_add(1,std::memory_order_acq_rel);
        passState.store(0,std::memory_order_release);
        passClearPending.store(false,std::memory_order_release);
        return true;
    }
    void recordPassAudio(bool playing) noexcept {
        if(passWrite.test_and_set(std::memory_order_acquire))return;
        const PassWriteRelease release{passWrite};
        if(passClearPending.load(std::memory_order_acquire)&&!finishPassClear())return;
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
        if(passClearPending.load(std::memory_order_acquire)){
            if(!passWrite.test_and_set(std::memory_order_acquire)){
                const PassWriteRelease release{passWrite};
                if(passClearPending.load(std::memory_order_acquire))finishPassClear();
            }
            if(passClearPending.load(std::memory_order_acquire))return;
        }
        const uint64_t gen=passGeneration.load(std::memory_order_acquire);
        if(gen==passConsumed&&!recordedYawMatchChanged)return;
        const int st=passState.load(std::memory_order_acquire);
        if(st==0){recordedPaths.clear();passConsumed=gen;recordedYawMatchChanged=false;return;}
        int ready=3;if(!passState.compare_exchange_strong(ready,4,std::memory_order_acq_rel))return;
        // Always release ownership, including on allocation failure.
        struct Release { std::atomic<int>& state;~Release(){state.store(3,std::memory_order_release);} } release{passState};
        const size_t n=(size_t)passCount.load(std::memory_order_acquire),stride=4*((size_t)S+1);
        std::vector<BakedPath> next;
        if(n>=2&&n<=passCap&&passBuffer.size()>=n*stride){
            std::vector<double> distance(n,0.);
            for(size_t i=1;i<n;++i){double e=0;for(size_t k=0;k<stride;++k){double v=passBuffer[i*stride+k]-passBuffer[(i-1)*stride+k];if(k%4==3)v=std::remainder(v,360.)*.01;e+=v*v;}distance[i]=distance[i-1]+std::sqrt(e);}
            if(distance.back()>0)for(int j=0;j<=S;++j){BakedPath p;p.object=j-1;p.group=2000;p.recorded=true;p.recording=gen;p.distance=distance;
                for(size_t i=0;i<n;++i){std::array<double,4> v;for(int k=0;k<4;++k)v[k]=passBuffer[i*stride+4*j+k];p.poses.push_back(v);}
                if((size_t)j<recordedYawMatch.size()&&recordedYawMatch[(size_t)j].free){
                    const auto& match=recordedYawMatch[(size_t)j];
                    const auto first=match.transform(p.poses.front().data(),-1.);
                    const bool moved=std::any_of(p.poses.begin()+1,p.poses.end(),[&](const auto& raw){
                        const auto q=match.transform(raw.data(),-1.);
                        return std::hypot(std::hypot(q[0]-first[0],q[1]-first[1]),q[2]-first[2])>1e-6||
                               std::abs(std::remainder(q[3]-first[3],360.))>1e-6;
                    });
                    // Keep the old metric for participants whose entire captured pose was static.
                    if(moved){
                        p.yawMatch=match;
                        for(auto& v:p.poses)v=match.transform(v.data(),-1.);
                    }
                }
                next.push_back(std::move(p));}
        }
        recordedPaths=std::move(next);passConsumed=gen;recordedYawMatchChanged=false;
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
        double queuedMs=0;int activeAtQueue=0;
        std::vector<std::future<TailHist>> futures;
        std::vector<TailHist> hist;
        std::vector<int> usedRays; // P3: rays actually used per source (adaptive initial probes may double)
    };
    struct GroupBakeJob {
        size_t groupIndex=0;
        BakedTable table;
        std::map<double,std::shared_ptr<BakeBatch>> active;
        std::map<double,std::vector<double>> cache;
        std::map<double,std::vector<double>> varCache; // P3: per station sum of squared ray contributions [(s*R+r)*NB+b] (MC variance)
        std::map<double,std::vector<double>> winCache,winEssCache; // P6: per-window energy & ESS [((s*W+w)*R+r)*NB+b]
        std::map<double,int> raysAt;                   // P3: rays each cached station was traced with
        std::vector<double> nodes,initial;
        std::vector<std::pair<double,double>> segments;
        size_t initialAt=0,segmentAt=0;
        double progressShown=0.;size_t progressDone=0;  // P2: honest progress state (worker thread only)
        std::vector<double> stationDurations;           // P3: completed station durations, for in-flight progress credit
        int baseRays=0;
        int selectedRays=0;bool raysDecided=false;      // P3: ONE ray budget per path, from the worst initial station
        std::vector<double> retracePending;             // P3: stations awaiting re-trace at the selected budget (queued under bakeActiveLimit)
        bool finished=false;
        double expectedStationMs() const {
            if(!stationDurations.empty()){
                std::vector<double> d=stationDurations;std::sort(d.begin(),d.end());
                return d[d.size()/2];
            }
            return 4600.*std::max(1.,(double)(raysDecided?selectedRays:baseRays)/48000.); // P1 median at 48k, display-only prior
        }
    };
    std::vector<GroupBakeJob> bakeJobs;
    std::vector<BakedTable> bakeKeptTables;
    // P4: bounded undo cache of published tables keyed by content signature (control/worker side only,
    // under bakeRequestMx). Dies with the engine; clearPathBake empties it; cancelled/failed bakes never enter.
    struct BakeCacheEntry { uint64_t sig=0; BakedTable table; bool saved=false; }; // saved: came from a saved project, not from this session
    std::vector<BakeCacheEntry> bakeCache;
    std::atomic<int> bakeRestored{0}; // 0 traced, 1 restored from saved state, 2 restored from the undo cache
    uint64_t bakeRestoreTriedKey=0;   // throttle: skip restore attempts while nothing relevant changed
    // P1 diagnostics (worker writes under bakeRequestMx; cheap steady_clock only).
    std::vector<Engine::BakeTraceStation> bakeTraceStations;
    std::vector<Engine::BakeTraceDecision> bakeTraceDecisions;
    double bakeMaxGapMs=0;uint64_t bakeStepCalls=0,bakeWaitSteps=0;
    std::chrono::steady_clock::time_point bakeLastStepAt{};
#if defined(_WIN32)
    std::shared_ptr<bake_diag::Bake> requestedDiagnostics; // bakeRequestMx
    std::shared_ptr<bake_diag::Bake> stationDiagnostics; // coordinator; copied into station closures
#endif
    double bakeMs() const {return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-bakeStarted).count();}
    size_t bakeActiveLimit=1;
    std::vector<size_t> bakeProjectHints;
    std::vector<uint8_t> bakeAudioSourceValid;
    std::vector<double> bakeLookupScratch,bakeLookupWinEnergyScratch,bakeLookupWinEssScratch;
    uint64_t bakeWorkerAction=0,bakeFallbackAction=0;
    uint64_t bakeQueuedAction=0,bakeQueuedStream=0;
    std::vector<double> bakeQueuedPose;
    std::chrono::steady_clock::time_point bakeQueuedWall;
    std::chrono::steady_clock::time_point bakeStarted;
    std::vector<double> bakeAudioEnergy,bakeAudioNorm,bakeAudioWinEnergy,bakeAudioWinEss;
    std::vector<float> bakeReferenceGain;
    bool bakeSharedApplied=false,bakePendingJump=false;
    std::array<std::vector<double>,2> bakeNormBuffers;
    std::vector<double> bakeTransNorm;
    std::vector<double> bakeAudioRefPose,bakeTransRefPose;
    std::array<std::vector<double>,2> bakeRefPoseBuffers;
    bool bakeAudioValid=false,bakeWasUsed=false;

    bool bakeContextTable(const BakedTable& t,bool requirePublishedU=true) const noexcept {
        if(requirePublishedU && t.u.size()<2)return false;
        if(t.estimator!=kBakeEstimatorVersion)return false; // P3: tables from another estimator are stale, never played back
        if(t.path.recorded&&(passClearPending.load(std::memory_order_acquire)||t.path.recording!=passGeneration.load(std::memory_order_acquire)||passCommand.load(std::memory_order_acquire)==1||passState.load(std::memory_order_acquire)==1||passState.load(std::memory_order_acquire)==2))return false;
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
    // P4: cross-session content signature of one bake group (control/worker thread; never the audio thread).
    // Covers everything the traced stations depend on; see baked_persist.hpp for the include/exclude list.
    bool hasFixedPatternInGroup(const BakedGroupSpec& sp) const {
        for (int s : sp.groupSources)
            if (s >= 0 && s < S && srcs[(size_t)s]->directivityState.controlRead().fixedPattern) return true;
        return false;
    }
    uint64_t groupContentSig(const BakedGroupSpec& sp, bool forceUnfixed = false) const {
        std::shared_ptr<World> w;{std::lock_guard<std::mutex>lock(worldMx);w=latest;}
        if(!w)return 0;
        uint64_t h=bakeMixI32((int32_t)kBakePersistVersion,1469598103934665603ull);
        h=bakeMixI32(kBakeEstimatorVersion,h);
        h=bakeMixU64(bakeSceneContentHash(w->sc),h);
        h=bakeMixF64(currentAbsorption.load(std::memory_order_relaxed),h);
        h=bakeMixI32(cfg.erOrder,h);h=bakeMixI32(cfg.tailRays,h);
        h=bakeMixU8(cfg.adaptiveTail?1:0,h);h=bakeMixU8(shared?1:0,h);
        h=bakeMixI32(cfg.spatialTaps,h);h=bakeMixF64(cfg.spatialRadius,h);
        for(int g=0;g<3;++g)h=bakeMixF64(requestedDecay[(size_t)g].load(std::memory_order_relaxed),h);
        h=bakeMixF64(sr,h);h=bakeMixI32(S,h);h=bakeMixI32(R,h);
        const auto live=livePose();
        // Poses are quantised (1 mm / 0.01 deg): a saved/reloaded pose differs from the live one by ~1.5e-5 (fixed-point jitter).
        auto mixPose=[&](size_t base){for(size_t k=0;k<4;++k)h=bakeMixU64((uint64_t)std::llround(live[base+k]*(k==3?100.:1000.)),h);};
        if(!sp.movesListener)mixPose(0);
        for(int j=1;j<=S;++j){
            bool moving=false;for(int o:sp.objects)if(o==j-1){moving=true;break;}
            if(moving)continue;
            mixPose((size_t)j*4);
        }
        for(int s:sp.groupSources){
            if(s<0||s>=S)continue;
            h=bakeMixF64(params[S*4+8+s].load(std::memory_order_relaxed),h);
            auto st=srcs[(size_t)s]->directivityState.controlRead();
            if(forceUnfixed)st.fixedPattern=false;
            h=bakeMixU64(bakeDirectivityStateHash(st),h);
        }
        h=bakeMixU64(sp.paths.size(),h);
        for(const auto&p:sp.paths)h=bakeMixU64(bakePathContentHash(p),h);
        return h;
    }
    struct GroupSigMatch {
        uint64_t canonical = 0;
        uint64_t legacyUnfixed = 0;
        bool matches(uint64_t candidate, bool allowLegacy) const noexcept {
            return candidate == canonical || (allowLegacy && legacyUnfixed != canonical && candidate == legacyUnfixed);
        }
    };
    GroupSigMatch groupSigMatch(const BakedGroupSpec& sp, bool includeLegacy) const {
        const uint64_t can = groupContentSig(sp);
        const uint64_t leg = (includeLegacy && hasFixedPatternInGroup(sp)) ? groupContentSig(sp, true) : can;
        return {can, leg};
    }
    // P4: re-stamp the session-local identity fields of a content-verified table so the cheap audio-thread
    // check (bakeContextTable) accepts it. Only called AFTER the content signature matched the live scene.
    void restampBakedTableLocked(BakedTable& t) const {
        t.geometry=geomSerial.load(std::memory_order_relaxed);
        t.quality=qualitySerial.load(std::memory_order_relaxed);
        t.request=bakeRevision.load();t.job=bakeAction.load();
        const uint64_t gen=passGeneration.load(std::memory_order_acquire);
        if(t.path.recorded)t.path.recording=gen;
        t.objRevisions.resize(t.movingPaths.size());
        for(size_t i=0;i<t.movingPaths.size();++i){
            auto&mp=t.movingPaths[i];
            if(mp.recorded)mp.recording=gen;
            const int idx=mp.object<0?0:mp.object+1;
            t.objRevisions[i]=(idx>=0&&idx<=S)?bakeObjectRevision[(size_t)idx].load(std::memory_order_acquire):~0ull;
        }
    }
    void pushBakeCacheLocked(const BakedTable& t,bool saved=false){
        if(t.contentSig==0||t.u.size()<2||t.estimator!=kBakeEstimatorVersion)return;
        const uint64_t th=t.hash();
        for(auto it=bakeCache.begin();it!=bakeCache.end();++it)
            if(it->sig==t.contentSig&&it->table.hash()==th){bakeCache.erase(it);break;}
        bakeCache.push_back(BakeCacheEntry{t.contentSig,t,saved});
        while(bakeCache.size()>kBakeCacheCap)bakeCache.erase(bakeCache.begin());
        bakeRestoreTriedKey=0;
    }
    uint64_t bakeRestoreKey() const {
        uint64_t h=bakeMixU64(geomSerial.load(),1469598103934665603ull);
        h=bakeMixU64(qualitySerial.load(),h);
        h=bakeMixU64(bakeRevision.load(),h);
        h=bakeMixU64(bakeAction.load(),h);
        h=bakeMixU64(passGeneration.load(),h);
        for(int j=0;j<=S;++j)for(int k=0;k<4;++k)h=bakeMixF64(params[(j==0?S*4:(j-1)*4)+k].load(std::memory_order_relaxed),h);
        return h;
    }
    // P4: the publication path shared by a finished worker bake, a saved-state install and an undo restore.
    // Caller holds bakeRequestMx; tables are in bakeGroups order and already content-verified. The audio
    // thread keeps using the same lock-free triple-buffer swap; nothing here runs on it.
    void publishBakedTablesLocked(std::vector<BakedTable> tables,int restoredSource,bool cachePush){
        if(tables.empty()||tables.size()!=bakeGroups.size())return;
        int totalDiscont=0;for(const auto&t:tables)totalDiscont+=t.discontinuities;
        auto listPtr=std::make_shared<std::vector<BakedTable>>(tables);
        auto firstPtr=std::make_shared<BakedTable>(tables.front());
        std::atomic_store(&bakeDiagnostic,std::shared_ptr<const BakedTable>(firstPtr));
        std::atomic_store(&bakeDiagnosticList,std::shared_ptr<const std::vector<BakedTable>>(listPtr));
        for(size_t gi=0;gi<bakeGroups.size();++gi){
            auto&gr=bakeGroups[gi];const auto&t=tables[gi];
            gr.table=std::make_shared<BakedTable>(t);
            gr.discontinuities=t.discontinuities;
            gr.done=(int)t.u.size();gr.total=gr.done;gr.percent=100;gr.dirty=false;gr.touched=true;
            gr.state=t.discontinuities?BakeState::Approximate:BakeState::Baked;
            for(int o:gr.spec.objects){const int idx=o<0?0:o+1;if(idx>=0&&idx<(int)bakeObjectTouched.size())bakeObjectTouched[(size_t)idx]=1;}
        }
        bakeHash=combinedTableHash(tables);
        bakeDiscontinuities=totalDiscont;
        BakedTableSlot published;published.tables=std::move(tables);
        bakeSlots[(size_t)bakeBack]=std::move(published);
        bakeBack=bakeMiddle.exchange(bakeBack|4,std::memory_order_acq_rel)&3;
        bakeDone=(int)0;bakeTotal=(int)0;for(const auto&t:*listPtr){bakeDone+=(int)t.u.size();bakeTotal+=(int)t.u.size();}
        bakePercent=100;bakeRequested=false;bakeState=totalDiscont?BakeState::Approximate:BakeState::Baked;
        bakeWaitReason=0;bakeRestored=restoredSource;
        if(cachePush)for(const auto&t:*listPtr)pushBakeCacheLocked(t);
    }
    // P4: auto-restore after Undo/Redo (or an exact move-back). Every group must be satisfiable: its current
    // table still valid, or a cached table whose content signature equals the freshly computed one. Never
    // serves a table whose signature differs; never starts a worker. Caller holds bakeRequestMx.
    bool tryRestoreBakedCacheLocked(int source){
        if(!bakeEnabled.load()||bakeRequested.load()||bakeGroups.empty()||cfg.motionMode<2)return false;
        const uint64_t key=bakeRestoreKey();
        if(key==bakeRestoreTriedKey)return false;
        std::vector<BakedTable> list;list.reserve(bakeGroups.size());
        bool anyRestored=false,anySaved=false;
        for(const auto&gr:bakeGroups){
            // A table that is valid again (e.g. the pose returned exactly) is re-promoted even without a cache hit.
            if(gr.table&&gr.table->u.size()>=2&&bakeContextTable(*gr.table,true)){
                list.push_back(*gr.table);
                if(gr.state!=BakeState::Baked&&gr.state!=BakeState::Approximate)anyRestored=true;
                continue;
            }
            const auto sigs=groupSigMatch(gr.spec,true);
            const BakedTable* hit=nullptr;bool hitSaved=false;
            for(auto it=bakeCache.rbegin();it!=bakeCache.rend();++it)
                if(sigs.matches(it->sig,it->saved)&&it->table.movingPaths==gr.spec.paths&&it->table.movesListener==gr.spec.movesListener){hit=&it->table;hitSaved=it->saved;break;}
            if(!hit){bakeRestoreTriedKey=key;return false;}
            BakedTable t=*hit;if(hitSaved)anySaved=true;
            restampBakedTableLocked(t);
            if(t.sources!=(int)gr.spec.groupSources.size()||t.receivers!=R||t.groupSources!=gr.spec.groupSources||!bakeContextTable(t,true)){bakeRestoreTriedKey=key;return false;}
            list.push_back(std::move(t));anyRestored=true;
        }
        if(!anyRestored){bakeRestoreTriedKey=key;return false;}
        publishBakedTablesLocked(std::move(list),anySaved?1:source,false);
        bakeRestoreTriedKey=0;
        return true;
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
    uint64_t bakeObservedStream = 0;
    std::chrono::steady_clock::time_point bakeStreamChangedAt{};
    int bakePriority(const World& world) {
        const auto now = std::chrono::steady_clock::now();
        const auto stream = streamPos.load(std::memory_order_acquire);
        if (bakeStreamChangedAt.time_since_epoch().count() == 0 || stream != bakeObservedStream) {
            bakeObservedStream = stream;
            bakeStreamChangedAt = now;
        }
        const bool audioIdle = !cfg.manualWorker &&
            std::chrono::duration<double>(now - bakeStreamChangedAt).count() >= cfg.motionSettleSec;
        const auto geometry = geomSerial.load();
        if (world.serial != geometry || workerQuality != qualitySerial.load()) return shared ? 2 : 5;
        if (poseScaleReady.load(std::memory_order_acquire) != world.constructionPoseScale) return 16;

        // Stations trace the latest World independently of the live IR slots.
        // Keep rebuild priority while callbacks progress. A stopped callback
        // cannot release a slot or finish a crossfade.
        bool waitingForAudio = shared && sharedTailHandoverWaiting;
        if (!shared) {
            for (const auto& sp : srcs) waitingForAudio |= tailHandoverWaiting(sp->tail);
        }
        if (jumpSerial.load(std::memory_order_acquire) != workerJumpSeen.load(std::memory_order_relaxed) && !(audioIdle && waitingForAudio)) return 1;
        if (shared) {
            if ((sLastGeom != geometry || sLastQuality != workerQuality) && !(audioIdle && waitingForAudio)) return 2;
        } else {
            for (const auto& sp : srcs) {
                if ((sp->lastGeom != geometry || sp->lastQuality != workerQuality) &&
                    !(audioIdle && tailHandoverWaiting(sp->tail))) return 5;
            }
        }
        if (audioIdle && waitingForAudio) return 0;
        if(!anchorValid)return 0;
        // Match the normal worker's resolved anchor; raw wall-line coordinates can differ permanently.
        bool moved=false;for(int j=0;j<=S;++j){const double*ref=j==0?(shared?sLastLst:srcs.front()->lastLst):srcs[(size_t)j-1]->lastPose;moved|=poseFar(anchor.data()+4*j,ref,cfg.motionDeadbandM,cfg.motionDeadbandDeg);}
        return moved&&((double)(stream-anchorStream)>=cfg.motionSettleSec*sr || (!cfg.manualWorker&&std::chrono::duration<double>(now-anchorWall).count()>=cfg.motionSettleSec))?7:0;
    }
    // P3: does the RAW integrated energy of this trace still carry too much Monte-Carlo noise?
    // sigma_rel = sqrt(sum h2)/sum h per receiver/band (h2 = sum of squared per-ray contributions).
    static bool bakeNeedsMoreRays(const TailHist& raw) {
        double largest=0;
        for(const auto&r:raw.h)for(const auto&band:r){double e=0;for(double v:band)e+=v;largest=std::max(largest,e);}
        if(largest<=0)return false;
        for(size_t r=0;r<raw.h.size();++r)for(int b=0;b<NB;++b){
            double e=0,v=0;
            for(size_t k=0;k<raw.nBins;++k){e+=raw.h[r][b][k];if(!raw.h2.empty())v+=raw.h2[r][b][k];}
            if(e>largest*1e-5&&v>0&&std::sqrt(v)>kBakeRayNoiseTarget*e)return true;
        }
        return false;
    }
    // P3: worst interpolation error per band (peak-source term and source-sum term), optionally minus the
    // estimated Monte-Carlo noise in dB. Same maths as the former scalar bakeError, kept per band so the split
    // rule can apply the band thresholds kBakeSplitThresholdDb.
    static void bakeErrorBands(const std::vector<double>& a,const std::vector<double>& mid,const std::vector<double>& b,int ns,int nr,const std::vector<double>*noise,const std::vector<double>*sumNoise,std::array<double,NB>& out) {
        out.fill(0.);
        for(int r=0;r<nr;++r)for(int band=0;band<NB;++band){double peak=0,sum=0,predSum=0;
            for(int s=0;s<ns;++s)peak=std::max(peak,mid[((size_t)s*nr+r)*NB+band]);
            for(int s=0;s<ns;++s){size_t k=((size_t)s*nr+r)*NB+band;double amp=.5*(std::sqrt(std::max(0.,a[k]))+std::sqrt(std::max(0.,b[k]))),pred=amp*amp;sum+=mid[k];predSum+=pred;if(mid[k]>=peak*1e-4&&peak>0)out[band]=std::max(out[band],std::max(0.,std::abs(10*std::log10(std::max(1e-30,pred)/std::max(1e-30,mid[k])))-(noise?(*noise)[k]:0.)));}
            if(sum>0)out[band]=std::max(out[band],std::max(0.,std::abs(10*std::log10(std::max(1e-30,predSum)/sum))-(sumNoise?(*sumNoise)[(size_t)r*NB+band]:0.)));
        }
    }
    static double bakeError(const std::vector<double>& a,const std::vector<double>& mid,const std::vector<double>& b,int ns,int nr,const std::vector<double>*noise=nullptr,const std::vector<double>*sumNoise=nullptr) {
        std::array<double,NB> bands;bakeErrorBands(a,mid,b,ns,nr,noise,sumNoise,bands);
        return *std::max_element(bands.begin(),bands.end());
    }
    // P3: Monte-Carlo noise of the midpoint test, from the raw second moments: Var(integrated energy) ~ sum of
    // squared per-ray contributions (h2). Propagated through pred=((sqrt(a)+sqrt(b))/2)^2 and the dB ratio.
    // Rays share their seed across stations (common random numbers), so this is a conservative upper bound on
    // the noise of the DIFFERENCE; subtracting it keeps the split rule from chasing Monte-Carlo steps.
    static void bakeNoiseDb(const std::vector<double>& a,const std::vector<double>& va,const std::vector<double>& m,const std::vector<double>& vm,const std::vector<double>& b,const std::vector<double>& vb,int ns,int nr,std::vector<double>& noise,std::vector<double>& sumNoise) {
        const double kDb=10./std::log(10.);
        noise.assign((size_t)ns*nr*NB,0.);sumNoise.assign((size_t)nr*NB,0.);
        for(int r=0;r<nr;++r)for(int band=0;band<NB;++band){
            double sp2=0,sm2=0,predSum=0,sum=0;
            for(int s=0;s<ns;++s){const size_t k=((size_t)s*nr+r)*NB+band;
                const double sa=std::sqrt(std::max(0.,a[k])),sb=std::sqrt(std::max(0.,b[k]));
                const double amp=.5*(sa+sb),pred=amp*amp;
                double vp=0;
                if(sa>0)vp+=(amp/(2*sa))*(amp/(2*sa))*va[k];
                if(sb>0)vp+=(amp/(2*sb))*(amp/(2*sb))*vb[k];
                predSum+=pred;sum+=m[k];sp2+=vp;sm2+=vm[k];
                if(pred>0&&m[k]>0)noise[k]=kDb*std::sqrt(vp/(pred*pred)+vm[k]/(m[k]*m[k]));
            }
            if(predSum>0&&sum>0)sumNoise[(size_t)r*NB+band]=kDb*std::sqrt(sp2/(predSum*predSum)+sm2/(sum*sum));
        }
    }
    // P3: the production split test on cached stations (band thresholds + MC-noise correction).
    BakedRefinement bakeSegmentSplit(const GroupBakeJob& jb,double a,double mid,double b,double& rawWorst) const {
        const auto& ea=jb.cache.at(a);const auto& em=jb.cache.at(mid);const auto& eb=jb.cache.at(b);
        const auto& va=jb.varCache.at(a);const auto& vm=jb.varCache.at(mid);const auto& vb=jb.varCache.at(b);
        std::vector<double> noise,sumNoise;
        bakeNoiseDb(ea,va,em,vm,eb,vb,jb.table.sources,R,noise,sumNoise);
        std::array<double,NB> bands;
        bakeErrorBands(ea,em,eb,jb.table.sources,R,&noise,&sumNoise,bands);
        rawWorst=bakeError(ea,em,eb,jb.table.sources,R);
        return bakedRefinementBands(bands,jb.nodes.size(),a,b);
    }
    bool bakeActiveEmpty() const {for(const auto&jb:bakeJobs)if(!jb.active.empty())return false;return true;}
    void stopBakeTasks(){if(bakeStop)bakeStop->store(true);for(auto&jb:bakeJobs)jb.active.clear();bakeJobs.clear();bakeKeptTables.clear();}
    void endBake(BakeState state,int reason,uint64_t expected){
        std::lock_guard<std::mutex>lock(bakeRequestMx);
        if(bakeAction.load()!=expected)return;
        bakeRequested=false;bakePercent=0;bakeState=state;bakeWaitReason=reason;lastEstEpoch=~0ull;bakeRestored=0;
        for(auto&gr:bakeGroups){
            if(state==BakeState::Blocked||state==BakeState::Failed||state==BakeState::Cancelled)gr.state=state;
            else if(gr.table && !bakeContextTable(*gr.table,true)){gr.state=BakeState::Outdated;gr.percent=0;gr.dirty=true;}
        }
    }
    void invalidateBake(){stopBakeTasks();endBake(BakeState::Outdated,11,bakeWorkerAction);}
    void queueGroupStation(GroupBakeJob&jb,double u,int rays,bool adapt){
        auto job=std::make_shared<BakeBatch>();job->u=u;job->rays=rays;job->queuedMs=bakeMs();{size_t c=0;for(const auto&j2:bakeJobs)c+=j2.active.size();job->activeAtQueue=(int)c;}job->hist.resize(jb.table.groupSources.size());job->usedRays.assign(jb.table.groupSources.size(),rays);
        auto pose=jb.table.staticPose;
        for(const auto&path:jb.table.movingPaths){auto v=path.tracePose(u);int offset=path.object<0?0:4+4*path.object;for(int k=0;k<4;++k)pose[(size_t)offset+k]=v[k];}
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
            job->futures.push_back(bakePool->submit([this,world,refTable,stop,receivers,source,preempt,pooled,order,rays,limit,s,jobId,adapt,si,job
#if defined(_WIN32)
                ,diagnostics=stationDiagnostics
#endif
            ]{
#if defined(_WIN32)
                const bake_diag::ScopedDenormalMode testMode(BakePool::testStationDenormalMode);
                const bake_diag::Bake::Job sample(diagnostics.get());
#endif
                auto cancelled=[&]{return !running||stop->load(std::memory_order_relaxed)||bakePreempt.load(std::memory_order_relaxed)!=preempt||jobId!=bakeAction.load(std::memory_order_relaxed)||!bakeContextTable(*refTable,false);};
                if(cancelled())throw std::runtime_error("Bake superseded");
                // P3: every bake trace uses the raised detection sphere (kBakeDetectRadiusMin); the live tail does not.
                if(!cfg.adaptiveTail)return traceTail(world->sc,source,*receivers,order,rays,world->sc.seed+131*s,false,cancelled,{},kBakeDetectRadiusMin);
                if(pooled)return traceTail(world->sc,source,*receivers,order,rays,world->sc.seed+131*s,false,cancelled,{},kBakeDetectRadiusMin);
                if(adapt){
                    // P3 initial probe only: double the rays until the RAW integral's MC noise is under the
                    // target (or the 4x limit); the path-wide budget is then fixed to the worst initial station.
                    int used=rays;TailHist raw;
                    for(;;){
                        raw=traceTail(world->sc,source,*receivers,order,used,world->sc.seed+131*s,false,cancelled,{},kBakeDetectRadiusMin);
                        if(used>=limit||!bakeNeedsMoreRays(raw))break;
                        used=(int)std::min<int64_t>(limit,2ll*used);
                    }
                    job->usedRays[si]=used;
                    return raw; // P3: station level = RAW integrated energy, no reliableEnd/ESS extrapolation
                }
                return traceTail(world->sc,source,*receivers,order,rays,world->sc.seed+131*s,false,cancelled,{},kBakeDetectRadiusMin);
            }));
        }
        jb.active.emplace(u,std::move(job));
    }
    // P1: lookahead. Validation decisions are still taken strictly in segment order (so the 64-node cap and the published table are
    // exactly the same as before); this only starts, ahead of time, stations whose need is already certain or very likely:
    //  - the midpoint of two neighbouring initial nodes as soon as both are computed (instead of after the slowest initial node);
    //  - the child midpoints of a segment whose own midpoint is cached and whose error already demands a split (also recursively),
    //    instead of after every earlier segment has been decided. Results depend only on u (seed fixed per source), never on order.
    // A speculative station that the in-order decision later refuses (node cap) is simply never used and is cancelled at publish.
    static bool bakeLookaheadEnabled(){static const bool on=std::getenv("VERB_BAKE_LOOKAHEAD")!=nullptr;return on;} // experimental, default OFF (P1: no measured wall-time gain)
    bool queueLookaheadStation(GroupBakeJob&jb){
        if(!bakeLookaheadEnabled())return false;
        auto need=[&](double u){return !jb.cache.count(u)&&!jb.active.count(u);};
        if(jb.initialAt<jb.initial.size()){
            for(size_t j=1;j<jb.initial.size();++j){
                const double a=jb.initial[j-1],b=jb.initial[j],mid=(a+b)*.5;
                if(mid>a&&mid<b&&jb.cache.count(a)&&jb.cache.count(b)&&need(mid)){queueGroupStation(jb,mid,jb.baseRays,false);return true;}
            }
            return false;
        }
        if(!jb.raysDecided)return false; // P3: energies may still be re-traced at the path-wide ray budget
        std::vector<std::pair<double,double>> stack(jb.segments.begin()+(std::ptrdiff_t)jb.segmentAt,jb.segments.end());
        size_t predicted=0;
        while(!stack.empty()){
            const auto ab=stack.back();stack.pop_back();
            const double mid=(ab.first+ab.second)*.5;
            if(!(mid>ab.first&&mid<ab.second))continue;
            const auto m=jb.cache.find(mid),ca=jb.cache.find(ab.first),cb=jb.cache.find(ab.second);
            if(m==jb.cache.end()||ca==jb.cache.end()||cb==jb.cache.end())continue;
            double rawWorst=0;
            if(bakeSegmentSplit(jb,ab.first,mid,ab.second,rawWorst)!=BakedRefinement::Split)continue; // P3: same band+noise rule as the in-order decision
            if(jb.nodes.size()+predicted>=kBakeNodeCap)continue;
            ++predicted;
            const std::pair<double,double> kids[2]={{ab.first,mid},{mid,ab.second}};
            for(const auto&k:kids){
                const double km=(k.first+k.second)*.5;
                if(!(km>k.first&&km<k.second))continue;
                if(need(km)){queueGroupStation(jb,km,jb.selectedRays,false);return true;}
                stack.push_back(k);
            }
        }
        return false;
    }
    void bakeWorkerStepImpl() {
        const auto state=bakeState.load();
        if(state==BakeState::Baking){
            const auto nowT=std::chrono::steady_clock::now();
            if(bakeLastStepAt.time_since_epoch().count()!=0){std::lock_guard<std::mutex>lock(bakeRequestMx);bakeMaxGapMs=std::max(bakeMaxGapMs,std::chrono::duration<double,std::milli>(nowT-bakeLastStepAt).count());++bakeStepCalls;if(bakeWaitReason.load())++bakeWaitSteps;}
            bakeLastStepAt=nowT;
        }else bakeLastStepAt={};
        if(state==BakeState::Baked||state==BakeState::Approximate||state==BakeState::Outdated||state==BakeState::NotBaked){
            std::lock_guard<std::mutex>lock(bakeRequestMx);
            bool anyPublishedInvalid=false;
            for(auto&gr:bakeGroups){
                if((gr.state==BakeState::Baked||gr.state==BakeState::Approximate)&&(!gr.table||!bakeContextTable(*gr.table,true))){
                    gr.state=BakeState::Outdated;gr.percent=0;gr.dirty=true;anyPublishedInvalid=true;
                }
            }
            if(anyPublishedInvalid){
                bakePercent=0;bakeState=BakeState::Outdated;bakeWaitReason=11;lastEstEpoch=~0ull;bakeRestored=0;
                if(!bakeRequested.load()){if(tryRestoreBakedCacheLocked(2))return;stopBakeTasks();return;} // P4: undo may return to a baked content
            }
        }
        if(!bakeRequested.load()){
            // P4: Outdated/NotBaked with a matching cached content (typical after Undo/Redo) restores without a worker.
            const auto idleState=bakeState.load();
            if(idleState==BakeState::Outdated||idleState==BakeState::NotBaked){std::lock_guard<std::mutex>lock(bakeRequestMx);if(tryRestoreBakedCacheLocked(2))return;}
            bakeStreamChangedAt={};
            stopBakeTasks();if(bakeFallbackAction!=bakeAction.load()){lastEstEpoch=~0ull;bakeFallbackAction=bakeAction.load();}return;
        }
        if(!bakeEnabled.load()||cfg.motionMode<2){const auto action=bakeAction.load();stopBakeTasks();endBake(BakeState::Blocked,10,action);return;}
        std::shared_ptr<World> world;
        {std::lock_guard<std::mutex>lock(worldMx);world=latest;}
        const int priority = bakePriority(*world);
        bakeWaitReason=priority;if(priority)return;
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
                if(!bakeRequested.load()||bakeAction.load()!=action||bakePaths.empty()||bakeGroups.empty())return;
                groupsCopy=bakeGroups;
                rev=bakeRevision.load();
#if defined(_WIN32)
                stationDiagnostics = bakeAction.load() == action ? requestedDiagnostics : nullptr;
#endif
            }
            const auto geom=world->serial;const auto qual=workerQuality;
            const auto live=livePose();
            std::vector<double> patterns((size_t)S,0.);for(int s=0;s<S;++s)patterns[(size_t)s]=workerPattern(s);
            std::vector<uint64_t> objRevs((size_t)S+1,0);for(int idx=0;idx<=S;++idx)objRevs[(size_t)idx]=bakeObjectRevision[(size_t)idx].load(std::memory_order_acquire);
            bakeWorld=std::move(world);
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
                jb.table.estimator=kBakeEstimatorVersion; // P3: stamp the estimator; pre-P3 tables can never pass bakeContextTable
                jb.table.contentSig=groupContentSig(gr.spec); // P4: cross-session identity (persistence + undo cache)
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
            {std::lock_guard<std::mutex>lock(bakeRequestMx);bakeTraceStations.clear();bakeTraceDecisions.clear();bakeMaxGapMs=0;bakeStepCalls=0;bakeWaitSteps=0;}
            const auto hw=BakePool::effectiveCpuCount();const unsigned available=hw>1?hw-1:1;unsigned count=bakeWantedWorkers.load();count=count?std::clamp(count,1u,available):defaultPoolWorkers(hw);
            ensureRebuildPool(count);bakeWorkers=(int)count;
            const double histBytes=std::max(1.,std::ceil(bakeWorld->sc.length/.001)+2)*R*NB*16*maxGroupS;
            bakeActiveLimit=std::max<size_t>(1,std::min<size_t>(count,(size_t)(128.*1024*1024/histBytes)));
            bakeStarted=std::chrono::steady_clock::now();
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
                int usedRays=rays;for(int ur:job.usedRays)usedRays=std::max(usedRays,ur);
                // P3: a station still in flight when the path-wide ray decision landed (a speculative lookahead
                // midpoint) may have been traced below the selected budget: discard it and re-trace at the budget,
                // otherwise its late low-budget completion would block allAtSelected forever.
                if(!retry&&jb.raysDecided&&usedRays<jb.selectedRays){retry=true;rays=jb.selectedRays;}
                {Engine::BakeTraceStation ts;ts.group=(int)jb.groupIndex;ts.u=u;ts.kind=std::find(jb.initial.begin(),jb.initial.end(),u)!=jb.initial.end()?0:1;ts.rays=usedRays;ts.queuedMs=job.queuedMs;ts.doneMs=bakeMs();ts.activeAtQueue=job.activeAtQueue;ts.retry=retry;std::lock_guard<std::mutex>lock(bakeRequestMx);bakeTraceStations.push_back(ts);}
                // P3: shared initial probes may still double (before the path-wide decision); afterwards every trace is fixed.
                if(!retry&&!jb.raysDecided&&shared&&cfg.adaptiveTail&&!job.hist.empty()){
                    const size_t nSrc=job.hist.size();
                    auto avg=job.hist[0];
                    for(int r=0;r<R;++r)for(int b=0;b<NB;++b)for(size_t k=0;k<avg.nBins;++k){
                        avg.h[r][b][k]=0;avg.h2[r][b][k]=0;
                        for(auto&h:job.hist){avg.h[r][b][k]+=h.h[r][b][k]/(double)nSrc;avg.h2[r][b][k]+=h.h2[r][b][k]/((double)nSrc*(double)nSrc);}
                    }
                    if(bakeNeedsMoreRays(avg)&&rays<rayLimit(jb.baseRays)){retry=true;rays=std::min(rayLimit(jb.baseRays),rays*2);}
                }
                if(!retry){
                    // P3: station level = RAW integrated energy (no extrapolation); keep the second moments for the noise estimate.
                    // P6: also accumulate per-window energy & ESS ([s][w][r][b]) aligned with TailConv partition boundaries.
                    const int ns=jb.table.sources;
                    std::vector<double> energy((size_t)ns*R*NB,0.),var((size_t)ns*R*NB,0.);
                    std::vector<double> winE((size_t)ns*kBakeWinCount*R*NB,0.),winEss((size_t)ns*kBakeWinCount*R*NB,0.);
                    for(int s=0;s<ns;++s){
                        const auto&h=job.hist[(size_t)s];
                        for(int r=0;r<R;++r)for(int b=0;b<NB;++b){
                            double sum1[kBakeWinCount]={},sum2[kBakeWinCount]={},tot=0.,tot2=0.;
                            const size_t nK=h.h[r][b].size();
                            const bool has2=!h.h2.empty()&&h.h2[r][b].size()>=nK;
                            for(size_t k=0;k<nK;++k){
                                const double v=h.h[r][b][k];
                                const double v2=has2?h.h2[r][b][k]:0.;
                                tot+=v;tot2+=v2;
                                const double tSec=((double)k+0.5)*h.binDur;
                                int w=0;while(w+1<kBakeWinCount&&tSec>=kBakeWinEndSec[w])++w;
                                sum1[w]+=v;sum2[w]+=v2;
                            }
                            const size_t kBase=((size_t)s*R+r)*NB+b;
                            energy[kBase]=tot;var[kBase]=tot2;
                            for(int w=0;w<kBakeWinCount;++w){
                                const size_t wk=(((size_t)s*kBakeWinCount+w)*R+r)*NB+b;
                                winE[wk]=sum1[w];
                                winEss[wk]=(sum2[w]>0.&&sum1[w]>0.)?(sum1[w]*sum1[w]/sum2[w]):0.;
                            }
                        }
                    }
                    bool finite=true;
                    for(double v:energy)finite&=std::isfinite(v)&&v>=0;
                    for(double v:winE)finite&=std::isfinite(v)&&v>=0;
                    for(double v:winEss)finite&=std::isfinite(v)&&v>=0;
                    if(!finite){stopBakeTasks();endBake(BakeState::Failed,12,bakeWorkerAction);return;}
                    jb.cache[u]=std::move(energy);jb.varCache[u]=std::move(var);
                    jb.winCache[u]=std::move(winE);jb.winEssCache[u]=std::move(winEss);
                    jb.raysAt[u]=usedRays;++bakeStations;
                    jb.stationDurations.push_back(bakeMs()-job.queuedMs);
                }
                it=jb.active.erase(it);if(retry){queueGroupStation(jb,u,rays,!jb.raysDecided);break;}
            }
            jb.initialAt=0;for(double u:jb.initial)if(jb.cache.count(u))++jb.initialAt;
            // P3: ONE ray decision per path, from the worst initial station. Every station traced with fewer rays
            // (initial probes or speculative lookahead midpoints) is re-traced at the selected budget, so all
            // published levels come from the same estimator settings and neighbouring stations cannot step.
            if(jb.initialAt==jb.initial.size()&&!jb.raysDecided){
                int worst=jb.baseRays;
                for(double u:jb.initial){auto rit=jb.raysAt.find(u);if(rit!=jb.raysAt.end())worst=std::max(worst,rit->second);}
                jb.selectedRays=worst;jb.raysDecided=true;
                jb.progressDone=0; // P3: re-trace completions must nudge the bar again (the done counter restarts at the new budget)
                std::vector<double> retrace;
                for(const auto&kv:jb.raysAt)if(kv.second<worst)retrace.push_back(kv.first);
                // Keep the stale entries cached until their replacement lands: decisions stay gated on
                // allAtSelected, and progress keeps counting them as work in flight instead of dropping back.
                // The queue loop drains retracePending under bakeActiveLimit (the memory budget).
                jb.retracePending.insert(jb.retracePending.end(),retrace.begin(),retrace.end());
            }
            bool allAtSelected=jb.raysDecided;
            if(allAtSelected)for(const auto&kv:jb.raysAt)if(kv.second<jb.selectedRays){allAtSelected=false;break;}
            if(jb.initialAt==jb.initial.size()&&allAtSelected){
                if(jb.segments.empty())for(size_t j=1;j<jb.nodes.size();++j)if(jb.nodes[j]-jb.nodes[j-1]>1e-6)jb.segments.push_back({jb.nodes[j-1],jb.nodes[j]});
                while(jb.segmentAt<jb.segments.size()){
                    auto ab=jb.segments[jb.segmentAt];double mid=(ab.first+ab.second)*.5;auto found=jb.cache.find(mid);
                    if(found==jb.cache.end())break;
                    ++jb.segmentAt;
                    double error=0;
                    auto decision=bakeSegmentSplit(jb,ab.first,mid,ab.second,error); // P3: band thresholds minus MC noise
                    {Engine::BakeTraceDecision td;td.group=(int)jb.groupIndex;td.a=ab.first;td.b=ab.second;td.error=error;td.decision=decision==BakedRefinement::Accept?0:decision==BakedRefinement::Split?1:2;td.nodes=jb.nodes.size();td.ms=bakeMs();std::lock_guard<std::mutex>lock(bakeRequestMx);bakeTraceDecisions.push_back(td);}
                    if(decision==BakedRefinement::Split){jb.nodes.push_back(mid);jb.segments.push_back({ab.first,mid});jb.segments.push_back({mid,ab.second});}
                    else if(decision==BakedRefinement::Approximate){++jb.table.discontinuities;jb.table.residualSegments.push_back({ab.first,ab.second,error});}
                }
            }
            jb.finished=(jb.initialAt==jb.initial.size()&&allAtSelected&&jb.segmentAt==jb.segments.size());
        }
        bool allFinished=true;for(const auto&jb:bakeJobs)allFinished&=jb.finished;
        if(allFinished){
            if(!bakeRequested.load())return;
            for(const auto&jb:bakeJobs)if(!bakeContextTable(jb.table,false))return;
            for(auto&jb:bakeJobs){
                jb.table.u=jb.nodes;std::sort(jb.table.u.begin(),jb.table.u.end());
                for(double u:jb.table.u){
                    const auto&e=jb.cache.at(u);jb.table.energy.insert(jb.table.energy.end(),e.begin(),e.end());
                    const auto&we=jb.winCache.at(u);jb.table.winEnergy.insert(jb.table.winEnergy.end(),we.begin(),we.end());
                    const auto&ws=jb.winEssCache.at(u);jb.table.winEss.insert(jb.table.winEss.end(),ws.begin(),ws.end());
                }
                if(jb.table.hasDeclaredJoint()&&!jb.table.u.empty()){
                    if(jb.table.u.front()>0.){
                        const double uFront=jb.table.u.front();
                        const auto firstE=jb.cache.at(uFront);
                        const auto firstWE=jb.winCache.at(uFront);
                        const auto firstWS=jb.winEssCache.at(uFront);
                        jb.table.u.insert(jb.table.u.begin(),0.);
                        jb.table.energy.insert(jb.table.energy.begin(),firstE.begin(),firstE.end());
                        jb.table.winEnergy.insert(jb.table.winEnergy.begin(),firstWE.begin(),firstWE.end());
                        jb.table.winEss.insert(jb.table.winEss.begin(),firstWS.begin(),firstWS.end());
                    }
                    if(jb.table.u.back()<1.){
                        const double uBack=jb.table.u.back();
                        const auto lastE=jb.cache.at(uBack);
                        const auto lastWE=jb.winCache.at(uBack);
                        const auto lastWS=jb.winEssCache.at(uBack);
                        jb.table.u.push_back(1.);
                        jb.table.energy.insert(jb.table.energy.end(),lastE.begin(),lastE.end());
                        jb.table.winEnergy.insert(jb.table.winEnergy.end(),lastWE.begin(),lastWE.end());
                        jb.table.winEss.insert(jb.table.winEss.end(),lastWS.begin(),lastWS.end());
                    }
                }
                for(double v:jb.table.energy)jb.table.amplitude.push_back(std::sqrt(v));
                for(double v:jb.table.winEnergy)jb.table.winAmplitude.push_back(std::sqrt(v));
                jb.table.selectedRays=jb.selectedRays;
                // P3: per-node Monte-Carlo noise (dB, 1 sigma of the integrated energy) for diagnostics; not part of the hash.
                // Mirrors the energy construction, including the duplicated 0/1 endpoints of declared joint curves
                // (those u values are not traced stations, so they reuse the first/last node's noise).
                {
                    const size_t stride=jb.table.stride();
                    const double kDb=10./std::log(10.);
                    std::vector<double> sortedNodes=jb.nodes;std::sort(sortedNodes.begin(),sortedNodes.end());
                    auto appendNoise=[&](double u,std::vector<double>& nd,std::vector<double>& ns){
                        const auto& e=jb.cache.at(u);const auto& v=jb.varCache.at(u);
                        for(size_t k=0;k<stride;++k)nd.push_back(e[k]>0?kDb*std::sqrt(std::max(0.,v[k]))/std::sqrt(e[k]):0.);
                        for(int r=0;r<R;++r)for(int b=0;b<NB;++b){
                            double se=0,sv=0;
                            for(int s=0;s<jb.table.sources;++s){const size_t k=((size_t)s*R+r)*NB+b;se+=e[k];sv+=v[k];}
                            ns.push_back(se>0?kDb*std::sqrt(std::max(0.,sv))/std::sqrt(se):0.);
                        }
                    };
                    std::vector<double> nd,ns;
                    nd.reserve(sortedNodes.size()*stride);ns.reserve(sortedNodes.size()*(size_t)R*NB);
                    for(double u:sortedNodes)appendNoise(u,nd,ns);
                    if(jb.table.hasDeclaredJoint()&&!sortedNodes.empty()){
                        if(sortedNodes.front()>0.){
                            std::vector<double> fd,fs;appendNoise(sortedNodes.front(),fd,fs);
                            nd.insert(nd.begin(),fd.begin(),fd.end());ns.insert(ns.begin(),fs.begin(),fs.end());
                        }
                        if(sortedNodes.back()<1.){appendNoise(sortedNodes.back(),nd,ns);}
                    }
                    jb.table.noiseDb=std::move(nd);
                    jb.table.sumNoiseDb=std::move(ns);
                }
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
                bakeMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-bakeStarted).count();
                publishBakedTablesLocked(std::move(published.tables),0,true); // P4: shared publication + undo cache
                if(bakeStop)bakeStop->store(true); // P1: stop speculative stations nobody needs
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
                if(!jb.retracePending.empty()){double u=jb.retracePending.back();jb.retracePending.pop_back();queueGroupStation(jb,u,jb.selectedRays,false);queued=true;}
                if(!queued)for(double u:jb.initial)if(!jb.cache.count(u)&&!jb.active.count(u)){queueGroupStation(jb,u,jb.baseRays,!jb.raysDecided);queued=true;break;}
                if(!queued&&jb.raysDecided&&jb.initialAt==jb.initial.size()){
                    for(size_t j=jb.segmentAt;j<jb.segments.size();++j){auto ab=jb.segments[j];double u=(ab.first+ab.second)*.5;if(!jb.cache.count(u)&&!jb.active.count(u)){queueGroupStation(jb,u,jb.selectedRays,false);queued=true;break;}}
                }
                if(!queued)queued=queueLookaheadStation(jb);
                progressed|=queued;
            }
        }
        size_t doneSum=0,totalSum=0;
        for(auto&jb:bakeJobs){
            // P3: the denominator follows the real remaining work (initial + known validations + expected splits,
            // node-cap bounded); in-flight stations earn asymptotic partial credit so the bar keeps moving while
            // a long station runs. Only stations at the selected ray budget count as done (stale entries awaiting
            // their re-trace count as work in flight). Display only: nothing here feeds the table or its hash.
            size_t d=0,stale=0;
            if(jb.raysDecided){for(const auto&kv:jb.raysAt){if(kv.second>=jb.selectedRays)++d;else ++stale;}}
            else d=jb.cache.size();
            {const double est=bakedStationEstimate(jb.initial.size(),jb.segments.size(),jb.segmentAt,jb.nodes.size())+(double)stale; // P3: re-traces are real remaining work too
             const double expect=std::max(1.,jb.expectedStationMs());const double nowMs=bakeMs();
             double credit=0.;for(const auto&kv:jb.active){const double el=nowMs-kv.second->queuedMs;credit+=0.9*(1.-std::exp(-std::max(0.,el)/expect));}
             const double raw=bakedHonestProgressActive((double)d,credit,est);
             if(d>jb.progressDone||jb.progressShown<=0.){jb.progressShown=bakedAdvanceProgressD(jb.progressShown,raw);jb.progressDone=d;}
             else jb.progressShown=std::min(99.,std::max(jb.progressShown,raw));
            }
            doneSum+=d;totalSum+=jb.initial.size()+jb.segments.size();
        }
        double shownSum=0.,weight=0.;
        for(const auto&jb:bakeJobs){
            size_t stale=0;
            if(jb.raysDecided)for(const auto&kv:jb.raysAt)if(kv.second<jb.selectedRays)++stale;
            const double est=std::max(1.,bakedStationEstimate(jb.initial.size(),jb.segments.size(),jb.segmentAt,jb.nodes.size())+stale);
            shownSum+=jb.finished?99.*est:jb.progressShown*est;
            weight+=est;
        }
        const int agg=weight>0.?std::min(99,(int)(shownSum/weight)):0;
        {
            std::lock_guard<std::mutex>lock(bakeRequestMx);
            if(!bakeRequested.load()||bakeAction.load()!=action)return;
            for(const auto&jb:bakeJobs)if(jb.groupIndex<bakeGroups.size()){
                auto&gr=bakeGroups[jb.groupIndex];
                size_t d=0;
                if(jb.raysDecided){for(const auto&kv:jb.raysAt)if(kv.second>=jb.selectedRays)++d;}
                else d=jb.cache.size();
                gr.done=(int)d;gr.total=(int)(jb.initial.size()+jb.segments.size());
                gr.percent=jb.finished?100:std::min(99,(int)jb.progressShown);
            }
            bakeDone=(int)doneSum;bakeTotal=(int)totalSum;
            bakePercent.store(std::max(bakePercent.load(),agg));
        }
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
                    const auto&p=t.movingPaths[j];double actual[4];
                    const int moff=p.object<0?S*4:p.object*4;
                    for(int k=0;k<4;++k)actual[k]=params[moff+k].load(std::memory_order_relaxed);
                    if(!p.matchesAt(actual,u)){match=false;break;}
                }
            }
            if(!match)continue;
            if(!t.lookup(u,bakeLookupScratch.data()))continue;
            const bool hasWins=t.lookupWindows(u,bakeLookupWinEnergyScratch.data(),bakeLookupWinEssScratch.data());
            const size_t srcStride=(size_t)R*NB;
            const size_t srcWinStride=(size_t)kBakeWinCount*R*NB;
            for(size_t si=0;si<t.groupSources.size();++si){
                const int s=t.groupSources[si];
                if(s>=0&&s<S){
                    std::copy_n(bakeLookupScratch.data()+si*srcStride,srcStride,bakeAudioEnergy.data()+(size_t)s*srcStride);
                    if(hasWins){
                        std::copy_n(bakeLookupWinEnergyScratch.data()+si*srcWinStride,srcWinStride,bakeAudioWinEnergy.data()+(size_t)s*srcWinStride);
                        std::copy_n(bakeLookupWinEssScratch.data()+si*srcWinStride,srcWinStride,bakeAudioWinEss.data()+(size_t)s*srcWinStride);
                    }
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

    // P2/P6: 4-window equalizer filter bank summed per sample on one just-produced block (called from inverse()).
    void eqApplyBank(float* out) {
        const Slot& sl=*eqSlot;
        auto& lpAll=sl.bakeEqLp[(size_t)eqR];
        auto& seen=sl.bakeEqSeen[(size_t)eqR];
        const bool resetLp=(seen+(uint64_t)B!=gcount); // slot was not evaluated last block: prime the filter memories
        seen=gcount;
        const float invB=1.f/(float)B;
        const int bins=B+1;
        std::fill_n(out,(size_t)B,0.f);
        for(int w=0;w<kBakeWinCount;++w){
            inverseRaw(&eqWinRe[(size_t)w*(size_t)bins],&eqWinIm[(size_t)w*(size_t)bins],eqWinTmp.data());
            auto& lp=lpAll[(size_t)w];
            if(resetLp)for(int b=0;b<NB-1;++b)lp[(size_t)b]=eqWinTmp[0];
            const auto& g0=eqOldWin[(size_t)w];
            const auto& g1=eqNewWin[(size_t)w];
            for(int n=0;n<B;++n){
                const float x=eqWinTmp[(size_t)n],wt=(float)(n+1)*invB;
                float prev=0.f,y=0.f;
                for(int b=0;b<NB-1;++b){
                    float& l=lp[(size_t)b];
                    l+=eqA[b]*(x-l);
                    y+=(g0[(size_t)b]+(g1[(size_t)b]-g0[(size_t)b])*wt)*(l-prev);
                    prev=l;
                }
                y+=(g0[(size_t)(NB-1)]+(g1[(size_t)(NB-1)]-g0[(size_t)(NB-1)])*wt)*(x-prev);
                out[n]+=y;
            }
        }
    }
    // E9/E16/P6 per-source: compute 4-window equalized targets for the old bank; the fresh exact bank remains untouched.
    void bakeEqualize(const TailConv& t,const Slot& sl,int r,float* re,float* im) const {
        if(shared||t.bakeSource<0||(!bakeEnabled.load(std::memory_order_relaxed)&&!bakeWasUsed))return;
        if(sl.bakeWinEnergy.size()<(size_t)kBakeWinCount*R*NB||sl.bakeEqGain.size()<=(size_t)r)return;
        const bool srcValid=t.bakeSource<S && bakeAudioSourceValid[(size_t)t.bakeSource]!=0;
        double source[4],listener[4];for(int k=0;k<4;++k){source[k]=params[t.bakeSource*4+k].load(std::memory_order_relaxed);listener[k]=params[S*4+k].load(std::memory_order_relaxed);}
        const bool atRef=!poseFar(source,sl.bakePose.data(),cfg.motionDeadbandM,cfg.motionDeadbandDeg)&&!poseFar(listener,sl.bakePose.data()+4,cfg.motionDeadbandM,cfg.motionDeadbandDeg);
        auto& gains=sl.bakeEqGain[(size_t)r];bool active=false,wasActive=false;
        const bool jumped=sl.bakeJumpSeen!=audioJumpSeen;if(r+1==t.R)sl.bakeJumpSeen=audioJumpSeen;
        // P5: a short loss of the table projection (a few blocks) keeps the last gain instead of dropping the tail to the reference level;
        // a listener that really leaves the path releases to the reference level after 0.3 s, as before P5.
        if(srcValid||atRef)sl.bakeInvalidSince=~0ull;else if(sl.bakeInvalidSince==~0ull)sl.bakeInvalidSince=gcount;
        const bool holdInvalid=!srcValid&&!atRef&&bakeEnabled.load(std::memory_order_relaxed)&&gcount-sl.bakeInvalidSince<=(uint64_t)(.3*sr);

        float targetWins[kBakeWinCount][NB];
        if(srcValid&&!atRef){
            const double* uWinBase=bakeAudioWinEnergy.data()+((size_t)t.bakeSource*kBakeWinCount)*R*NB;
            const double* uEssBase=bakeAudioWinEss.data()+((size_t)t.bakeSource*kBakeWinCount)*R*NB;
            double uWinSum[kBakeWinCount]={},rWinSum[kBakeWinCount]={},uAllSum=0.,rAllSum=0.;
            for(int w=0;w<kBakeWinCount;++w){
                for(int b=0;b<NB;++b){
                    const size_t wk=((size_t)w*R+r)*NB+b;
                    uWinSum[w]+=uWinBase[wk];
                    rWinSum[w]+=sl.bakeWinEnergy[wk];
                }
                uAllSum+=uWinSum[w];rAllSum+=rWinSum[w];
            }
            const double gAllBroad=(uAllSum>0.&&rAllSum>0.)?std::sqrt(uAllSum/rAllSum):1.;
            double prevWinBroad=gAllBroad;
            for(int w=0;w<kBakeWinCount;++w){
                double gWinBroad=(uWinSum[w]>0.&&rWinSum[w]>0.)?std::sqrt(uWinSum[w]/rWinSum[w]):(w>0?prevWinBroad:gAllBroad);
                if(w>0&&(uWinSum[w]<1e-7*uAllSum||rWinSum[w]<1e-7*rAllSum))gWinBroad=prevWinBroad;
                prevWinBroad=gWinBroad;
                gWinBroad=std::clamp(gWinBroad,1e-6,1e6);
                const double logBroad=std::log(gWinBroad);
                for(int b=0;b<NB;++b){
                    const size_t wk=((size_t)w*R+r)*NB+b;
                    const double uE=uWinBase[wk],rE=sl.bakeWinEnergy[wk];
                    const double uEss=uEssBase[wk],rEss=(wk<sl.bakeWinEss.size())?sl.bakeWinEss[wk]:0.;
                    const double gRaw=(uE>0.&&rE>0.)?std::clamp(std::sqrt(uE/rE),1e-6,1e6):gWinBroad;
                    const double fU=uWinSum[w]>0.?uE/uWinSum[w]:0.,fR=rWinSum[w]>0.?rE/rWinSum[w]:0.;
                    const double essMin=std::min(uEss,rEss);
                    const double tEss=std::clamp((essMin-4.)/12.,0.,1.);
                    const double tFrac=std::clamp((std::min(fU,fR)-1e-3)/1e-2,0.,1.);
                    const double alpha=std::min(tEss,tFrac);
                    targetWins[w][b]=(float)std::clamp(std::exp(logBroad+alpha*(std::log(gRaw)-logBroad)),1e-6,1e6);
                }
            }
        }else{
            for(int w=0;w<kBakeWinCount;++w)for(int b=0;b<NB;++b)targetWins[w][b]=holdInvalid?gains[(size_t)w][(size_t)b]:1.f;
        }
        for(int w=0;w<kBakeWinCount;++w)for(int b=0;b<NB;++b){
            const float old=gains[(size_t)w][(size_t)b],target=targetWins[w][b];
            float next=jumped&&srcValid?target:std::clamp(old+(target-old)*.25f,old*.944060876f,old*1.05925373f);
            if(std::abs(next-target)<1e-6f*target)next=target;
            eqOldWin[(size_t)w][(size_t)b]=old;eqNewWin[(size_t)w][(size_t)b]=next;
            wasActive|=old!=1.f;gains[(size_t)w][(size_t)b]=next;active|=next!=1.f;
        }
        if(!active&&!wasActive)return;
        // P2/P6: the equalizer is a 4-window time-domain 7-band one-pole filter bank with per-sample gain
        // interpolation and window-broadband regularization. inverse() accumulates/inverts each partition window
        // and sums them cleanly, so no zero-phase spectral filter wraps around and room decay differences vanish.
        eqSlot=&sl;eqR=r;eqFilter=true;eqPending=true;(void)re;(void)im;(void)t;
    }
