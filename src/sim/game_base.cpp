#include "sim/game_base.h"
#include "sim/game_connection.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <cstdlib>
#include <strings.h>

std::string GameBase::handle() const {
    return script ? std::to_string(ScriptEngine::instance().objectId(script)) : std::string();
}

void GameBase::onRemove() {
    if (auto* connection = controllingClient.empty() ? nullptr : EngineObjects::get<GameConnection>(controllingClient)) {
        if (connection->controlObject() == ScriptEngine::instance().objectKey(script))
            connection->setControlObject({});
    }
    controllingClient.clear();
    SceneObject::onRemove();
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

bool GameBase::setDataBlock(ScriptObject* data) {
    if (!script || !data) return false;
    auto& engine = ScriptEngine::instance();
    if (dataBlock() == std::to_string(engine.objectId(data))) return true;
    std::string field = "dataBlock";
    for (const auto& [name, value] : script->fields)
        if (strcasecmp(name.c_str(), "dataBlock") == 0) field = name;
    script->fields[field] = VMValue(data->name.empty() ? std::to_string(engine.objectId(data)) : data->name);
    if (!onNewDataBlock()) return false;
    // scriptOnNewDataBlock, by the leaf class once everything is loaded.
    callDataBlock("onNewDataBlock");
    return true;
}

// Retail GameBase::packUpdate: the datablock, then the target id.
uint32_t GameBase::packUpdate(GameConnection&, uint32_t mask, TorqueBitWriter& w) {
    const std::string block = dataBlock();
    const int id = block.empty() ? 0 : std::atoi(block.c_str());
    if (w.writeFlag((mask & DataBlockMask) && id >= 3 && id <= 2050)) w.writeRangedU32((uint32_t)id, 3, 2050);
    if (w.writeFlag(mask & ExtendedInfoMask))
        if (w.writeFlag(targetId != -1)) w.writeInt(targetId, 9);
    return 0;
}
