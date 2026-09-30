#pragma once
// AITask (ai/aiTask.cc): a bot's weighted task. Its script namespace is the
// task name, whose weight/monitor/assume/retire methods the bot calls.
// AISlicer: the budget that spreads those calls over the ticks.
#include "sim/engine_object.h"
#include <algorithm>
#include <cstdint>
#include <string>

class AIConnection;

class AITask : public EngineObject {
public:
    void setWeightFreq(int freq) { mWeightFreq = std::max(1, freq); }
    void setWeight(int weight) { mWeight = weight; }
    void reWeight() { mWeightCounter = 0; }
    int getWeight() const { return mWeight; }
    void setMonitorFreq(int freq) { mMonitorFreq = std::max(1, freq); }
    void reMonitor() { mMonitorCounter = 0; }
    void calcWeight(AIConnection* ai);
    void monitor(AIConnection* ai);
    void assume(AIConnection* ai);
    void retire(AIConnection* ai);
    std::string name; // the task's name (its namespace)

private:
    void call(const char* method, AIConnection* ai);
    int mWeightFreq = 30, mWeightCounter = 0, mMonitorFreq = 30, mMonitorCounter = 0, mWeight = 0;
};

class AISlicer {
public:
    AISlicer() { reset(); }
    void init(uint32_t delay = 30, int maxPerTic = 1);
    void reset() { init(); }
    bool ready(int& counter, int resetValue);

private:
    uint32_t mDelay = 30, mLastTime = 0;
    float mBudget = 2.0f;
};

AISlicer& gCalcWeightSlicer();
AISlicer& gScriptEngageSlicer();
uint32_t simTimeMs();
