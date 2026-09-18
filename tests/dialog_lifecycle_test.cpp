#include "render/dialog_lifecycle.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    DialogLifecycleStack stack;
    std::vector<std::string> events;
    stack.setCallback([&](const std::string& name, DialogLifecycleStack::Event event) {
        events.push_back(name + (event == DialogLifecycleStack::Event::Wake ? ":wake" : ":sleep"));
    });

    stack.setFocus("contentEdit");
    stack.push("content");
    stack.push("modal", false);
    stack.setFocus("modalButton");
    stack.pop("modal");
    assert(stack.focus() == "contentEdit");
    assert(stack.pointerLocked() == true);
    assert((events == std::vector<std::string>{"content:wake", "modal:wake", "modal:sleep"}));

    stack.pop("content");
    events.clear();
    stack.addChild("parent", "child");
    stack.push("parent");
    stack.clear();
    assert((events == std::vector<std::string>{"parent:wake", "child:wake", "child:sleep", "parent:sleep"}));
    assert(!stack.pointerLocked());
    return 0;
}
