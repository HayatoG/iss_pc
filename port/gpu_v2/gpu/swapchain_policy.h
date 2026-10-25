// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Vulkan::WsiPolicy {
enum class Action { Continue, Skip, Recreate, ExclusiveLost, Fallback, Fatal };
constexpr Action ExclusiveAcquire(int32_t result,unsigned attempts){
    if(result==0)return Action::Continue;
    if(result==-4)return Action::Fatal; // VK_ERROR_DEVICE_LOST
    return result==-3 && attempts<3 ? Action::Skip : Action::Fallback;
}
constexpr bool CanPresentExclusive(bool configured,bool requested,bool active){
    return !(configured && requested && !active);
}
// Values are VkResult ABI constants. A timeout/lost-exclusive result must never
// be submitted as though an image-acquired semaphore had been signaled.
constexpr Action Acquire(int32_t result){
    switch(result){
    case 0:return Action::Continue;
    case 1:case 2:return Action::Skip;
    case 1000001003:case -1000001004:case -1000000000:case -13:return Action::Recreate;
    case -1000255000:return Action::ExclusiveLost;
    default:return Action::Fatal;
    }
}
constexpr Action Present(int32_t result){
    return result==0 ? Action::Continue : result==-1000255000 ? Action::ExclusiveLost :
        (result==1000001003 || result==-1000001004 || result==-1000000000) ? Action::Recreate : Action::Fatal;
}
}
