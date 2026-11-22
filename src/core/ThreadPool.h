#pragma once
#include <functional>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <deque>
#include <atomic>

namespace ps {

// Pool de threads partilhado: usado para descomprimir chunks HAP em paralelo
// e para gerar miniaturas sem bloquear a interface.
class ThreadPool {
public:
    static ThreadPool& get();
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();

    void submit(std::function<void()> fn);
    // Executa fn(i) para i em [0,count) e espera que todas terminem.
    void parallelFor(unsigned count, const std::function<void(unsigned)>& fn);

private:
    void worker();
    std::vector<std::thread> m_threads;
    std::deque<std::function<void()>> m_jobs;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    bool m_stop = false;
};

} // namespace ps
