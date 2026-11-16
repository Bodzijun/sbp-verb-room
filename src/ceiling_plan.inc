// Included inside namespace verb after Scene and pointInZone. Construct off audio.
// Pair free ends shortest-first; never add a crossing or duplicate edge. Virtual
// segments close doorways/open ends, but never become acoustic wall surfaces.
struct CeilingPlan {
    std::vector<Zone> polygons;
    struct Bounds { double x0,x1,y0,y1; bool rectangle; };
    std::vector<Bounds> bounds;
    bool contains(double x,double y) const {
        for(size_t index=0;index<polygons.size();++index) {
            const auto& p=polygons[index]; const auto& b=bounds[index];
            if(x<b.x0-1e-7||x>b.x1+1e-7||y<b.y0-1e-7||y>b.y1+1e-7)continue;
            if(b.rectangle)return true;
            if(pointInZone(p,x,y))return true;
            for(size_t j=0;j<p.poly.size();++j){auto a=p.poly[j],b=p.poly[(j+1)%p.poly.size()];
                const double dx=b.first-a.first,dy=b.second-a.second,l=dx*dx+dy*dy;
                const double t=l>0?((x-a.first)*dx+(y-a.second)*dy)/l:0;
                if(t>=0&&t<=1&&std::hypot(x-a.first-t*dx,y-a.second-t*dy)<1e-7)return true;
            }
        }return false;
    }
    explicit CeilingPlan(const Scene& s) {
        using P=std::pair<double,double>;
        std::vector<P> pts;std::vector<std::pair<int,int>> edges;
        auto vertex=[&](P p){for(size_t i=0;i<pts.size();++i)if(std::hypot(p.first-pts[i].first,p.second-pts[i].second)<1e-3)return (int)i;pts.push_back(p);return (int)pts.size()-1;};
        // Architectural walls at mid-height; ignore low furniture/transparent baffles.
        std::vector<const Wall*> walls;
        for(const auto& w:s.walls)if(w.z0<=s.height*.5&&w.z1>=s.height*.5&&w.mat>=0&&s.mats[(size_t)w.mat].tau[3]<.98){
            walls.push_back(&w);vertex({w.ax,w.ay});vertex({w.bx,w.by});
        }
        auto add=[&](int a,int b){if(a==b)return;for(auto e:edges)if((e.first==a&&e.second==b)||(e.first==b&&e.second==a))return;edges.push_back({a,b});};
        // Split at T-junctions before walking planar faces.
        for(auto* w:walls){const double dx=w->bx-w->ax,dy=w->by-w->ay,l=dx*dx+dy*dy;if(l<1e-12)continue;
            std::vector<std::pair<double,int>> along;
            for(size_t i=0;i<pts.size();++i){double t=((pts[i].first-w->ax)*dx+(pts[i].second-w->ay)*dy)/l;
                if(t>=-1e-6&&t<=1+1e-6&&std::hypot(pts[i].first-w->ax-t*dx,pts[i].second-w->ay-t*dy)<1e-3)along.push_back({t,(int)i});}
            std::sort(along.begin(),along.end());for(size_t i=1;i<along.size();++i)add(along[i-1].second,along[i].second);
        }
        const size_t realEdges=edges.size();std::vector<int> degree(pts.size());for(auto e:edges){++degree[e.first];++degree[e.second];}
        auto cross=[](P a,P b,P c){return (b.first-a.first)*(c.second-a.second)-(b.second-a.second)*(c.first-a.first);};
        struct Gap{double d;int a,b;};std::vector<Gap> gaps;
        for(int a=0;a<(int)pts.size();++a)if(degree[a]==1)for(int b=a+1;b<(int)pts.size();++b)if(degree[b]==1)
            gaps.push_back({std::hypot(pts[a].first-pts[b].first,pts[a].second-pts[b].second),a,b});
        std::sort(gaps.begin(),gaps.end(),[](const Gap&a,const Gap&b){if(a.d!=b.d)return a.d<b.d;return std::pair<int,int>{a.a,a.b}<std::pair<int,int>{b.a,b.b};});
        for(auto gap:gaps){int a=gap.a,b=gap.b;if(degree[a]!=1||degree[b]!=1)continue;bool blocked=false;
            for(auto e:edges){if((e.first==a&&e.second==b)||(e.first==b&&e.second==a)){blocked=true;break;}
                if(e.first==a||e.first==b||e.second==a||e.second==b)continue;
                if(cross(pts[a],pts[b],pts[e.first])*cross(pts[a],pts[b],pts[e.second])<0&&cross(pts[e.first],pts[e.second],pts[a])*cross(pts[e.first],pts[e.second],pts[b])<0){blocked=true;break;}}
            if(!blocked){add(a,b);++degree[a];++degree[b];}
        }
        std::vector<std::vector<int>> adj(pts.size());for(auto e:edges){adj[e.first].push_back(e.second);adj[e.second].push_back(e.first);}
        for(size_t i=0;i<pts.size();++i)std::sort(adj[i].begin(),adj[i].end(),[&](int a,int b){return std::atan2(pts[a].second-pts[i].second,pts[a].first-pts[i].first)<std::atan2(pts[b].second-pts[i].second,pts[b].first-pts[i].first);});
        std::vector<std::pair<int,int>> visited;
        auto angularInside=[&](P p){int any=0,both=0,odd=0,even=0;for(int k=0;k<16;++k){double th=k*kPi/16+.001,dx=std::cos(th),dy=std::sin(th);int pos=0,neg=0;
            for(auto* w:walls){double ex=w->bx-w->ax,ey=w->by-w->ay,den=dx*ey-dy*ex;if(std::abs(den)<1e-12)continue;
                double ax=w->ax-p.first,ay=w->ay-p.second,t=(ax*ey-ay*ex)/den,u=(ax*dy-ay*dx)/den;
                if(u<0||u>1)continue;if(t>1e-9)++pos;else if(t< -1e-9)++neg;}
            any+=(pos>0)+(neg>0);both+=(pos>0&&neg>0);odd+=(pos&1)+(neg&1);even+=(pos>0&&!(pos&1))+(neg>0&&!(neg&1));}
            return both==16||(any>=17&&both>=1&&odd>even);};
        for(auto edge:edges)for(int direction=0;direction<2;++direction){int a=direction?edge.second:edge.first,b=direction?edge.first:edge.second;
            const auto first=std::pair<int,int>{a,b};if(std::find(visited.begin(),visited.end(),first)!=visited.end())continue;
            Zone poly;bool virtualEdge=false,closed=false;double area=0;
            for(size_t step=0;step<=edges.size()*2;++step){auto key=std::pair<int,int>{a,b};if(step&&key==first){closed=true;break;}if(std::find(visited.begin(),visited.end(),key)!=visited.end())break;
                visited.push_back(key);poly.poly.push_back(pts[a]);area+=pts[a].first*pts[b].second-pts[b].first*pts[a].second;
                for(size_t i=realEdges;i<edges.size();++i)if((edges[i].first==a&&edges[i].second==b)||(edges[i].first==b&&edges[i].second==a))virtualEdge=true;
                const auto& n=adj[b];auto it=std::find(n.begin(),n.end(),a);if(it==n.end()||n.empty())break;
                int c=n[((size_t)(it-n.begin())+n.size()-1)%n.size()];a=b;b=c;
            }
            if(!closed||area<1e-6||poly.poly.size()<3)continue; // positive bounded faces only
            bool accepted=!virtualEdge;
            if(!accepted){P centre{};for(auto p:poly.poly){centre.first+=p.first/poly.poly.size();centre.second+=p.second/poly.poly.size();}
                accepted=pointInZone(poly,centre.first,centre.second)&&angularInside(centre);
                for(size_t j=0;!accepted&&j<poly.poly.size();++j){auto p=poly.poly[j],q=poly.poly[(j+1)%poly.poly.size()];double dx=q.first-p.first,dy=q.second-p.second,l=std::hypot(dx,dy);if(l<=0)continue;
                    P probe{(p.first+q.first)*.5-dy/l*.01,(p.second+q.second)*.5+dx/l*.01};accepted=pointInZone(poly,probe.first,probe.second)&&angularInside(probe);}}
            if(accepted) {
                Bounds bound{1e30,-1e30,1e30,-1e30,poly.poly.size()==4};
                for(size_t j=0;j<poly.poly.size();++j){auto p=poly.poly[j],q=poly.poly[(j+1)%poly.poly.size()];bound.x0=std::min(bound.x0,p.first);bound.x1=std::max(bound.x1,p.first);bound.y0=std::min(bound.y0,p.second);bound.y1=std::max(bound.y1,p.second);bound.rectangle=bound.rectangle&&(p.first==q.first||p.second==q.second);}
                polygons.push_back(std::move(poly));bounds.push_back(bound);
            }
        }
    }
};
