#pragma once
#include <cstdlib>
#include <new>
static thread_local bool stage2AudioAllocationGuard=false;
static thread_local size_t stage2AudioAllocations=0;
void* operator new(std::size_t size) {
    if(stage2AudioAllocationGuard)++stage2AudioAllocations;
    if(auto* p=std::malloc(std::max<std::size_t>(1,size)))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
static void stage2Process(VerbProcessor& p,juce::AudioBuffer<float>& block,juce::MidiBuffer& midi){
    stage2AudioAllocationGuard=true;p.processBlock(block,midi);stage2AudioAllocationGuard=false;
}
