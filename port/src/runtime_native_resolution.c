#include "iss_native_resolution.h"
#include <stdatomic.h>
static atomic_int native_status;
static _Atomic(const char *) native_reason="Disabled";
static IssNativePlan active_plan;
void runtime_native_publish(int status,const char *reason,const IssNativePlan *plan) {
    if(plan)active_plan=*plan;else memset(&active_plan,0,sizeof(active_plan));
    atomic_store(&native_reason,reason);
    atomic_store_explicit(&native_status,status,memory_order_release);
}
int runtime_native_status(void){return atomic_load_explicit(&native_status,memory_order_acquire);}
const char *runtime_native_reason(void){return atomic_load(&native_reason);}
const IssNativePlan *runtime_native_plan(void){return &active_plan;}
float runtime_native_safe_area(void){
    return runtime_native_status()==1 && !active_plan.hud_edges ? 1920.0f/active_plan.ui_width : 1.0f;
}
