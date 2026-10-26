// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// ISS_MINIMAL_LOGS=1 (default 0): the GPU log lines written per event during play (a sparse
// arena range made resident, ~20 lines a second; a clamped buffer binding) become counts, one
// summary line per 5 s written by the thread that counts. The first events of each kind keep
// their own line. The counts follow the log level: a line the level hides is not counted.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace iss::minimal_logs {

inline bool Enabled() {
    static const bool on = [] {
        const char* p = std::getenv("ISS_MINIMAL_LOGS");
        return p && p[0] == '1';
    }();
    return on;
}

/// One kind of event, counted with an amount (amount_name: its unit; null: no amount).
class Summary {
public:
    constexpr Summary(const char* name_, const char* amount_name_, std::uint64_t first_)
        : name{name_}, amount_name{amount_name_}, first{first_} {}

    /// True while the event still gets its own line (the first `first` ones); otherwise it is
    /// only counted, and every 5 s the counts since the last summary are written.
    bool Note(std::uint64_t amount) {
        const std::uint64_t n = total.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= first) return true;
        events.fetch_add(1, std::memory_order_relaxed);
        amounts.fetch_add(amount, std::memory_order_relaxed);
        const std::int64_t now = NowMs();
        std::int64_t last = last_ms.load(std::memory_order_relaxed);
        if (!last) {
            last_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
            return false;
        }
        if (now - last < 5000 || !last_ms.compare_exchange_strong(last, now, std::memory_order_relaxed))
            return false;
        const std::uint64_t e = events.exchange(0, std::memory_order_relaxed);
        const std::uint64_t a = amounts.exchange(0, std::memory_order_relaxed);
        if (amount_name)
            std::fprintf(stderr, "%s events=%llu %s=%llu in %.1f s (ISS_MINIMAL_LOGS; %llu in all)\n", name,
                         (unsigned long long)e, amount_name, (unsigned long long)a, double(now - last) / 1000.0,
                         (unsigned long long)n);
        else
            std::fprintf(stderr, "%s events=%llu in %.1f s (ISS_MINIMAL_LOGS; %llu in all)\n", name,
                         (unsigned long long)e, double(now - last) / 1000.0, (unsigned long long)n);
        return false;
    }

private:
    static std::int64_t NowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    const char* name;
    const char* amount_name;
    std::uint64_t first;
    std::atomic<std::uint64_t> total{}, events{}, amounts{};
    std::atomic<std::int64_t> last_ms{};
};

} // namespace iss::minimal_logs
