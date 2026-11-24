// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

class Buffer;
class PageManager;

/**
 * Upload worker (async_uploads): a thread doing for the command processor the costly part of
 * buffer uploads, on another core. The command processor decides what to upload and records the
 * GPU copies from staging memory; the worker then protects the pages again (system calls), and
 * only afterwards copies guest memory to the staging memory (so a guest write is either in the
 * copy or faults and makes the page dirty again). The submission of the recorded copies waits
 * until the worker did the jobs given before (Scheduler::RequireBeforeSubmit).
 */
class UploadWorker {
public:
    struct Copy {
        VAddr src; ///< Guest address.
        u8* dst;   ///< Mapped staging memory.
        u64 size;
    };

    struct Job {
        std::vector<std::pair<u64, u64>> pages; ///< Deferred protections (page numbers).
        std::vector<Copy> copies;
        Buffer* flush_buffer{}; ///< Staging buffer flushed once written (non coherent memory).
        u64 flush_offset{};
        u64 flush_size{};
        /// ISS_UPLOAD_QUEUE: once the staging memory is written, the copy engine copies it into
        /// device memory (engine_src -> engine_dst) and signals engine_tick (0: no engine copy).
        vk::Buffer engine_src{};
        vk::Buffer engine_dst{};
        vk::BufferCopy engine_copy{};
        u64 engine_tick{};
    };

    /// Called with each batch once its staging memory is written, before its jobs count as
    /// done (on the thread doing them, under exec_mutex, so in job order).
    void SetAfterBatch(std::function<void(std::span<const Job>)>&& callback) {
        std::scoped_lock lk{exec_mutex};
        after_batch = std::move(callback);
    }

    explicit UploadWorker(PageManager& page_manager);
    ~UploadWorker();

    UploadWorker(const UploadWorker&) = delete;
    UploadWorker& operator=(const UploadWorker&) = delete;

    /// Gives a job to the worker. Returns its number, for Wait.
    u64 Enqueue(Job&& job);

    /// Returns once the jobs up to this number are done (does them itself if the worker did not
    /// start them yet).
    void Wait(u64 seq);

    /// Returns once all the jobs given so far are done.
    void WaitIdle() {
        Wait(enqueued.load(std::memory_order_acquire));
    }

private:
    void Run(std::stop_token stoken);

    /// Does the queued jobs (the caller holds exec_mutex).
    void ExecuteQueued();

    PageManager& page_manager;
    std::mutex queue_mutex;
    std::condition_variable_any queue_cv;
    std::vector<Job> queue;
    bool worker_sleeping{};
    std::mutex exec_mutex; ///< Held while jobs are done: they are done in order.
    std::vector<Job> batch;
    std::function<void(std::span<const Job>)> after_batch;
    std::vector<std::pair<u64, u64>> ranges;
    std::atomic<u64> enqueued{};
    std::atomic<u64> completed{};
    std::jthread thread;
};

} // namespace VideoCore
