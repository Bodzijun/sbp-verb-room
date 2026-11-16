// E11 focused tests, intentionally excluded from the default suite.
static void testE11Length() {
    TailHist h; h.nBins=20000; h.binDur=.001; h.h.resize(2);
    for(auto& r:h.h)for(auto& b:r)b.assign(h.nBins,0.);
    h.h[0][0][0]=1.; h.h[0][0][5000]=1e-4;
    check(std::abs(tailSecondsFromTrace(h,.2)-5.801)<1e-9,
        "E11 double slope retains late energy instead of extrapolating early fit");
    h.h[1][6][0]=1.; h.h[1][6][8000]=1e-4;
    check(std::abs(tailSecondsFromTrace(h,.2)-8.801)<1e-9,
        "E11 maximum crossing over all receivers and bands");
    h.h[1][6][19999]=1e-4;
    check(tailSecondsFromTrace(h,.2)==20.,"E11 cap includes unresolved late energy");
    for(auto& r:h.h)for(auto& b:r)std::fill(b.begin(),b.end(),0.);
    check(tailSecondsFromTrace(h,3.)==3.,"E11 silent probe respects authored minimum");
}

static void testE11Sky() {
    auto s=builtinSceneNamed("Bedroom");s.clutter.count=0;s.roughness=0;
    // A deterministic shell, independent of builtin source anchoring.
    s.walls.clear();s.height=3.;s.ceilMat=s.floorMat=0;s.ceilZones.clear();s.floorZones.clear();
    s.walls={{0,0,4,0,0},{4,0,4,8,0},{4,8,0,8,0},{0,8,0,0,0}};
    for(auto& m:s.mats)m.tau.fill(0.);
    CeilingPlan closed(s);check(closed.contains(2,4)&&!closed.contains(2,-.01),"E11 closed footprint excludes outside");
    check(ceilMatAt(s,2,-1,&closed)<0&&floorMatAt(s,2,-1)==0,"E11 sky outside, infinite ground unchanged");
    s.walls.pop_back();CeilingPlan open(s);
    check(open.contains(.01,4)&&!open.contains(-.01,4),"E11 open end ends on straight gap line");
    Zone z;z.mat=0;z.poly={{-3,2},{-1,2},{-1,6},{-3,6}};s.ceilZones.push_back(z);
    check(ceilMatAt(s,-2,4,&open)==0,"E11 explicit canopy outside");
    z.mat=-1;z.poly={{1,2},{3,2},{3,6},{1,6}};s.ceilZones.push_back(z);
    check(ceilMatAt(s,2,4,&open)<0,"E11 explicit opening inside");
    auto text=serializeScene(s);std::istringstream input(text);auto reload=parseScene(input,"E11");
    check(!reload.ceilingInfinite&&serializeScene(reload)==text,"E11 walls scene roundtrip");
    s.ceilingInfinite=true;text=serializeScene(s);std::istringstream legacy(text);reload=parseScene(legacy,"E11");
    check(reload.ceilingInfinite&&ceilMatAt(reload,-20,4)==0,"E11 infinite scene roundtrip");
    s.ceilZones.clear();s.ceilingInfinite=false;s.walls.resize(1);CeilingPlan baffle(s);
    check(baffle.polygons.empty()&&ceilMatAt(s,100,100,&baffle)==0,"E11 baffle scene keeps infinite ceiling");
    s.walls={{0,0,4,0,0},{4,0,4,2,0},{4,2,2,2,0},{2,2,2,4,0},{2,4,0,4,0},{0,4,0,0,0}};
    CeilingPlan concave(s);check(concave.contains(1,3)&&!concave.contains(3,3),"E11 concave shell is not convex hull");
    for(const auto& room:verbp::builtinRooms()) {
        std::istringstream in(room.text);auto walls=parseScene(in,room.name);walls.length=2.;auto infinite=walls;infinite.ceilingInfinite=true;
        auto a=traceTail(walls,walls.sources[0],walls.receivers,1,4096,walls.seed,false);
        auto b=traceTail(infinite,infinite.sources[0],infinite.receivers,1,4096,infinite.seed,false);
        bool same=a.h==b.h;
        auto ea=earlyFieldEnergy(walls,walls.sources[0],walls.receivers[0].pos),eb=earlyFieldEnergy(infinite,infinite.sources[0],infinite.receivers[0].pos);
        bool early=ea.direct==eb.direct&&ea.early==eb.early;
        printf("E11 builtin %s raw %s early %s footprint %zu\n",room.name.c_str(),same?"identical":"CHANGED",early?"identical":"CHANGED",CeilingPlan(walls).polygons.size());
        // Closed opaque shells cannot reach the sky from inside. Openings and
        // transmitting walls may legitimately change late paths outside.
        if(room.name=="Bedroom"||room.name=="Living room"||room.name=="Bathroom (tiles)")check(same&&early,"E11 opaque closed room raw and full early energy bit identity");
    }
}
