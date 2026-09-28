#pragma once
// C++ state behind engine console classes. A script object whose class is
// (or derives from) a registered engine class carries one EngineObject of
// the most derived registered class; console methods reach it through
// EngineObjects::get<T>().
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ScriptObject;
struct VMValue;

class EngineObject {
public:
    virtual ~EngineObject() = default;
    ScriptObject* script = nullptr;
    // GameBase objects run once per 32 ms server tick (gServerProcessList).
    virtual bool processesTicks() const { return false; }
    virtual void processTick() {}
};

namespace EngineObjects {

using Factory = std::function<std::shared_ptr<EngineObject>()>;
void registerClass(const std::string& engineClass, Factory factory);
// Attach state for the object's class, if an ancestor is registered.
void attach(ScriptObject* object);
EngineObject* find(const std::string& handle);

template <class T> T* get(const std::string& handle) {
    return dynamic_cast<T*>(find(handle));
}

// Objects with tick processing, in creation order.
void forEachTicking(const std::function<void(EngineObject&)>& visit);

} // namespace EngineObjects
