
#include <new>
#include "bake_pool.hpp"
static thread_local bool e10Audio=false;
static thread_local size_t e10Allocations=0;
void* operator new(size_t n){if(e10Audio)++e10Allocations;if(void*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{std::free(p);}void operator delete[](void*p)noexcept{std::free(p);}
void operator delete(void*p,size_t)noexcept{std::free(p);}void operator delete[](void*p,size_t)noexcept{std::free(p);}
struct E10Audio {
    Engine& engine;std::vector<float> input,output;std::vector<const float*>ip;std::vector<float*>op;double peak=0;
    explicit E10Audio(Engine&e):engine(e),input(128,.01f),output((size_t)e.numReceivers()*128){ip.assign(e.numSources(),input.data());for(int r=0;r<e.numReceivers();++r)op.push_back(output.data()+128*r);}
    void block(){e10Audio=true;engine.process(ip.data(),op.data(),128);e10Audio=false;for(float v:output){if(!std::isfinite(v))peak=INFINITY;else peak=std::max(peak,std::abs(double(v)));}}
    template<class P>bool until(P predicate,double seconds){auto end=std::chrono::steady_clock::now()+std::chrono::duration<double>(seconds);while(std::chrono::steady_clock::now()<end){block();if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}return false;}
};
static void testE10(){
    const unsigned hw=std::thread::hardware_concurrency(),maximum=hw>1?hw-1:1;
    std::printf("E10 deterministic independent work:1/2/7/%u workers\n",maximum);
    if(!std::getenv("VERB_E10_RUNTIME_ONLY")){
    int rooms=0;
    for(const auto& room:verbp::builtinRooms()){
        Scene s=builtinSceneNamed(room.name.c_str());
        for(bool shared:{false,true}){
            uint64_t expected=0;
            for(unsigned workers:{1u,2u,7u,maximum}){
                EngineConfig c;c.erOrder=1;c.tailRays=256;c.tailThread=false;c.sharedTail=shared;c.rebuildWorkers=workers;
                Engine e(s,c);const auto h=e.lateStateHash();if(workers==1)expected=h;
                printf("E10_COLD|%s|%d|%u|%016llx\n",room.name.c_str(),shared,workers,(unsigned long long)h);
                const std::string label="E10 cold hash "+room.name+(shared?" shared":" per-source")+" workers"+std::to_string(workers);
                check(h==expected,label.c_str());
            }
        }++rooms;
    }
    check(rooms==17,"E10 covers every built-in room in both modes");
    BakePool rayPool(maximum);
    for(const auto& room:verbp::builtinRooms()){
        Scene sc=withClutter(withRoughness(builtinSceneNamed(room.name.c_str())),128);sc.length=20.;uint64_t expected=0;
        for(unsigned workers:{1u,2u,7u,maximum}){
            auto h=traceTail(sc,sc.sources[0],sc.receivers,1,48000,sc.seed,false,{},TailExecution{&rayPool,workers,{}});
            uint64_t hash=1469598103934665603ull;auto bytes=[&](double x){const auto*p=(const unsigned char*)&x;for(size_t j=0;j<sizeof(x);++j){hash^=p[j];hash*=1099511628211ull;}};
            for(size_t r=0;r<h.h.size();++r)for(int b=0;b<NB;++b){for(double x:h.h[r][b])bytes(x);for(double x:h.h2[r][b])bytes(x);}
            if(workers==1)expected=hash;
            printf("E10_RAYS|%s|48000|%u|%016llx\n",room.name.c_str(),workers,(unsigned long long)hash);
            check(hash==expected,"E10 full48000 ray moments independent of worker count");
        }
    }

    }
    for(bool shared:{false,true}){
        Scene s=builtinSceneNamed("Vocal booth");
        if(shared){auto t=s.sources.front();t.pos.x+=.05;s.sources.push_back(t);t.pos.x+=.05;s.sources.push_back(t);}
        EngineConfig c;c.erOrder=1;c.tailRays=48000;c.sharedTail=shared;c.rebuildWorkers=maximum;c.tailRefreshMs=0;
        Engine e(s,c);E10Audio audio(e);for(int j=0;j<1000;++j)audio.block();auto p=s.sources[0].pos;
        const auto cancelled=e.motionStats().retraceCancelled;const int refresh=e.tailRefreshCount();
        e.setSourcePosition(0,p.x+.12,p.y,p.z);
        check(audio.until([&]{return !e.tailIsCurrent();},10),"E10 observes active full-quality rebuild");
        auto start=std::chrono::steady_clock::now();e.setSourcePosition(0,p.x+.24,p.y,p.z);
        bool stopped=audio.until([&]{return e.motionStats().retraceCancelled>cancelled;},2);
        double latency=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        printf("E10_CANCEL shared%d latency_ms%.6f refresh%d/%d\n",shared,latency,refresh,e.tailRefreshCount());
        check(stopped&&latency<2000,"E10 active obsolete rebuild cancels promptly");
        check(e.tailRefreshCount()==refresh,"E10 obsolete rebuild is not published");
        check(audio.until([&]{return e.tailRefreshCount()>refresh&&e.tailIsCurrent();},30),"E10 newest pose rebuild completes");
        check(audio.peak>1e-8&&std::isfinite(audio.peak),"E10 wet survives cancelled rebuild");
    }
    {
        Scene s=builtinSceneNamed("Vocal booth");EngineConfig c;c.erOrder=1;c.tailRays=20000;c.tailRefreshMs=0;
        Engine e(s,c);E10Audio audio(e);for(int j=0;j<1000;++j)audio.block();
        auto a=s.hasListener?s.listenerPos:s.receivers.front().pos;
        BakedPath path;path.object=-1;path.poses={{{a.x,a.y,a.z,s.listenerYaw}},{{a.x+.3,a.y,a.z,s.listenerYaw}}};path.distance={0,.3};e.setBakedPaths({path});e.requestPathBake();
        check(audio.until([&]{return e.bakeStats().state==Engine::BakeState::Baking;},10),"E10 bake running on shared pool");
        const int refresh=e.tailRefreshCount();e.setListener(a.x+.15,a.y,a.z,s.listenerYaw);
        check(audio.until([&]{return e.tailRefreshCount()>refresh&&e.tailIsCurrent();},30),"E10 waiting rebuild preempts bake without starvation/deadlock");
        check(audio.until([&]{return e.bakeStats().percent==100;},60),"E10 bake resumes after priority rebuild");
    }

    {
        BakePool pool(maximum);BandBuf buf(2);for(auto&r:buf)for(auto&b:r)b.assign(1u<<19,.0001);
        std::atomic<bool>entered{false},stop{false},caught{false};TailExecution run{&pool,maximum,[&]{entered=true;return stop.load();}};
        std::thread task([&]{try{auto ir=combineBands(buf,48000,run);(void)ir;}catch(const std::runtime_error&){caught=true;}});
        while(!entered)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto start=std::chrono::steady_clock::now();stop=true;task.join();
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        printf("E10_CANCEL_RENDER latency_ms%.6f workers%u\n",ms,maximum);
        check(caught&&ms<2000,"E10 render cancellation joins every FFT worker before returning");
    }

    for(bool shared:{false,true}){
        Scene sc=makeE8Corridor({1.85,1.0,1.5},{1.85,4.0,1.5},-90);
        EngineConfig c;c.erOrder=1;c.tailRays=8000;c.sharedTail=shared;c.tailRefreshMs=0;
        Engine e(sc,c);e.setSoloMask(4);E10Audio audio(e);for(int j=0;j<1000;++j)audio.block();
        Scene target=sc;target.sources[0].pos.y=1.2;auto referenceConfig=c;referenceConfig.tailThread=false;Engine cold(target,referenceConfig);
        auto start=std::chrono::steady_clock::now();int event=-1;bool parked=false;
        while(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<10){
            double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            int tick=(int)(seconds*20);if(tick!=event){event=tick;double y=.25+1.2*std::sin(tick*.15);e.setSourcePosition(0,1.85,y,1.5);e.notifyJump();}
            if(seconds>3&&!parked){parked=true;std::this_thread::sleep_for(std::chrono::milliseconds(300));}
            audio.block();std::this_thread::sleep_for(std::chrono::microseconds(2667));
        }
        const int refresh=e.tailRefreshCount();e.setSourcePosition(0,1.85,1.2,1.5);e.notifyJump();
        check(audio.until([&]{return e.tailRefreshCount()>refresh&&e.tailIsCurrent();},30),"E10 Corridor recovers after10s in/out drag and stopped/started processing");
        for(int j=0;j<(int)std::ceil((e.tailSeconds()+2)*48000/128);++j)audio.block();
        double energy=0;for(int j=0;j<188;++j){audio.block();for(float x:audio.output)energy+=(double)x*x;}
        double rms=std::sqrt(energy/(188*audio.output.size()));
        printf("E10_CORRIDOR shared%d wet_rms%.12g hash%s\n",shared,rms,e.lateStateHash()==cold.lateStateHash()?"IDENTICAL":"DIFFERENT");
        check(std::isfinite(rms)&&rms>1e-12,"E10 Corridor final wet has not disappeared");
        check(e.lateStateHash()==cold.lateStateHash(),"E10 Corridor final settled hash equals cold static build");
    }
    check(e10Allocations==0,"E10 process() allocates nothing during rebuild/bake");
    printf("E10 process allocations%zu\n",e10Allocations);
}
