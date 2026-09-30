#include "ai/ai_task.h"
#include "ai/ai_connection.h"
#include "sim/sim_state.h"
#include "script/script_engine.h"
#include "script/torquescript.h"

uint32_t simTimeMs() { return (uint32_t)(SimState::simTime() * 1000.0); }

AISlicer& gCalcWeightSlicer() {
    static AISlicer slicer;
    return slicer;
}

AISlicer& gScriptEngageSlicer() {
    static AISlicer slicer;
    return slicer;
}

void AISlicer::init(uint32_t delay, int) {
    mDelay = delay;
    mLastTime = simTimeMs();
    mBudget = 2.0f;
}

// AISlicer::ready: a counter that reached zero goes through while the
// budget (one per mDelay ms, at most 40 banked) allows; one that has waited
// ~1.2 s goes through regardless. Counters are in ticks, the delay in ms.
bool AISlicer::ready(int& counter, int resetValue) {
    bool goodToGo = false;
    counter--;
    if (mBudget > 0) {
        if (counter < 0) {
            mBudget -= 1.0f;
            counter = resetValue;
            goodToGo = true;
        }
    } else {
        const uint32_t t = simTimeMs();
        mBudget += float(t - mLastTime) / float(mDelay);
        mBudget = std::min(mBudget, 40.0f);
        mLastTime = t;
        if (counter < -37) {
            counter = resetValue;
            goodToGo = true;
        }
    }
    return goodToGo;
}

// Con::executef(this, 2, method, clientId): the task's namespace method.
void AITask::call(const char* method, AIConnection* ai) {
    auto* ts = ScriptEngine::instance().ts();
    if (!ts || !script || !ai) return;
    ts->callObjectMethod(ScriptEngine::instance().objectKey(script), method, {VMValue(std::to_string(ai->id()))});
}

void AITask::calcWeight(AIConnection* ai) {
    if (!ai) return;
    // see if it's time to re-weight the task
    if (gCalcWeightSlicer().ready(mWeightCounter, mWeightFreq)) call("weight", ai);
}

void AITask::monitor(AIConnection* ai) {
    if (!ai) return;
    if (--mMonitorCounter <= 0) {
        mMonitorCounter = mMonitorFreq;
        call("monitor", ai);
    }
}

void AITask::assume(AIConnection* ai) {
    if (!ai) return;
    mMonitorCounter = 0;
    call("assume", ai);
}

void AITask::retire(AIConnection* ai) {
    if (!ai) return;
    call("retire", ai);
}
