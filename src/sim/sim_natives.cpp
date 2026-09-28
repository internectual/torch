// Engine console functions the retail server scripts call that belong to
// the simulation/server layer rather than the client.
#include "sim/sim_natives.h"
#include "sim/engine_crc.h"
#include "sim/sim_state.h"
#include "core/engine.h"
#include "script/torquescript.h"
#include "sim/shape_base.h"
#include "sim/net_string_table.h"

void registerSimNatives(TorqueScript& ts) {
    registerShapeBaseNatives(ts);

    // game/net.cc and consoleFunctions.cc tagged strings.
    ts.registerNative("addTaggedString", [](const std::vector<VMValue>& args) -> VMValue {
        return VMValue(NetStrings::tag(NetStrings::add(args.empty() ? "" : args[0].toString())));
    });
    ts.registerNative("removeTaggedString", [](const std::vector<VMValue>& args) -> VMValue {
        if (!args.empty()) NetStrings::remove(NetStrings::tagId(args[0].toString()));
        return VMValue("");
    });
    ts.registerNative("getTaggedString", [](const std::vector<VMValue>& args) -> VMValue {
        const std::string* text = args.empty() ? nullptr : NetStrings::lookup(NetStrings::tagId(args[0].toString()));
        return VMValue(text ? *text : std::string());
    });
    ts.registerNative("detag", [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string value = args[0].toString();
        if (!NetStrings::isTag(value)) return VMValue(value);
        const size_t space = value.find(' ');
        return VMValue(space == std::string::npos ? std::string() : value.substr(space + 1));
    });
    ts.registerNative("buildTaggedString", [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty()) return VMValue("");
        const std::string* format = NetStrings::lookup(NetStrings::tagId(args[0].toString()));
        if (!format) return VMValue("");
        std::string out;
        for (size_t i = 0; i < format->size(); ++i) {
            if ((*format)[i] == '%' && i + 1 < format->size() && (*format)[i + 1] >= '1' && (*format)[i + 1] <= '9') {
                const size_t index = (size_t)((*format)[i + 1] - '0');
                if (index >= args.size()) break;
                out += args[index].toString();
                ++i;
                continue;
            }
            out += (*format)[i];
        }
        return VMValue(out.substr(0, 511));
    });
    // consoleFunctions.cc getFileCRC: -1 when the file is not found.
    ts.registerNative("getFileCRC", [](const std::vector<VMValue>& args) -> VMValue {
        if (args.empty()) return VMValue(-1);
        std::vector<uint8_t> data;
        if (!Engine::instance().fs().readFile(args[0].toString().c_str(), data)) return VMValue(-1);
        return VMValue((int32_t)EngineCrc::calculate(data.data(), data.size()));
    });
    ts.registerNative("isPackage", [&ts](const std::vector<VMValue>& args) -> VMValue {
        return VMValue(!args.empty() && ts.isPackage(args[0].toString()) ? 1 : 0);
    });
    ts.registerNative("allowConnections", [](const std::vector<VMValue>& args) -> VMValue {
        SimState::server().allowConnections = !args.empty() && args[0].toBool();
        return VMValue(1);
    });
    ts.registerNative("disableCyclingConnections", [](const std::vector<VMValue>& args) -> VMValue {
        SimState::server().cyclingConnectionsDisabled = !args.empty() && args[0].toBool();
        return VMValue(1);
    });
    ts.registerNative("startHeartbeat", [](const std::vector<VMValue>&) -> VMValue {
        SimState::server().heartbeat = true;
        return VMValue(1);
    });
    ts.registerNative("stopHeartbeat", [](const std::vector<VMValue>&) -> VMValue {
        SimState::server().heartbeat = false;
        return VMValue(1);
    });
    ts.registerNative("getGravity", [](const std::vector<VMValue>&) -> VMValue {
        return VMValue(SimState::server().gravity);
    });
    ts.registerNative("setGravity", [](const std::vector<VMValue>& args) -> VMValue {
        if (!args.empty()) SimState::server().gravity = args[0].toFloat();
        return VMValue(1);
    });
    // ResManager::purge frees unreferenced cached resources; Torch's
    // resource caches hold nothing the scripts can observe.
    ts.registerNative("purgeResources", [](const std::vector<VMValue>&) -> VMValue {
        return VMValue(1);
    });
}
