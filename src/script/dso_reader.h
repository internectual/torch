#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>

// TGE/Tribes 2 DSO opcodes. Tribes 2 uses the TGE 1.0 map and DSO version
// 174; this ordering matches TURD's Tribes2 compiler target.
enum class DSOOpcode : uint8_t {
    OP_FUNC_DECL                = 0x00,
    OP_CREATE_OBJECT            = 0x01,
    OP_UNUSED1                  = 0x02,
    OP_UNUSED2                  = 0x03,
    OP_ADD_OBJECT               = 0x04,
    OP_END_OBJECT               = 0x05,
    OP_JMPIFFNOT                = 0x06,
    OP_JMPIFNOT                 = 0x07,
    OP_JMPIFF                   = 0x08,
    OP_JMPIF                    = 0x09,
    OP_JMPIFNOT_NP              = 0x0A,
    OP_JMPIF_NP                 = 0x0B,
    OP_JMP                      = 0x0C,
    OP_RETURN                   = 0x0D,
    OP_CMPEQ                    = 0x0E,
    OP_CMPGR                    = 0x0F,
    OP_CMPGE                    = 0x10,
    OP_CMPLT                    = 0x11,
    OP_CMPLE                    = 0x12,
    OP_CMPNE                    = 0x13,
    OP_XOR                      = 0x14,
    OP_MOD                      = 0x15,
    OP_BITAND                   = 0x16,
    OP_BITOR                    = 0x17,
    OP_NOT                      = 0x18,
    OP_NOTF                     = 0x19,
    OP_ONESCOMPLEMENT           = 0x1A,
    OP_SHR                      = 0x1B,
    OP_SHL                      = 0x1C,
    OP_AND                      = 0x1D,
    OP_OR                       = 0x1E,
    OP_ADD                      = 0x1F,
    OP_SUB                      = 0x20,
    OP_MUL                      = 0x21,
    OP_DIV                      = 0x22,
    OP_NEG                      = 0x23,
    OP_SETCURVAR                = 0x24,
    OP_SETCURVAR_CREATE         = 0x25,
    OP_SETCURVAR_ARRAY          = 0x26,
    OP_SETCURVAR_ARRAY_CREATE   = 0x27,
    OP_LOADVAR_UINT             = 0x28,
    OP_LOADVAR_FLT              = 0x29,
    OP_LOADVAR_STR              = 0x2A,
    OP_SAVEVAR_UINT             = 0x2B,
    OP_SAVEVAR_FLT              = 0x2C,
    OP_SAVEVAR_STR              = 0x2D,
    OP_SETCUROBJECT             = 0x2E,
    OP_SETCUROBJECT_NEW         = 0x2F,
    OP_SETCUROBJECT_INTERNAL    = 0xFE,
    OP_SETCURFIELD              = 0x30,
    OP_SETCURFIELD_ARRAY        = 0x31,
    OP_LOADFIELD_UINT           = 0x32,
    OP_LOADFIELD_FLT            = 0x33,
    OP_LOADFIELD_STR            = 0x34,
    OP_SAVEFIELD_UINT            = 0x35,
    OP_SAVEFIELD_FLT             = 0x36,
    OP_SAVEFIELD_STR             = 0x37,
    OP_STR_TO_UINT              = 0x38,
    OP_STR_TO_FLT               = 0x39,
    OP_STR_TO_NONE              = 0x3A,
    OP_FLT_TO_UINT              = 0x3B,
    OP_FLT_TO_STR               = 0x3C,
    OP_FLT_TO_NONE              = 0x3D,
    OP_UINT_TO_FLT              = 0x3E,
    OP_UINT_TO_STR              = 0x3F,
    OP_UINT_TO_NONE             = 0x40,
    OP_LOADIMMED_UINT           = 0x41,
    OP_LOADIMMED_FLT            = 0x42,
    OP_TAG_TO_STR               = 0x43,
    OP_LOADIMMED_STR            = 0x44,
    OP_LOADIMMED_IDENT          = 0x45,
    OP_CALLFUNC_RESOLVE         = 0x46,
    OP_CALLFUNC                 = 0x47,
    OP_UNUSED3                  = 0x48,
    OP_ADVANCE_STR              = 0x49,
    OP_ADVANCE_STR_APPENDCHAR   = 0x4A,
    OP_ADVANCE_STR_COMMA        = 0x4B,
    OP_ADVANCE_STR_NUL          = 0x4C,
    OP_REWIND_STR               = 0x4D,
    OP_TERMINATE_REWIND_STR     = 0x4E,
    OP_COMPARE_STR              = 0x4F,
    OP_PUSH                     = 0x50,
    OP_PUSH_FRAME               = 0x51,
    OP_BREAK                    = 0x52,
    OP_UNIT_CONVERSION          = 0xFD,
    OP_EXTENDED                 = 0xFF
};

struct DSOConstant {
    enum Type { UInt, Float, String };
    Type type;
    union { uint32_t u; double f; };
    std::string str;
};

struct DSOFunction {
    std::string name;
    std::string ns;       // namespace
    std::string package;
    std::string filename;
    uint32_t startIp;
    uint32_t endIp;
    uint32_t argc;
    bool hasVarArgs;
    std::vector<std::string> argNames;
};

struct DSOFile {
    uint32_t version{};
    std::string filename;

    // String tables
    std::vector<char> globalStrings;
    std::vector<char> functionStrings;

    // Float tables
    std::vector<double> globalFloats;
    std::vector<double> functionFloats;

    // Code
    std::vector<uint8_t> code;
    uint32_t codeSize{};

    // Line break pairs
    std::vector<std::pair<uint32_t, uint32_t>> lineBreaks;

    // Identifier table: maps IP -> string index
    std::unordered_map<uint32_t, uint32_t> identTable;

    // Functions parsed from OP_FUNC_DECL
    // Function addresses are stored in funcMap and by the script dispatcher.
    // deque keeps them stable as later declarations are appended.
    std::deque<DSOFunction> functions;
    std::unordered_map<std::string, DSOFunction*> funcMap;

    // Resolved opcodes (stored as 32-bit values, < 0x100 = byte opcode, >= 0x100 = extended)
    std::vector<uint32_t> opcodes;
    std::vector<uint32_t> ipToSlot;  // raw IP -> opcode slot index
};

class DSOReader {
public:
    DSOReader();

    bool read(const uint8_t* data, size_t size, DSOFile& out);
    bool readFromFile(const char* path, DSOFile& out);

    // Access helpers
    const char* globalString(DSOFile& file, uint32_t index);
    const char* functionString(DSOFile& file, uint32_t index);
    void dumpInfo(DSOFile& file);

private:
    bool readStringTable(const uint8_t*& ptr, size_t& remaining, std::vector<char>& out);
    bool readFloatTable(const uint8_t*& ptr, size_t& remaining, std::vector<double>& out);
    bool readCodeStream(const uint8_t*& ptr, size_t& remaining, uint32_t& outCodeSize, uint32_t& outLineBreakCount, DSOFile& out);
    bool readIdentTable(const uint8_t*& ptr, size_t& remaining, DSOFile& out);

    uint32_t readU32(const uint8_t*& ptr, size_t& remaining);
    double readF64(const uint8_t*& ptr, size_t& remaining);
    std::string readString(const uint8_t*& ptr, size_t& remaining, uint32_t maxLen = 0);
    uint32_t readOpcodeSlot(const uint8_t*& ptr, size_t& remaining);
};
