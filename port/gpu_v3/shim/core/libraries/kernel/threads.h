// port: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("port:hle");
            func(stop);
            done->store(true);
        });
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    // port: these are called from guest code, which a C++ exception cannot unwind: a failed
    // join is logged and the thread detached instead.
    void Join() {
        if (!thread.joinable()) return;
        try {
            if (thread.get_id() == std::this_thread::get_id()) thread.detach();
#ifndef _WIN32
            else if (finished) {
                // port: Gravity Rush 2 hung 86 s in sceAvPlayerStop joining an AvPlayer decoder
                // whose function had already returned (its exit log was printed; the join only
                // came back when a debugger attached). Wait for the function itself, then let the
                // exiting thread go: after `finished` it touches nothing of its owner. Linux only:
                // on Windows detaching a thread that has already returned fails ("Invalid
                // argument"), and the join there never showed the hang.
                while (!finished->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                thread.detach();
            }
#endif
            else thread.join();
        } catch (const std::system_error& error) {
            // Windows (libc++ on Win32 threads): the join can fail although the thread is fine.
            // Wait for its function to return, so nothing it uses is freed under it, then let go.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            if (thread.get_id() != std::this_thread::get_id()) {
                for (int waited = 0; finished && !finished->load() && waited < 5000; ++waited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (thread.joinable()) thread.detach();
        }
    }
    bool Joinable() const { return thread.joinable(); }
    /// port: asks the thread to stop without waiting (Stop() joins); callers that hold several
    /// threads ask them all first, wake what they wait on, then join.
    void RequestStop() {
        if (thread.joinable()) thread.request_stop();
    }
    void Stop() {
        if (thread.joinable()) {
            thread.request_stop();
            Join();
        }
    }

private:
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
} // namespace Libraries::Kernel
