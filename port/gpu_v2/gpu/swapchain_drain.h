// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <mutex>
namespace Vulkan {
// A preceding GPU batch may wait on work submitted later. Serialize only the
// drain marker submission; never prevent that later work while waiting.
template<class Submit,class Wait>
void SubmitPresentationDrain(std::mutex& mutex,Submit submit,Wait wait){
    {std::scoped_lock lock{mutex};submit();}
    wait();
}
}
