#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

// GL-free state machine documenting the supported Torque dialog contract.
class DialogLifecycleStack {
public:
    enum class Event { Wake, Sleep };
    using Callback = std::function<void(const std::string&, Event)>;

    void setCallback(Callback callback) { callback_ = std::move(callback); }
    void setFocus(std::string focus) { focus_ = std::move(focus); }
    const std::string& focus() const { return focus_; }
    bool pointerLocked() const { return pointerLocked_; }

    void push(const std::string& name, bool pointerLocked = true) {
        if (std::find(stack_.begin(), stack_.end(), name) != stack_.end()) return;
        previousFocus_[name] = focus_;
        lockStates_[name] = pointerLocked;
        stack_.push_back(name);
        pointerLocked_ = pointerLocked;
        emit(name, Event::Wake);
        for (const auto& child : children_[name]) emit(child, Event::Wake);
    }

    void pop(const std::string& name) {
        auto it = std::find(stack_.begin(), stack_.end(), name);
        if (it == stack_.end()) return;
        const auto childIt = children_.find(name);
        if (childIt != children_.end())
            for (auto child = childIt->second.rbegin(); child != childIt->second.rend(); ++child)
                emit(*child, Event::Sleep);
        emit(name, Event::Sleep);
        stack_.erase(it);
        focus_ = previousFocus_[name];
        previousFocus_.erase(name);
        lockStates_.erase(name);
        pointerLocked_ = stack_.empty() ? false : lockStates_[stack_.back()];
    }

    void clear() {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            const auto childIt = children_.find(*it);
            if (childIt != children_.end())
                for (auto child = childIt->second.rbegin(); child != childIt->second.rend(); ++child)
                    emit(*child, Event::Sleep);
            emit(*it, Event::Sleep);
        }
        stack_.clear();
        previousFocus_.clear();
        lockStates_.clear();
        focus_.clear();
        pointerLocked_ = false;
    }

    void addChild(const std::string& parent, const std::string& child) {
        children_[parent].push_back(child);
    }

private:
    void emit(const std::string& name, Event event) { if (callback_) callback_(name, event); }
    std::vector<std::string> stack_;
    std::unordered_map<std::string, std::string> previousFocus_;
    std::unordered_map<std::string, bool> lockStates_;
    std::unordered_map<std::string, std::vector<std::string>> children_;
    Callback callback_;
    std::string focus_;
    bool pointerLocked_ = false;
};
