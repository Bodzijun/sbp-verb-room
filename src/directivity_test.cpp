#include "rt_engine.hpp"
#include "geometry.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <sstream>
#include <thread>
static thread_local bool audio=false;
static std::atomic<int> allocations{0},audioDeletes{0},retired{0};
void* operator new(std::size_t n){if(audio)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}
static int fails=0;
static void check(bool ok,const char* label){printf("[%s] %s\n",ok?"PASS":"FAIL",label);if(!ok)++fails;}
static std::shared_ptr<const verb::DirectivityProfile> profile(int id=0){
 auto p=new verb::DirectivityProfile;p->id="test_"+std::to_string(id);p->name="Test";p->order=1;
 for(auto& c:p->coeffs)c={1,0,0,.3f};std::string err;verb::normalizeProfilePower(*p,&err);
 return {p,[](const verb::DirectivityProfile* q){if(audio)++audioDeletes;else ++retired;delete q;}};
}
static verb::Scene scene(){
 std::istringstream in("sr 48000\nlength .3\nheight 3\nmaterial W .3 .3 .3 .3 .3 .3 .3 .2 .5\nfloor W\nceiling W\nwall 0 0 6 0 W\nwall 6 0 6 5 W\nwall 6 5 0 5 W\nwall 0 5 0 0 W\nsource S 1 2 1 0 .5\nreceiver R 4 2 1\n");return verb::parseScene(in,"directivity regression");
}
int main(){
 for(int order:{0,8}){
  verb::DirectivityProfile p;p.id="negative";p.name="Negative";p.order=order;
  for(auto& c:p.coeffs){c.assign(verb::directivityNumCoeffs(order),0);c[0]=-1;}
  verb::DirectivityProfile parsed;std::string err;
  check(!verb::parseVerbdir(verb::writeVerbdir(p),parsed,err)&&err.find("positive")!=std::string::npos,"negative constant reaches radiation validation and is rejected");
  for(auto& c:p.coeffs){c[0]=1e-5f;if(order)c[3]=1.f;}
  check(verb::normalizeProfilePower(p,&err),"near-cancelling/order8 profile normalizes");
  double worst=0;for(int b=0;b<7;++b){double sum=0;for(int i=0;i<8192;++i){double z=1.-2.*(i+.5)/8192.,r=std::sqrt(1-z*z),phi=2.399963229728653*i;double g=p.evalBandLocal(b,r*std::cos(phi),r*std::sin(phi),z);sum+=g*g;}worst=std::max(worst,std::abs(sum/8192.-1.));}
  printf("order %d independent power error %.9g\n",order,worst);check(worst<.002,"independent dense post-clipping power integral");
 }
 {
  verb::DirectivityProfile p;p.id="cancel";p.name="Nearly cancelled";p.order=8;
  for(auto& c:p.coeffs){c.assign(81,0);c[0]=(float)(-std::sqrt(3.)+1e-7);c[3]=1;}
  std::string err;check(!verb::normalizeProfilePower(p,&err),"order8 almost-cancelled positive cap rejected as unusable radiation");
 }
 {
  verb::DirectivityMailbox mailbox;std::atomic<bool> done{false};std::atomic<int> bad{0};
  std::thread producer([&]{for(int i=1;i<20000;++i)mailbox.publish({profile(i),.25,(double)i,true});done=true;});
  while(!done){audio=true;const auto& s=mailbox.audioRead();if(s.profile && s.profile->id!="test_"+std::to_string((int)s.pattern))++bad;audio=false;}
  producer.join();check(bad==0,"concurrent mailbox profile/amount/pattern are coherent");
 }
 {
  for(bool active:{false,true}) {
   auto sc=scene();auto p=active?profile(999):std::shared_ptr<const verb::DirectivityProfile>{};
   sc.sources[0].directivity=p;sc.sources[0].instrument=active?"custom":"";
   verb::EngineConfig cfg;cfg.tailThread=false;cfg.tailRays=1500;cfg.rebuildWorkers=1;
   verb::Engine offline(sc,cfg);offline.setSourceDirectivity(0,p,1.,sc.sources[0].pattern);
   check(offline.tailIsCurrent(),"equivalent initial None/custom publication keeps offline IR current");
  }
 }
 allocations=0;audioDeletes=0;
 for(bool shared:{false,true}){
  auto sc=scene();verb::EngineConfig cfg;cfg.tailRays=1500;cfg.rebuildWorkers=1;cfg.manualWorker=true;cfg.sharedTail=shared;cfg.motionMode=0;cfg.tailRefreshMs=1;
  verb::Engine e(sc,cfg);float in[128]{},out[128]{};const float* ip[]={in};float* op[]={out};
  auto process=[&]{audio=true;e.process(ip,op,128);audio=false;};
  for(int swap=0;swap<12;++swap){
   auto p=(swap%3==2)?std::shared_ptr<const verb::DirectivityProfile>{}:profile(swap+shared*100);
   e.setSourceDirectivity(0,p,.75);auto freshScene=sc;freshScene.sources[0].directivity=p;freshScene.sources[0].instrument=p?"custom":"";freshScene.sources[0].instrumentAmount=.75;
   verb::Engine fresh(freshScene,cfg);p.reset();
   bool current=false;for(int k=0;k<1800;++k){process();if(k%8==0)e.runWorkerStep();if(k>32 && e.tailIsCurrent()){current=true;break;}}
   const auto a=e.autoLevelEnergy(),b=fresh.autoLevelEnergy();double worst=0;
   for(int band=0;band<7;++band){const double x=a->points[0].late[band],y=b->points[0].late[band];if(!(x>0&&y>0&&std::isfinite(x)&&std::isfinite(y)))worst=1e9;else worst=std::max(worst,std::abs(x/y-1));}
   printf("shared=%d swap=%d current=%d max late relative error %.9g\n",shared,swap,current,worst);check(current&&worst<.02,"live/fresh late energy agrees in all seven bands (None/custom transitions)");
  }
 }
 printf("process allocations=%d audio profile deletions=%d offthread retirements=%d\n",(int)allocations,(int)audioDeletes,(int)retired);
 check(allocations==0&&audioDeletes==0&&retired>0,"no allocations/profile destruction in process; old profiles retired offthread");
 {
  // E22 Unit tests: applyProfileAim, dirGainBands, horizontalSlice, composition with source orientation
  auto synth = std::make_shared<verb::DirectivityProfile>();
  synth->id = "e22_synth_o3";
  synth->name = "E22 Synth Order 3";
  synth->order = 3;
  for (int b = 0; b < verb::NB; ++b) {
   synth->coeffs[b].assign(verb::directivityNumCoeffs(3), 0.f);
   synth->coeffs[b][0] = 1.0f;
   synth->coeffs[b][1] = 0.28f;  // y (left/right asymmetry)
   synth->coeffs[b][2] = 0.22f;  // z (up/down asymmetry)
   synth->coeffs[b][3] = 0.65f;  // x (front/back lobe)
   synth->coeffs[b][6] = 0.18f;  // order 2
   synth->coeffs[b][8] = -0.15f; // order 2
   synth->coeffs[b][12] = 0.14f; // order 3
  }
  std::string err;
  check(verb::normalizeProfilePower(*synth, &err), "E22 synth order-3 asymmetric profile normalizes");

  auto invAimRef = [](double azDeg, double elDeg, double rollDeg, verb::Vec3 v) -> verb::Vec3 {
   constexpr double deg2rad = verb::kPi / 180.0;
   const double a = azDeg * deg2rad, e = elDeg * deg2rad, r = rollDeg * deg2rad;
   const double ca = std::cos(a), sa = std::sin(a);
   verb::Vec3 v1{v.x * ca + v.y * sa, -v.x * sa + v.y * ca, v.z};
   const double ce = std::cos(e), se = std::sin(e);
   verb::Vec3 v2{v1.x * ce + v1.z * se, v1.y, -v1.x * se + v1.z * ce};
   const double cr = std::cos(r), sr = std::sin(r);
   verb::Vec3 v3{v2.x, v2.y * cr + v2.z * sr, -v2.y * sr + v2.z * cr};
   return verb::unit(v3);
  };

  std::vector<verb::Vec3> dirs = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  for (int i = 0; i < 32; ++i) {
   const double z = 1.0 - 2.0 * (i + 0.5) / 32.0;
   const double rxy = std::sqrt(std::max(0.0, 1.0 - z * z));
   const double phi = 2.399963229728653 * i;
   dirs.push_back({rxy * std::cos(phi), rxy * std::sin(phi), z});
  }

  std::vector<std::shared_ptr<const verb::DirectivityProfile>> profs = {synth};
  for (const char* pid : {"th_koeln_voice_sentence", "brass_trumpet_bell_model", "genelec_8030c"}) {
   if (auto bp = verb::findBuiltinDirectivity(pid)) profs.push_back(bp);
  }

  struct AimCase { double az, el, roll; };
  const AimCase cases[] = {{0,0,0},{90,0,0},{-90,0,0},{180,0,0},{0,35,0},{0,-45,0},{0,0,60},{0,0,-90},{37,-22,55}};
  bool unitOk = true;
  double worstDiff = 0.0;
  for (const auto& pr : profs) {
   for (const auto& c : cases) {
    verb::Transducer t;
    t.directivity = pr;
    t.instrument = pr->id;
    t.instrumentAmount = 1.0;
    t.aimAzDeg = c.az;
    t.aimElDeg = c.el;
    t.aimRollDeg = c.roll;
    for (const auto& d : dirs) {
     double gGot[verb::NB]{};
     verb::dirGainBands(t, d, gGot);
     const verb::Vec3 q = (c.az == 0 && c.el == 0 && c.roll == 0) ? d : invAimRef(c.az, c.el, c.roll, d);
     const auto gExp = pr->evalLocal(q.x, q.y, q.z);
     for (int b = 0; b < verb::NB; ++b) {
      const double diff = std::abs(gGot[b] - gExp[(size_t)b]);
      worstDiff = std::max(worstDiff, diff);
      if (diff > 1e-6) unitOk = false;
     }
    }
   }
  }
  printf("E22 unit aim rotation max diff=%.3e\n", worstDiff);
  check(unitOk, "E22 unit aim rotation (zero/az/el/roll/flip/combined across 38 dirs incl poles & 4 profiles) <= 1e-6");

  // Flip front/back == az=180, and horizontalSlice shifts cleanly on az=90
  const auto h0 = synth->horizontalSlice(6, 72, 1.0, 0.0, 0.0, 0.0);
  const auto h90 = synth->horizontalSlice(6, 72, 1.0, 90.0, 0.0, 0.0);
  const auto h180 = synth->horizontalSlice(6, 72, 1.0, 180.0, 0.0, 0.0);
  bool sliceOk = h0.size() == 72 && h90.size() == 72 && h180.size() == 72;
  for (int k = 0; k < 72 && sliceOk; ++k) {
   if (std::abs(h90[(size_t)k] - h0[(size_t)((k + 54) % 72)]) > 1e-5f) sliceOk = false;
   if (std::abs(h180[(size_t)k] - h0[(size_t)((k + 36) % 72)]) > 1e-5f) sliceOk = false;
  }
  check(sliceOk, "E22 horizontalSlice shifts by 90 and 180 deg matching azimuth offset & flip");

  // Composition: aim correction followed by source orientation (yaw/pitch)
  verb::Transducer tComp;
  tComp.directivity = synth;
  tComp.instrument = synth->id;
  tComp.instrumentAmount = 1.0;
  tComp.azimuthDeg = 35.0;
  tComp.elevationDeg = -18.0;
  tComp.aimAzDeg = 42.0;
  tComp.aimElDeg = 20.0;
  tComp.aimRollDeg = -55.0;
  const double yaw = tComp.azimuthDeg * verb::kPi / 180.0;
  const double pitch = tComp.elevationDeg * verb::kPi / 180.0;
  const verb::Vec3 fwd{std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)};
  const verb::Vec3 left{-std::sin(yaw), std::cos(yaw), 0.0};
  const verb::Vec3 up{-std::cos(yaw) * std::sin(pitch), -std::sin(yaw) * std::sin(pitch), std::cos(pitch)};
  bool compOk = true;
  for (const auto& localD : dirs) {
   const verb::Vec3 worldD = fwd * localD.x + left * localD.y + up * localD.z;
   double gGot[verb::NB]{};
   verb::dirGainBands(tComp, worldD, gGot);
   const verb::Vec3 q = invAimRef(tComp.aimAzDeg, tComp.aimElDeg, tComp.aimRollDeg, localD);
   const auto gExp = synth->evalLocal(q.x, q.y, q.z);
   for (int b = 0; b < verb::NB; ++b) {
    if (std::abs(gGot[b] - gExp[(size_t)b]) > 1e-6) compOk = false;
   }
  }
  check(compOk, "E22 profile aim composes cleanly with source azimuth/elevation orientation");

  // Tail invariance for rotated source profile + receiver tail directionality in diffuse box
  {
   auto scBase = scene();
   scBase.sources[0].directivity = synth;
   scBase.sources[0].instrument = synth->id;
   scBase.sources[0].instrumentAmount = 1.0;
   auto scRot = scBase;
   scRot.sources[0].aimAzDeg = 90.0;
   scRot.sources[0].aimElDeg = 35.0;
   scRot.sources[0].aimRollDeg = -60.0;
   auto scOmni = scBase;
   scOmni.sources[0].directivity.reset();
   scOmni.sources[0].instrument.clear();
   scOmni.sources[0].pattern = 0.0;
   const auto tBase = verb::traceTail(scBase, scBase.sources[0], scBase.receivers, 2, 8000, scBase.seed, true);
   const auto tRot = verb::traceTail(scRot, scRot.sources[0], scRot.receivers, 2, 8000, scRot.seed, true);
   const auto tOmni = verb::traceTail(scOmni, scOmni.sources[0], scOmni.receivers, 2, 8000, scOmni.seed, true);
   bool invOk = true;
   for (int b = 0; b < verb::NB; ++b) {
    double sB = 0, sR = 0, sO = 0;
    for (size_t r = 0; r < tBase.h.size(); ++r) {
     for (size_t k = 0; k < tBase.nBins; ++k) {
      sB += tBase.h[r][b][k];
      sR += tRot.h[r][b][k];
      sO += tOmni.h[r][b][k];
     }
    }
    if (!(sO > 0 && std::abs(sR / sB - 1.0) < 1e-6 && std::abs(sR / sO - 1.0) < 0.02)) invOk = false;
   }
   check(invOk, "E22 rotated source profile preserves total tail unit power per band");

   // Diffuse box with reflective +Y wall vs absorptive other walls: rotating receiver aimAz=90 collects much more late energy
   std::istringstream boxIn(
    "sr 48000\nlength 0.4\nheight 4\nseed 7\n"
    "material Abs 0.96 0.96 0.96 0.96 0.96 0.96 0.96 0.8 0.5\n"
    "material Ref 0.05 0.05 0.05 0.05 0.05 0.05 0.05 0.8 0.5\n"
    "floor Abs\nceiling Abs\n"
    "wall 0 0 8 0 Abs\n"
    "wall 8 0 8 8 Abs\n"
    "wall 8 8 0 8 Ref\n"
    "wall 0 8 0 0 Abs\n"
    "source S 4 3.5 2 0 0\n"
    "receiver R 4 4.5 2 0 0 0 0\n");
   verb::Scene boxY = verb::parseScene(boxIn, "e22_diffuse_box");
   const auto trumpet = verb::findBuiltinDirectivity("brass_trumpet_bell_model");
   boxY.receivers[0].directivity = trumpet;
   boxY.receivers[0].instrument = trumpet->id;
   boxY.receivers[0].instrumentAmount = 1.0;
   auto boxY0 = boxY; boxY0.receivers[0].aimAzDeg = 0.0;
   auto boxY90 = boxY; boxY90.receivers[0].aimAzDeg = 90.0;
   auto boxYm90 = boxY; boxYm90.receivers[0].aimAzDeg = -90.0;
   const auto tY0 = verb::traceTail(boxY0, boxY0.sources[0], boxY0.receivers, 1, 16000, boxY0.seed, true);
   const auto tY90 = verb::traceTail(boxY90, boxY90.sources[0], boxY90.receivers, 1, 16000, boxY90.seed, true);
   const auto tYm90 = verb::traceTail(boxYm90, boxYm90.sources[0], boxYm90.receivers, 1, 16000, boxYm90.seed, true);
   double eY0 = 0, eY90 = 0, eYm90 = 0;
   for (size_t k = 0; k < tY0.nBins; ++k) { eY0 += tY0.h[0][6][k]; eY90 += tY90.h[0][6][k]; eYm90 += tYm90.h[0][6][k]; }
   printf("E22 diffuse-box +Y reflective receiver late energy (8kHz): aimAz=0 -> %.4e, aimAz=+90 -> %.4e, aimAz=-90 -> %.4e (ratio +90/-90 = %.2f)\n",
          eY0, eY90, eYm90, eY90 / std::max(1e-30, eYm90));
   check(eY90 > eY0 && eY90 > eYm90 * 1.4, "E22 tail receiver directionality follows corrected lobe (aimAz=+90 > 0 and >> -90 toward reflective +Y wall)");
  }

  // Moving-source bake accuracy gate (<= 1 dB) + outdating when profile aim changes
  {
   std::istringstream studioIn(
    "sr 48000\nlength 0.6\nheight 3.2\ntemp 20\nhumidity 50\norder 1\nrays 8000\nseed 3\ndetect_radius 0.5\n"
    "material Wall 0.14 0.16 0.18 0.20 0.22 0.24 0.26 0.15 0.45\nclass Wall 0\n"
    "floor Wall\nceiling Wall\n"
    "wall 0 0 6 0 Wall\nwall 6 0 6 5 Wall\nwall 6 5 0 5 Wall\nwall 0 5 0 0 Wall\n"
    "source S0 1.5 1.5 1.4 10 0\n"
    "receiver R0 4.5 2.0 1.4 0 0 0 0\n"
    "receiver R1 4.5 3.2 1.6 45 0 0 0\n");
   verb::Scene scBake = verb::parseScene(studioIn, "e22_bake_room");
   const auto trumpet = verb::findBuiltinDirectivity("brass_trumpet_bell_model");
   scBake.sources[0].directivity = trumpet;
   scBake.sources[0].instrument = trumpet->id;
   scBake.sources[0].instrumentAmount = 1.0;
   scBake.sources[0].aimAzDeg = 90.0;
   scBake.sources[0].aimElDeg = 30.0;
   scBake.sources[0].aimRollDeg = -25.0;

   verb::BakedPath bp;
   bp.object = 0;
   bp.group = -1;
   bp.poses = {{{1.4, 1.4, 1.4, 10.0}}, {{3.2, 3.0, 1.5, 45.0}}};
   bp.distance = {0.0, verb::norm(verb::Vec3{1.8, 1.6, 0.1})};

   verb::EngineConfig cfg;
   cfg.tailThread = false;
   cfg.erOrder = 1;
   cfg.tailRays = 8000;
   cfg.adaptiveTail = true;
   cfg.sharedTail = false;
   cfg.rebuildWorkers = 2;
   cfg.manualWorker = true;
   verb::Engine eng(scBake, cfg);
   eng.setBakedPaths({bp});
   eng.requestPathBake(2);

   std::vector<float> in(128, 0.01f);
   std::vector<std::vector<float>> out((size_t)eng.numReceivers(), std::vector<float>(128, 0.f));
   const float* ip[1] = {in.data()};
   std::vector<float*> op((size_t)eng.numReceivers());
   for (int r = 0; r < eng.numReceivers(); ++r) op[(size_t)r] = out[(size_t)r].data();
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
   while (eng.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) {
    eng.process(ip, op.data(), 128);
    eng.runWorkerStep();
   }
   check(eng.bakeStats().state == verb::Engine::BakeState::Baked, "E22 moving-source bake with corrected aim completes");

   const auto tables = eng.bakedTablesSnapshot();
   const auto scSnap = eng.sceneSnapshot();
   const verb::Scene& traceSc = scSnap ? *scSnap : scBake;
   const int R = eng.numReceivers();
   double worstDb = 0.0;
   for (double u : {0.2, 0.55, 0.85}) {
    std::vector<double> pred(tables[0]->stride(), 0.0);
    check(tables[0]->lookup(u, pred.data()), "E22 baked table lookup succeeds");
    const auto pose = bp.at(u);
    verb::Transducer srcAt = traceSc.sources[0];
    srcAt.pos = {pose[0], pose[1], pose[2]};
    srcAt.azimuthDeg = pose[3];
    const auto ref = verb::traceTailEnergy(traceSc, srcAt, traceSc.receivers, cfg.erOrder, 16000, traceSc.seed);
    for (int r = 0; r < R; ++r) {
     double peak = 0.0;
     for (int b = 0; b < verb::NB; ++b) peak = std::max(peak, ref[(size_t)r][(size_t)b]);
     for (int b = 0; b < verb::NB; ++b) {
      const double rv = ref[(size_t)r][(size_t)b];
      const double pv = pred[(size_t)r * verb::NB + (size_t)b];
      if (rv >= peak * 1e-4 && peak > 0.0) {
       worstDb = std::max(worstDb, std::abs(10.0 * std::log10(std::max(1e-30, pv) / std::max(1e-30, rv))));
      }
     }
    }
   }
   printf("E22 moving-source bake with corrected aim max error=%.3f dB\n", worstDb);
   check(worstDb <= 1.0, "E22 moving-source baked table with corrected aim accuracy <= 1.0 dB");

   // Changing profile aim correction invalidates baked table
   eng.setSourceDirectivity(0, trumpet, 1.0, traceSc.sources[0].pattern, 45.0, 30.0, -25.0);
   std::vector<float> zeroIn(128, 0.f);
   const float* zip[1] = {zeroIn.data()};
   eng.process(zip, op.data(), 128);
   eng.runWorkerStep();
   check(eng.bakeStats().state == verb::Engine::BakeState::Outdated,
         "E22 changing profile aim correction outdates baked table");
  }
 }
 return fails?1:0;
}
