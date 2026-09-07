#include "net/v12_registry.h"

namespace V12 {

const char* ghostClassName(size_t classId) {
    return classId < GhostClassCount ? GhostClassNames[classId].data() : nullptr;
}

const char* dataBlockClassName(size_t classId) {
    return classId < DataBlockClassCount ? DataBlockClassNames[classId].data() : nullptr;
}

const char* eventClassName(size_t classId) {
    return classId < EventClassCount ? EventClassNames[classId].data() : nullptr;
}

} // namespace V12
