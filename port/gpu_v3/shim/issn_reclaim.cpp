// SPDX-License-Identifier: GPL-2.0-or-later
#include "issn_reclaim.h"

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "issn_threads.h"

namespace IssnReclaim {
namespace {
std::mutex mutex;
std::condition_variable cv;
std::vector<std::shared_ptr<const void>> pending;
std::once_flag started;

void Run() {
    IssnThreads::MakeBackground();
    std::vector<std::shared_ptr<const void>> batch;
    for (;;) {
        {
            std::unique_lock lock{mutex};
            cv.wait(lock, [] { return !pending.empty(); });
            batch.swap(pending);
        }
        batch.clear(); // the destructors run here
    }
}
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("ISSN_ASYNC_FREE");
        return e && *e == '1';
    }();
    return enabled;
}

void Later(std::shared_ptr<const void> object) {
    if (!object) {
        return;
    }
    if (!Enabled() || object.use_count() > 1) {
        return; // destroyed here, or not the last reference
    }
    std::call_once(started, [] { std::thread(Run).detach(); });
    {
        std::scoped_lock lock{mutex};
        pending.push_back(std::move(object));
    }
    cv.notify_one();
}
} // namespace IssnReclaim
