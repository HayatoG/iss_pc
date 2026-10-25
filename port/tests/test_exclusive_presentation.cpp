// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu_v2/gpu/swapchain_policy.h"
#include "../gpu_v2/gpu/swapchain_drain.h"
#include "../gpu/shim/tlg_graphics.h"
#include <vulkan/vulkan_core.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstdio>
int main(){
    namespace P=Vulkan::WsiPolicy;
    assert(P::Acquire(VK_SUCCESS)==P::Action::Continue);
    assert(P::Acquire(VK_NOT_READY)==P::Action::Skip);
    assert(P::Acquire(VK_TIMEOUT)==P::Action::Skip);
    assert(P::Acquire(VK_SUBOPTIMAL_KHR)==P::Action::Recreate);
    assert(P::Acquire(VK_ERROR_OUT_OF_DATE_KHR)==P::Action::Recreate);
    assert(P::Acquire(VK_ERROR_SURFACE_LOST_KHR)==P::Action::Recreate);
    assert(P::Acquire(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)==P::Action::ExclusiveLost);
    assert(P::Present(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)==P::Action::ExclusiveLost);
    assert(P::Present(VK_SUBOPTIMAL_KHR)==P::Action::Recreate);
    assert(P::Acquire(VK_ERROR_DEVICE_LOST)==P::Action::Fatal);
    assert(P::Present(VK_ERROR_DEVICE_LOST)==P::Action::Fatal);
    assert(P::ExclusiveAcquire(VK_SUCCESS,1)==P::Action::Continue);
    assert(P::ExclusiveAcquire(VK_ERROR_INITIALIZATION_FAILED,1)==P::Action::Skip);
    assert(P::ExclusiveAcquire(VK_ERROR_INITIALIZATION_FAILED,2)==P::Action::Skip);
    assert(P::ExclusiveAcquire(VK_ERROR_INITIALIZATION_FAILED,3)==P::Action::Fallback);
    assert(P::ExclusiveAcquire(VK_ERROR_EXTENSION_NOT_PRESENT,1)==P::Action::Fallback);
    assert(P::ExclusiveAcquire(VK_ERROR_DEVICE_LOST,1)==P::Action::Fatal);
    assert(!P::CanPresentExclusive(true,true,false));
    assert(P::CanPresentExclusive(true,true,true));
    assert(P::CanPresentExclusive(false,false,false));
    assert(TlgGraphics::ParseDisplayMode("exclusive")==TlgGraphics::ExclusiveFullscreen);
    assert(TlgGraphics::ParseDisplayMode("Fullscreen")==TlgGraphics::Fullscreen);
    assert(TlgGraphics::ParseDisplayMode("borderless")==TlgGraphics::Borderless);
    // Reproduce the reviewed queue dependency: earlier work cannot finish until
    // another producer obtains submit_mutex. Waiting under it must fail this test.
    std::mutex submit_mutex,state_mutex;std::condition_variable cv;
    bool marker=false,later_work=false,wait_completed=false;
    std::thread producer([&]{
        {std::unique_lock lock{state_mutex};cv.wait(lock,[&]{return marker;});}
        std::scoped_lock submit{submit_mutex};
        {std::scoped_lock lock{state_mutex};later_work=true;}cv.notify_one();
    });
    Vulkan::SubmitPresentationDrain(submit_mutex,[&]{
        {std::scoped_lock lock{state_mutex};marker=true;}cv.notify_one();
    },[&]{
        std::unique_lock lock{state_mutex};
        wait_completed=cv.wait_for(lock,std::chrono::seconds(2),[&]{return later_work;});
    });
    producer.join();assert(wait_completed);
    puts("PASS: queue dependency progresses outside submit lock; exclusive-loss, timeout, resize and device-loss policies; display-mode compatibility.");
}
