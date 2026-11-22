#include "core/ThreadPool.h"
#include <algorithm>

namespace ps {

ThreadPool& ThreadPool::get() {
    static ThreadPool pool(std::max(2u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 1 : 2u));
    return pool;
}

ThreadPool::ThreadPool(unsigned threads) {
    for (unsigned i = 0; i < threads; ++i) m_threads.emplace_back([this] { worker(); });
}

ThreadPool::~ThreadPool() {
    { std::lock_guard<std::mutex> lk(m_mtx); m_stop = true; }
    m_cv.notify_all();
    for (auto& t : m_threads) if (t.joinable()) t.join();
}

void ThreadPool::submit(std::function<void()> fn) {
    { std::lock_guard<std::mutex> lk(m_mtx); m_jobs.push_back(std::move(fn)); }
    m_cv.notify_one();
}

void ThreadPool::worker() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait(lk, [&] { return m_stop || !m_jobs.empty(); });
            if (m_stop && m_jobs.empty()) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        try { job(); } catch (...) {}
    }
}

void ThreadPool::parallelFor(unsigned count, const std::function<void(unsigned)>& fn) {
    if (count == 0) return;
    if (count == 1) { fn(0); return; }
    // estado partilhado: jobs auxiliares podem arrancar depois de esta função
    // retornar, por isso nada pode viver na stack.
    struct State {
        std::atomic<unsigned> next{0}, done{0};
        std::mutex m; std::condition_variable cv;
        std::function<void(unsigned)> fn;
        unsigned count = 0;
    };
    auto st = std::make_shared<State>();
    st->fn = fn; st->count = count;
    auto run = [st] {
        unsigned i;
        while ((i = st->next.fetch_add(1)) < st->count) {
            try { st->fn(i); } catch (...) {}
            if (st->done.fetch_add(1) + 1 == st->count) { std::lock_guard<std::mutex> lk(st->m); st->cv.notify_all(); }
        }
    };
    unsigned helpers = std::min<unsigned>(count - 1, (unsigned)m_threads.size());
    for (unsigned h = 0; h < helpers; ++h) submit(run);
    run(); // a thread que chama também trabalha
    std::unique_lock<std::mutex> lk(st->m);
    st->cv.wait(lk, [&] { return st->done.load() >= count; });
}

} // namespace ps
