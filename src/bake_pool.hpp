#pragma once
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <exception>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef far
#undef far
#endif
#ifdef near
#undef near
#endif
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif
namespace verb {
// Created lazily on the engine worker. Never entered/destroyed by process().
class BakePool {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> jobs;
    std::deque<std::function<void()>> analysisJobs;
    std::vector<std::thread> threads;
    bool stopping=false;
public:
    static void lowerCurrentThread(bool analysis=false) noexcept {
       #if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(),analysis?THREAD_PRIORITY_LOWEST:THREAD_PRIORITY_BELOW_NORMAL);
       #elif defined(__APPLE__)
        pthread_set_qos_class_self_np(analysis?QOS_CLASS_BACKGROUND:QOS_CLASS_UTILITY,0);
       #elif defined(__linux__)
        setpriority(PRIO_PROCESS,0,analysis?10:5);
       #else
        (void)analysis;
       #endif
    }
    explicit BakePool(unsigned count){
        try{for(unsigned i=0;i<count;++i)threads.emplace_back([this]{
            lowerCurrentThread(true);
            for(;;){std::function<void()>job;{std::unique_lock<std::mutex>lock(mutex);ready.wait(lock,[this]{return stopping||!jobs.empty()||!analysisJobs.empty();});if(stopping&&jobs.empty()&&analysisJobs.empty())return;auto& queue=jobs.empty()?analysisJobs:jobs;job=std::move(queue.front());queue.pop_front();}job();}
        });}catch(...){ {std::lock_guard<std::mutex>lock(mutex);stopping=true;}ready.notify_all();for(auto&t:threads)t.join();throw;}
    }
    ~BakePool(){ {std::lock_guard<std::mutex>lock(mutex);stopping=true;}ready.notify_all();for(auto&t:threads)t.join(); }
    unsigned size() const noexcept {return (unsigned)threads.size();}
    template<class F> auto submit(F f, bool urgent=false, bool analysis=false)->std::future<decltype(f())>{
        auto task=std::make_shared<std::packaged_task<decltype(f())()>>(std::move(f));auto result=task->get_future();
        {std::lock_guard<std::mutex>lock(mutex);if(analysis)analysisJobs.emplace_back([task]{(*task)();});else if(urgent)jobs.emplace_front([task]{(*task)();});else jobs.emplace_back([task]{(*task)();});}ready.notify_one();return result;
    }
    // Coordinator only: no nested waits on this pool. Join EVERY task even on
    // failure, before releasing references to the caller's immutable job data.
    template<class F> void parallelFor(size_t count,unsigned width,F fn){
        width=(unsigned)std::min<size_t>(count,std::min(width,size()));
        if(width<=1){for(size_t i=0;i<count;++i)fn(i);return;}
        std::atomic<bool> failed{false};std::vector<std::future<void>> work;
        std::exception_ptr error;work.reserve(width);
        try{for(unsigned j=0;j<width;++j)work.push_back(submit([&,j]{
            try{for(size_t i=j;i<count&&!failed.load();i+=width)fn(i);}
            catch(...){failed=true;throw;}
        },true));}catch(...){failed=true;error=std::current_exception();}
        for(auto& task:work)try{task.get();}catch(...){if(!error)error=std::current_exception();}
        if(error)std::rethrow_exception(error);
    }
};
}
