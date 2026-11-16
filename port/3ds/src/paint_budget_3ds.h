#pragma once
#include <algorithm>
#include <cstdint>
namespace n3ds {
// A tick contributes time to a paint budget. Keeping the remainder allows
// e.g. 24 paintings/second instead of quantizing every cost above33ms to15.
// No clock adjustment: simulation ticks keep their existing30Hz schedule.
struct PaintBudget {
  int64_t credit=0;
  bool decide(int64_t period,int64_t cost,bool ready,bool forced,bool catchUp){
    cost=std::max(cost,period);
    credit=std::min(credit+period,cost+period); // no backlog of paints after a stall
    const bool paint=(ready && credit>=cost && !catchUp)||forced;
    if(paint)credit=std::max(-cost,credit-cost);
    return paint;
  }
};
}
