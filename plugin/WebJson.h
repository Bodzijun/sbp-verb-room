#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
namespace verbweb {
inline juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> props = {}) {
    auto* o = new juce::DynamicObject(); for (const auto& p : props) o->setProperty(p.first, p.second); return juce::var(o);
}
inline juce::var array(std::initializer_list<juce::var> v) { juce::Array<juce::var> a; for (const auto& x : v) a.add(x); return a; }
template<class T> juce::var numbers(const T& v) { juce::Array<juce::var> a; for (auto x : v) a.add(juce::var(x)); return a; }
// Last serialization boundary: invalid diagnostics must never produce NaN/Inf JSON.
// Ray geometry is rejected earlier, rather than inventing a replacement position.
inline juce::var finiteJson(juce::var value) {
    if(value.isDouble()&&!std::isfinite((double)value))return 0.;
    if(auto* a=value.getArray())for(auto& item:*a)item=finiteJson(item);
    if(auto* o=value.getDynamicObject())for(int i=0;i<o->getProperties().size();++i){const auto key=o->getProperties().getName(i);o->setProperty(key,finiteJson(o->getProperty(key)));}
    return value;
}
inline juce::String decimal(uint64_t n) { return juce::String(std::to_string(n)); }
inline juce::var prop(const juce::var& v, const char* k) { return v.getProperty(k, {}); }
}
