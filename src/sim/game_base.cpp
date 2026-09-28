#include "sim/game_base.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <cstdlib>
#include <strings.h>

std::string GameBase::handle() const {
    return script ? std::to_string(ScriptEngine::instance().objectId(script)) : std::string();
}

std::string GameBase::dataBlock() const {
    return script ? ScriptEngine::instance().objectDataBlock(script) : std::string();
}

static const VMValue* dataField(const GameBase& object, const char* field) {
    const std::string block = object.dataBlock();
    if (block.empty()) return nullptr;
    ScriptObject* data = ScriptEngine::instance().findObject(block.c_str());
    if (!data) return nullptr;
    for (const auto& [name, value] : data->fields)
        if (strcasecmp(name.c_str(), field) == 0) return &value;
    return nullptr;
}

float GameBase::dataFloat(const char* field, float fallback) const {
    const VMValue* value = dataField(*this, field);
    return value ? value->toFloat() : fallback;
}

bool GameBase::dataBool(const char* field, bool fallback) const {
    const VMValue* value = dataField(*this, field);
    return value ? value->toBool() : fallback;
}

void GameBase::callDataBlock(const char* callback, const std::vector<std::string>& extra) const {
    const std::string block = dataBlock();
    auto* ts = ScriptEngine::instance().ts();
    if (block.empty() || !ts) return;
    std::vector<VMValue> args{VMValue(handle())};
    for (const auto& value : extra) args.emplace_back(value);
    ts->callObjectMethod(block, callback, args);
}
