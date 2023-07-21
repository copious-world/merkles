#pragma once

#ifndef _TPOOL_USER_DEFS_H_
#define _TPOOL_USER_DEFS_H_


#include <vector>
#include <thread>
#include <memory>
#include <future>
#include <functional>
#include <type_traits>
#include <cassert>
#include <deque>

using namespace std;

class TaskQueue {
public:
    TaskQueue() {}
    virtual ~TaskQueue() { invalidate(); }

public:

    void invalidate(void) {
        lock_guard<mutex> lock{_m};
        _v.notify_all();
        _valid = false;
    }

    void push(packaged_task<void()> &f)
    {
        {
            lock_guard<mutex> lock{_m};
            work.emplace_back(move(p));
        }
        _v.notify_one();
    }


    void pop(packaged_task<void()> &f) {
        unique_lock<mutex> lck(_m);
        if ( _work.empty() ){
            _v.wait(lck,[&]{return !(_work.empty()) || !(_valid); });
        }
        f = move(_work.front());
        _work.pop_front();
    }

    // cancel_pending() merely cancels all non-started tasks:
    void cancel_pending() {
        unique_lock<mutex> l(_m);
        _work.clear();
    }

    void push_enders(uint8_t thread_count) {
        unique_lock<mutex> l(_m);
        for ( int i = 0; i < thread_count; i++ ) {
            _work.push_back({});        // none of these will be valid
        }
        _v.notify_all();
    }

public:

    mutable mutex                   _m;
    condition_variable              _v;
    deque<packaged_task<void()>>    _work;  // note that a packaged_task<void> can store a packaged_task<R>:
    //
    atomic_bool                     _valid{true};
};

/**
 * 
*/
class ThreadPoolUser {
public:
    ThreadPoolUser() {
        _thread_count = thread::hardware_concurrency() - 1;
    }
    virtual ~ThreadPoolUser() {}

public: 


public:

    void initialize_pool() {
        for (size_t i = 0; i < _thread_count; ++i) {
            // each thread is a async running this->worker():
            _finished.push_back(
                async(launch::async, [this]{ worker(); })
            );
        }
    }

    void finish(void) {
        _tasks.push_enders();
        _finished.clear();
    }

    // enqueue( lambda ) will enqueue the lambda into the tasks for the threads
    // to use.  A future of the type the lambda returns is given to let you get
    // the result out.

    template<class F, class R=result_of_t<F&()>>   // allow  different function types
    future<R> enqueue(F&& f) {
        packaged_task<R()> pack(forward<F>(f));
        auto r = pack.get_future(); // promise for return value
        _tasks.push(pack);
        return r; // return the future result of the task
    }

    template<class F, class R=result_of_t<F&()>>  // allow  different function types
    void enqueue_status(F&& f) {
        packaged_task<R()> pack(forward<F>(f));
        auto r = pack.get_future(); // promise for return value
        _tasks.push(pack);
        _status_futures.emplace_back(r);
    }

    bool await_status_all(void) {
        bool status = true;
        for (auto& fut:_status_futures) {
            status = status && fut.get();
        }
        return status;
    }



    // abort() cancels all non-started tasks, and tells every working thread
    // stop running, and waits for them to finish up.
    void abort() {
        _tasks.cancel_pending();
        finish();
    }


    void worker() {
        while(true){
            packaged_task<void()> f;    // pop a task off the queue:
            _tasks.pop(&f);
            if ( !f.valid() ) return;   // abort
            f();                        // run 
        }
    }

public:

    TaskQueue                       _tasks;         // shared by threads.
    uint8_t                         _thread_count;
    vector<future<void>>            _finished; // this holds futures representing the worker threads being done:

    list<future<bool>>              _status_futures;

};









#endif