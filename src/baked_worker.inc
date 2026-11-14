    std::mutex bakeRequestMx;
    std::vector<BakedPath> bakePaths;
    std::atomic<uint64_t> bakeRevision{0},bakeAction{0},bakePreempt{0};
    std::atomic<bool> bakeEnabled{false},bakeRequested{false};
    std::atomic<BakeState> bakeState{BakeState::NotBaked};
    std::atomic<unsigned> bakeWantedWorkers{0};
    std::atomic<int> bakePercent{0},bakeDiscontinuities{0},bakeWaitReason{0},bakeWorkers{0},bakeDone{0},bakeTotal{0};
    std::atomic<uint64_t> bakeHash{0},bakeLookups{0},bakeStations{0},bakeResets{0},bakeCancelled{0};
    std::atomic<double> bakeMilliseconds{0};
    std::array<BakedTable,3> bakeSlots;
    std::atomic<int> bakeMiddle{1};int bakeBack=2,bakeFront=0;
    std::shared_ptr<const BakedTable> bakeDiagnostic,bakeInput,bakeRunningDiagnostic;
    BakedTable bakeWork;
    std::shared_ptr<World> bakeWorld;
    std::shared_ptr<std::atomic<bool>> bakeStop;
    std::unique_ptr<BakePool> bakePool;
    unsigned bakePoolCount=0;
    struct BakeBatch {
        double u=0;int rays=0;
        uint64_t preempt=0;
        std::vector<std::future<TailHist>> futures;
        std::vector<TailHist> hist;
    };
    std::map<double,std::unique_ptr<BakeBatch>> bakeActive;
    std::map<double,std::vector<double>> bakeCache;
    std::vector<double> bakeNodes,bakeInitial;
    std::vector<std::pair<double,double>> bakeSegments;
    size_t bakeInitialAt=0,bakeSegmentAt=0,bakeActiveLimit=1,bakeProjectHint=0;
    uint64_t bakeWorkerAction=0,bakeFallbackAction=0;
    uint64_t bakeQueuedAction=0,bakeQueuedStream=0;
    std::vector<double> bakeQueuedPose;
    std::chrono::steady_clock::time_point bakeQueuedWall;
    std::chrono::steady_clock::time_point bakeStarted;
    std::vector<double> bakeAudioEnergy,bakeAudioNorm;
    std::vector<float> bakeReferenceGain;
    bool bakeSharedApplied=false;
    std::array<std::vector<double>,2> bakeNormBuffers;
    std::vector<double> bakeTransNorm;
    std::vector<double> bakeAudioRefPose,bakeTransRefPose;
    std::array<std::vector<double>,2> bakeRefPoseBuffers;
    bool bakeAudioValid=false,bakeWasUsed=false;

    bool bakeContextAudio(const BakedTable& t) const noexcept {
        if(t.job!=bakeAction.load(std::memory_order_acquire)||t.request!=bakeRevision.load(std::memory_order_acquire)||t.geometry!=geomSerial.load(std::memory_order_relaxed)||t.quality!=qualitySerial.load(std::memory_order_relaxed))return false;
        for(int j=0;j<=S;++j){if(t.moves(j))continue;double p[4];for(int k=0;k<4;++k)p[k]=params[j==0?S*4+k:(j-1)*4+k].load(std::memory_order_relaxed);if(poseFar(p,t.staticPose.data()+j*4,cfg.motionDeadbandM,cfg.motionDeadbandDeg))return false;}
        for(int s=0;s<S;++s)if(t.patterns[(size_t)s]!=params[S*4+8+s].load(std::memory_order_relaxed))return false;
        return true;
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
    void stopBakeTasks(){if(bakeStop)bakeStop->store(true);bakeActive.clear();}
    void endBake(BakeState state,int reason,uint64_t expected){std::lock_guard<std::mutex>lock(bakeRequestMx);if(bakeAction.load()!=expected)return;bakeRequested=false;bakePercent=0;bakeState=state;bakeWaitReason=reason;lastEstEpoch=~0ull;}
    void invalidateBake(){stopBakeTasks();endBake(BakeState::Outdated,11,bakeWorkerAction);}
    void queueBakeStation(double u,int rays){
        auto job=std::make_unique<BakeBatch>();job->u=u;job->rays=rays;job->hist.resize((size_t)S);
        auto pose=bakeWork.staticPose;for(const auto&path:bakeWork.movingPaths){auto v=path.at(u);int offset=path.object<0?0:4+4*path.object;for(int k=0;k<4;++k)pose[(size_t)offset+k]=v[k];}
        SurfaceSideHist lh;double listener[4];resolvePose4(bakeWorld->geo,bakeWorld->serial,lh,pose.data(),listener);auto receivers=std::make_shared<std::vector<Transducer>>();resolvedReceivers(bakeWorld->geo,listener,*receivers,!shared);
        auto world=bakeWorld;auto input=bakeInput;auto stop=bakeStop;const auto preempt=bakePreempt.load();job->preempt=preempt;const bool pooled=shared;const int order=cfg.erOrder,limit=cfg.adaptiveTail?(int)std::min<int64_t>(2000000,4ll*rays):rays;
        for(int s=0;s<S;++s){Transducer source=srcs[(size_t)s]->t;SurfaceSideHist sh;double eff[4];resolvePose4(world->geo,world->serial,sh,pose.data()+4+4*s,eff);source.pos={eff[0],eff[1],eff[2]};source.azimuthDeg=eff[3];source.pattern=bakeWork.patterns[(size_t)s];
            job->futures.push_back(bakePool->submit([this,world,input,stop,receivers,source,preempt,pooled,order,rays,limit,s]{
                // Full static-pose coverage is checked by the coordinator/commit/RT.
                // Per-ray cancellation needs only epochs, not O(S) atomic pose reads.
                auto cancelled=[&]{return !running||stop->load(std::memory_order_relaxed)||bakePreempt.load(std::memory_order_relaxed)!=preempt||input->job!=bakeAction.load(std::memory_order_relaxed)||input->request!=bakeRevision.load(std::memory_order_relaxed)||input->geometry!=geomSerial.load(std::memory_order_relaxed)||input->quality!=qualitySerial.load(std::memory_order_relaxed);};
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
        bakeActive.emplace(u,std::move(job));
    }
    void bakeWorkerStepImpl() {
        const auto state=bakeState.load();
        if((state==BakeState::Baked||state==BakeState::Approximate)&&!bakeContextAudio(bakeWork)){invalidateBake();return;}
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
            bakeWork={};{std::lock_guard<std::mutex>lock(bakeRequestMx);if(bakePaths.empty())return;bakeWork.path=bakePaths.front();bakeWork.movingPaths=bakePaths;bakeWork.request=bakeRevision.load();}
            bakeWork.job=action;bakeWork.geometry=geomSerial.load();bakeWork.quality=qualitySerial.load();bakeWork.staticPose=livePose();bakeWork.sources=S;bakeWork.receivers=R;
            for(int s=0;s<S;++s)bakeWork.patterns.push_back(params[S*4+8+s].load());
            {std::lock_guard<std::mutex>lock(worldMx);bakeWorld=latest;}
            bakeInput=std::make_shared<BakedTable>(bakeWork);std::atomic_store(&bakeRunningDiagnostic,bakeInput);bakeWorkerAction=action;bakeCache.clear();bakeNodes.clear();bakeInitial.clear();bakeSegments.clear();bakeSegmentAt=0;bakeInitialAt=0;bakeHash=0;bakeDiscontinuities=0;bakeStations=0;
            double xmin=1e30,xmax=-1e30,ymin=1e30,ymax=-1e30;for(const auto&w:bakeWorld->sc.walls){xmin=std::min({xmin,w.ax,w.bx});xmax=std::max({xmax,w.ax,w.bx});ymin=std::min({ymin,w.ay,w.by});ymax=std::max({ymax,w.ay,w.by});}
            double roomMin=xmax>xmin&&ymax>ymin?std::min({xmax-xmin,ymax-ymin,bakeWorld->sc.height}):5.;int intervals=std::clamp((int)std::ceil(bakeWork.path.distance.back()/std::clamp(.1*roomMin,.1,.5)),1,31);
            bakeInitial={0.,.5,1.};for(int j=1;j<intervals;++j)if((double)j/intervals!=.5)bakeInitial.push_back((double)j/intervals);std::sort(bakeInitial.begin(),bakeInitial.end());bakeNodes=bakeInitial;
            const auto hw=std::thread::hardware_concurrency();const unsigned available=hw>1?hw-1:1;unsigned count=bakeWantedWorkers.load();count=count?std::clamp(count,1u,available):defaultPoolWorkers(hw);
            ensureRebuildPool(count);bakeWorkers=(int)count;
            if(!cfg.adaptiveTail){
                bakeActiveLimit=std::max<size_t>(1,std::min<size_t>(count,(size_t)std::max<unsigned>(std::min(count,2u),(count+(unsigned)S-1)/(unsigned)std::max(1,S))));
            }else{
                const double histBytes=std::max(1.,std::ceil(bakeWorld->sc.length/.001)+2)*R*NB*16*S;
                bakeActiveLimit=std::max<size_t>(1,std::min<size_t>(count,(size_t)(128.*1024*1024/histBytes)));
            }
            bakeStarted=std::chrono::steady_clock::now();bakeState=BakeState::Baking;
        }
        if(!bakeContextAudio(bakeWork)){invalidateBake();return;}
        const int base=shared?std::max(1500,cfg.tailRays/std::max(1,S)):cfg.tailRays;
        for(auto it=bakeActive.begin();it!=bakeActive.end();){auto&job=*it->second;bool ready=true;for(auto&f:job.futures)if(f.valid()&&f.wait_for(std::chrono::seconds(0))!=std::future_status::ready){ready=false;break;}if(!ready){++it;continue;}
            bool retry=false;try{for(size_t s=0;s<job.futures.size();++s)job.hist[s]=job.futures[s].get();}catch(const std::exception&){const bool cancelled=!running||bakeStop->load()||job.preempt!=bakePreempt.load()||!bakeContextAudio(bakeWork);if(!cancelled){stopBakeTasks();endBake(BakeState::Failed,15,bakeWorkerAction);return;}++bakeCancelled;retry=true;}
            double u=job.u;int rays=job.rays;
            if(!retry&&shared&&cfg.adaptiveTail){auto avg=job.hist[0];for(int r=0;r<R;++r)for(int b=0;b<NB;++b)for(size_t k=0;k<avg.nBins;++k){avg.h[r][b][k]=0;avg.h2[r][b][k]=0;for(auto&h:job.hist){avg.h[r][b][k]+=h.h[r][b][k]/S;avg.h2[r][b][k]+=h.h2[r][b][k]/((double)S*S);}}auto raw=avg;finalizeTail(avg);if(tailNeedsMoreRays(raw,avg)&&rays<rayLimit(base)){retry=true;rays=std::min(rayLimit(base),rays*2);}}
            if(!retry){std::vector<double> energy((size_t)S*R*NB,0.);for(int s=0;s<S;++s)for(int r=0;r<R;++r)for(int b=0;b<NB;++b)for(double v:job.hist[(size_t)s].h[r][b])energy[((size_t)s*R+r)*NB+b]+=v;bool finite=true;for(double v:energy)finite&=std::isfinite(v)&&v>=0;if(!finite){stopBakeTasks();endBake(BakeState::Failed,12,bakeWorkerAction);return;}bakeCache[u]=std::move(energy);++bakeStations;}
            it=bakeActive.erase(it);if(retry){queueBakeStation(u,rays);break;}
        }
        bakeInitialAt=0;for(double u:bakeInitial)if(bakeCache.count(u))++bakeInitialAt;
        if(bakeInitialAt==bakeInitial.size()){
            if(bakeSegments.empty())for(size_t j=1;j<bakeNodes.size();++j)bakeSegments.push_back({bakeNodes[j-1],bakeNodes[j]});
            while(bakeSegmentAt<bakeSegments.size()){auto ab=bakeSegments[bakeSegmentAt];double mid=(ab.first+ab.second)*.5;auto found=bakeCache.find(mid);if(found==bakeCache.end())break;++bakeSegmentAt;double error=bakeError(bakeCache.at(ab.first),found->second,bakeCache.at(ab.second),S,R);
                auto decision=bakedRefinement(error,bakeNodes.size(),ab.first,ab.second);if(decision==BakedRefinement::Split){bakeNodes.push_back(mid);bakeSegments.push_back({ab.first,mid});bakeSegments.push_back({mid,ab.second});}else if(decision==BakedRefinement::Approximate){++bakeWork.discontinuities;bakeWork.residualSegments.push_back({ab.first,ab.second,error});}
            }
            if(bakeSegmentAt==bakeSegments.size()&&bakeActive.empty()){
                if(!bakeRequested.load()||!bakeContextAudio(bakeWork))return;
                bakeWork.u=bakeNodes;std::sort(bakeWork.u.begin(),bakeWork.u.end());for(double u:bakeWork.u){const auto&e=bakeCache.at(u);bakeWork.energy.insert(bakeWork.energy.end(),e.begin(),e.end());}for(double v:bakeWork.energy)bakeWork.amplitude.push_back(std::sqrt(v));bakeWork.selectedRays=base;
                auto diagnostic=std::shared_ptr<const BakedTable>(std::make_shared<BakedTable>(bakeWork));BakedTable published=bakeWork;
                std::lock_guard<std::mutex>lock(bakeRequestMx);if(!bakeRequested.load()||!bakeContextAudio(bakeWork))return;
                std::atomic_store(&bakeDiagnostic,diagnostic);bakeHash=bakeWork.hash();bakeDiscontinuities=bakeWork.discontinuities;
                bakeSlots[(size_t)bakeBack]=std::move(published);bakeBack=bakeMiddle.exchange(bakeBack|4,std::memory_order_acq_rel)&3;bakeMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-bakeStarted).count();bakePercent=100;bakeRequested=false;bakeState=bakeWork.discontinuities?BakeState::Approximate:BakeState::Baked;return;
            }
        }
        for(double u:bakeInitial)if(bakeActive.size()<bakeActiveLimit&&!bakeCache.count(u)&&!bakeActive.count(u))queueBakeStation(u,base);
        if(bakeInitialAt==bakeInitial.size())for(size_t j=bakeSegmentAt;j<bakeSegments.size()&&bakeActive.size()<bakeActiveLimit;++j){auto ab=bakeSegments[j];double u=(ab.first+ab.second)*.5;if(!bakeCache.count(u)&&!bakeActive.count(u))queueBakeStation(u,base);}
        bakeDone=(int)bakeCache.size();bakeTotal=(int)(bakeInitial.size()+bakeSegments.size());bakePercent.store(std::max(bakePercent.load(),bakedProgressPercent(bakeInitialAt,bakeInitial.size(),bakeSegmentAt,bakeSegments.size())));
    }
    void bakeWorkerStep(){const auto action=bakeAction.load();try{bakeWorkerStepImpl();}catch(const std::exception&){stopBakeTasks();endBake(BakeState::Failed,15,action);}}

    void bakeAudioStep(bool jumped) {
        bakeAudioValid=false;
        if(!bakeEnabled.load(std::memory_order_acquire)){if(shared&&bakeSharedApplied&&!motionExact&&transState.load()==0){std::copy(bakeReferenceGain.begin(),bakeReferenceGain.end(),lateGlocal.begin());lateGtarget=lateGlocal.data();gainMode=1;}bakeSharedApplied=false;return;}
        if(bakeMiddle.load(std::memory_order_acquire)&4){bakeFront=bakeMiddle.exchange(bakeFront,std::memory_order_acq_rel)&3;bakeProjectHint=0;}
        const auto&t=bakeSlots[(size_t)bakeFront];if(!bakeContextAudio(t))return;
        double pose[4];int offset=t.path.object<0?S*4:t.path.object*4;for(int k=0;k<4;++k)pose[k]=params[offset+k].load(std::memory_order_relaxed);
        double u;if(!t.path.project(pose,u,&bakeProjectHint))return;
        for(size_t j=1;j<t.movingPaths.size();++j){const auto&p=t.movingPaths[j];auto expected=p.at(u);double actual[4];for(int k=0;k<4;++k)actual[k]=params[p.object*4+k].load(std::memory_order_relaxed);if(poseFar(actual,expected.data(),.02,1.))return;}
        if(!t.lookup(u,bakeAudioEnergy.data()))return;
        bakeAudioValid=true;bakeWasUsed=true;++bakeLookups;
        if(!shared||motionExact||transState.load(std::memory_order_relaxed)!=0)return;
        int moving=t.path.object<0?0:4+4*t.path.object;if(bakeAudioRefPose.size()>=(size_t)moving+4&&!poseFar(pose,bakeAudioRefPose.data()+moving,cfg.motionDeadbandM,cfg.motionDeadbandDeg)){std::copy(bakeReferenceGain.begin(),bakeReferenceGain.end(),lateGlocal.begin());if(jumped)std::copy(bakeReferenceGain.begin(),bakeReferenceGain.end(),lateGuse.begin());lateGtarget=lateGlocal.data();gainMode=1;bakeSharedApplied=true;return;}
        for(int r=0;r<R;++r)for(int s=0;s<S;++s)for(int b=0;b<NB;++b){size_t k=((size_t)r*S+s)*8+b;double n=bakeAudioNorm[(size_t)r*NB+b];if(n>0){lateGlocal[k]=(float)std::sqrt(bakeAudioEnergy[((size_t)s*R+r)*NB+b]/n);if(jumped)lateGuse[k]=lateGlocal[k];}}
        lateGtarget=lateGlocal.data();gainMode=1;bakeSharedApplied=true;
    }

    // E9 per-source: modify only the old bank's spectrum. The fresh exact bank remains untouched.
    void bakeEqualize(const TailConv& t,const Slot& sl,int r,float* re,float* im) const {
        if(shared||t.bakeSource<0||(!bakeEnabled.load(std::memory_order_relaxed)&&!bakeWasUsed))return;
        double source[4],listener[4];for(int k=0;k<4;++k){source[k]=params[t.bakeSource*4+k].load(std::memory_order_relaxed);listener[k]=params[S*4+k].load(std::memory_order_relaxed);}
        const bool atRef=!poseFar(source,sl.bakePose.data(),cfg.motionDeadbandM,cfg.motionDeadbandDeg)&&!poseFar(listener,sl.bakePose.data()+4,cfg.motionDeadbandM,cfg.motionDeadbandDeg);
        auto& gains=sl.bakeEqGain[(size_t)r];bool active=false;
        const bool jumped=sl.bakeJumpSeen!=audioJumpSeen;if(r+1==t.R)sl.bakeJumpSeen=audioJumpSeen;
        for(int b=0;b<NB;++b){float target=1;size_t k=(size_t)r*NB+b;if(bakeAudioValid&&!atRef&&sl.bakeEnergy[k]>0)target=(float)std::clamp(std::sqrt(bakeAudioEnergy[((size_t)t.bakeSource*R+r)*NB+b]/sl.bakeEnergy[k]),1e-6,1e6);
            float old=gains[(size_t)b];float next=jumped&&bakeAudioValid?target:std::clamp(old+(target-old)*.25f,old*.944060876f,old*1.05925373f);
            if(std::abs(next-target)<1e-6f*target)next=target;gains[(size_t)b]=next;active|=next!=1.f;
        }
        if(!active)return;
        float power[NB];for(int b=0;b<NB;++b)power[b]=gains[(size_t)b]*gains[(size_t)b];int bin=0;
       #if VERB_REQUIRE_AVX2
        for(;bin+8<=t.bins;bin+=8){auto sum=_mm256_setzero_ps();for(int b=0;b<NB;++b)sum=_mm256_add_ps(sum,_mm256_mul_ps(_mm256_loadu_ps(t.bakeWeights[b].data()+bin),_mm256_set1_ps(power[b])));auto g=_mm256_sqrt_ps(_mm256_max_ps(sum,_mm256_setzero_ps()));_mm256_storeu_ps(re+bin,_mm256_mul_ps(_mm256_loadu_ps(re+bin),g));_mm256_storeu_ps(im+bin,_mm256_mul_ps(_mm256_loadu_ps(im+bin),g));}
       #endif
        for(;bin<t.bins;++bin){float square=0;for(int b=0;b<NB;++b)square+=t.bakeWeights[b][(size_t)bin]*power[b];float gain=std::sqrt(std::max(0.f,square));re[bin]*=gain;im[bin]*=gain;}
    }
