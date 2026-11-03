/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"

namespace JSC {
namespace Python {

// The module _opcode, which Lib/opcode.py and Lib/dis.py are built on. (Modules/_opcode.c in CPython is the reference.)
//
// It describes the instructions of CPython's interpreter, those Lib/_opcode_metadata.py numbers. Code here is made of none of
// them (co_code is empty), so this serves only what asks about the instruction set itself, and lets `dis` and `inspect` load.

namespace OpcodeModule {

enum OpcodeFlag : uint16_t {
    Arg = 1 << 0,
    Const = 1 << 1,
    Name = 1 << 2,
    Jump = 1 << 3,
    Free = 1 << 4,
    Local = 1 << 5,
    Exc = 1 << 6, // Pushes an exception handler.
    Effect = 1 << 7, // stack_effect() knows of it: it is not a specialized form of another.
    UnpackEx = 1 << 8, // It pushes 1 + (oparg & 0xFF) + (oparg >> 8).
};

// What it pops and what it pushes are each constant + perArg * oparg + lowBit * (oparg & 1).
struct OpcodeInfo {
    uint16_t opcode;
    uint16_t flags;
    int8_t poppedConstant;
    int8_t poppedPerArg;
    int8_t poppedLowBit;
    int8_t pushedConstant;
    int8_t pushedPerArg;
    int8_t pushedLowBit;
};

// In order of opcode. Made by pytests/mod_opcode/tools/make_table.py from CPython 3.15.
constexpr OpcodeInfo opcodeTable[] = {
    { 0, Effect, 0, 0, 0, 0, 0, 0 }, { 1, Effect, 3, 0, 0, 1, 0, 0 }, { 2, Effect, 2, 0, 0, 1, 0, 0 }, { 3, Local, 2, 0, 0, 1, 0, 0 },
    { 4, Effect, 4, 0, 0, 1, 0, 0 }, { 5, Effect, 2, 0, 0, 2, 0, 0 }, { 6, Effect, 2, 0, 0, 2, 0, 0 }, { 7, Effect, 4, 0, 0, 3, 0, 0 },
    { 8, Effect, 2, 0, 0, 0, 0, 0 }, { 9, Effect, 1, 0, 0, 0, 0, 0 }, { 10, Effect, 3, 0, 0, 1, 0, 0 }, { 11, Effect, 1, 0, 0, 0, 0, 0 },
    { 12, Effect, 1, 0, 0, 1, 0, 0 }, { 13, Effect, 2, 0, 0, 1, 0, 0 }, { 14, Effect, 1, 0, 0, 1, 0, 0 }, { 15, Effect, 1, 0, 0, 2, 0, 0 },
    { 16, Effect, 1, 0, 0, 2, 0, 0 }, { 17, Effect, 0, 0, 0, 0, 0, 0 }, { 18, Effect, 1, 0, 0, 0, 0, 0 }, { 19, Effect, 0, 0, 0, 1, 0, 0 },
    { 20, Effect, 0, 0, 0, 1, 0, 0 }, { 21, Effect, 1, 0, 0, 1, 0, 0 }, { 22, Effect, 2, 0, 0, 3, 0, 0 }, { 23, Effect, 1, 0, 0, 2, 0, 0 },
    { 24, Effect, 1, 0, 0, 2, 0, 0 }, { 25, Effect, 0, 0, 0, 0, 0, 0 }, { 26, Effect, 0, 0, 0, 0, 0, 0 }, { 27, Effect, 1, 0, 0, 0, 0, 0 },
    { 28, Effect, 2, 0, 0, 0, 0, 0 }, { 29, Effect, 1, 0, 0, 0, 0, 0 }, { 30, Effect, 1, 0, 0, 2, 0, 0 }, { 31, Effect, 0, 0, 0, 1, 0, 0 },
    { 32, Effect, 0, 0, 0, 1, 0, 0 }, { 33, Effect, 1, 0, 0, 1, 0, 0 }, { 34, Effect, 0, 0, 0, 0, 0, 0 }, { 35, Effect, 4, 0, 0, 0, 0, 0 },
    { 36, Effect, 3, 0, 0, 0, 0, 0 }, { 37, Effect, 1, 0, 0, 1, 0, 0 }, { 38, Effect, 1, 0, 0, 1, 0, 0 }, { 39, Effect, 1, 0, 0, 1, 0, 0 },
    { 40, Effect, 1, 0, 0, 1, 0, 0 }, { 41, Effect, 5, 0, 0, 6, 0, 0 }, { 42, Arg | Effect, 2, 0, 0, 1, 0, 0 },
    { 43, Arg | Effect, 2, 0, 1, 1, 0, 0 }, { 44, Arg | Effect, 0, 1, 0, 1, 0, 0 }, { 45, Arg | Effect, 0, 2, 0, 1, 0, 0 },
    { 46, Arg | Effect, 0, 1, 0, 1, 0, 0 }, { 47, Arg | Effect, 0, 1, 0, 1, 0, 0 }, { 48, Arg | Effect, 0, 1, 0, 1, 0, 0 },
    { 49, Arg | Effect, 0, 1, 0, 1, 0, 0 }, { 50, Arg | Effect, 2, 1, 0, 1, 0, 0 }, { 51, Arg | Effect, 1, 0, 0, 1, 0, 0 },
    { 52, Arg | Effect, 2, 0, 0, 1, 0, 0 }, { 53, Arg | Effect, 3, 1, 0, 1, 0, 0 }, { 54, Arg | Effect, 2, 0, 0, 1, 0, 0 },
    { 55, Arg | Effect, 2, 0, 0, 1, 0, 0 }, { 56, Arg | Effect, 1, 0, 0, 1, 0, 0 }, { 57, Arg | Effect, 0, 1, 0, 1, 1, 0 },
    { 58, Arg | Effect, 0, 0, 0, 0, 0, 0 }, { 59, Arg | Name | Effect, 1, 0, 0, 0, 0, 0 }, { 60, Arg | Free | Effect, 0, 0, 0, 0, 0, 0 },
    { 61, Arg | Local | Effect, 0, 0, 0, 0, 0, 0 }, { 62, Arg | Name | Effect, 0, 0, 0, 0, 0, 0 }, { 63, Arg | Name | Effect, 0, 0, 0, 0, 0, 0 },
    { 64, Arg | Effect, 4, 1, 0, 3, 1, 0 }, { 65, Arg | Effect, 1, 1, 0, 0, 1, 0 }, { 66, Arg | Jump | Effect, 2, 0, 0, 0, 0, 0 },
    { 67, Arg | Effect, 0, 0, 0, 0, 0, 0 }, { 68, Arg | Jump | Effect, 2, 0, 0, 3, 0, 0 }, { 69, Arg | Effect, 1, 0, 0, 1, 0, 0 },
    { 70, Arg | Effect, 1, 0, 0, 2, 0, 0 }, { 71, Arg | Name | Effect, 1, 0, 0, 2, 0, 0 }, { 72, Arg | Name | Effect, 2, 0, 0, 1, 0, 0 },
    { 73, Arg | Effect, 2, 0, 0, 1, 0, 0 }, { 74, Arg | Jump | Effect, 0, 0, 0, 0, 0, 0 }, { 75, Arg | Jump | Effect, 0, 0, 0, 0, 0, 0 },
    { 76, Arg | Jump | Effect, 0, 0, 0, 0, 0, 0 }, { 77, Arg | Effect, 1, 1, 0, 0, 1, 0 }, { 78, Arg | Effect, 1, 1, 0, 0, 1, 0 },
    { 79, Arg | Name | Effect, 1, 0, 0, 1, 0, 1 }, { 80, Arg | Effect, 0, 0, 0, 1, 0, 0 }, { 81, Arg | Const | Effect, 0, 0, 0, 1, 0, 0 },
    { 82, Arg | Local | Effect, 0, 0, 0, 1, 0, 0 }, { 83, Arg | Local | Effect, 0, 0, 0, 1, 0, 0 }, { 84, Arg | Local | Effect, 0, 0, 0, 1, 0, 0 },
    { 85, Arg | Local | Effect, 0, 0, 0, 1, 0, 0 }, { 86, Arg | Local | Effect, 0, 0, 0, 2, 0, 0 }, { 87, Arg | Local | Effect, 0, 0, 0, 1, 0, 0 },
    { 88, Arg | Local | Effect, 0, 0, 0, 2, 0, 0 }, { 89, Arg | Free | Effect, 1, 0, 0, 1, 0, 0 }, { 90, Arg | Name | Effect, 1, 0, 0, 1, 0, 0 },
    { 91, Arg | Name | Effect, 0, 0, 0, 1, 0, 1 }, { 92, Arg | Name | Effect, 0, 0, 0, 1, 0, 0 }, { 93, Arg | Effect, 0, 0, 0, 1, 0, 0 },
    { 94, Arg | Effect, 1, 0, 0, 2, 0, 0 }, { 95, Arg | Name | Effect, 3, 0, 0, 1, 0, 1 }, { 96, Arg | Free | Effect, 0, 0, 0, 0, 0, 0 },
    { 97, Arg | Effect, 2, 1, 0, 0, 1, 0 }, { 98, Arg | Effect, 3, 0, 0, 1, 0, 0 }, { 99, Arg | Jump | Effect, 1, 0, 0, 0, 0, 0 },
    { 100, Arg | Jump | Effect, 1, 0, 0, 0, 0, 0 }, { 101, Arg | Jump | Effect, 1, 0, 0, 0, 0, 0 }, { 102, Arg | Jump | Effect, 1, 0, 0, 0, 0, 0 },
    { 103, Arg | Effect, 0, 1, 0, 0, 0, 0 }, { 104, Arg | Effect, 1, 1, 0, 0, 1, 0 }, { 105, Arg | Jump | Effect, 3, 0, 0, 3, 0, 0 },
    { 106, Arg | Effect, 1, 1, 0, 0, 1, 0 }, { 107, Arg | Effect, 2, 0, 0, 1, 0, 0 }, { 108, Arg | Effect, 1, 1, 0, 0, 1, 0 },
    { 109, Arg | Name | Effect, 2, 0, 0, 0, 0, 0 }, { 110, Arg | Free | Effect, 1, 0, 0, 0, 0, 0 }, { 111, Arg | Local | Effect, 1, 0, 0, 0, 0, 0 },
    { 112, Arg | Local | Effect, 1, 0, 0, 1, 0, 0 }, { 113, Arg | Local | Effect, 2, 0, 0, 0, 0, 0 }, { 114, Arg | Name | Effect, 1, 0, 0, 0, 0, 0 },
    { 115, Arg | Name | Effect, 1, 0, 0, 0, 0, 0 }, { 116, Arg | Effect, 0, 1, 0, 0, 1, 0 }, { 117, Arg | Effect | UnpackEx, 1, 0, 0, 0, 0, 0 },
    { 118, Arg | Effect, 1, 0, 0, 0, 1, 0 }, { 119, Arg | Effect, 1, 0, 0, 1, 0, 0 }, { 128, Arg | Effect, 0, 0, 0, 0, 0, 0 },
    { 129, 0, 2, 0, 0, 1, 0, 0 }, { 130, 0, 2, 0, 0, 1, 0, 0 }, { 131, 0, 2, 0, 0, 1, 0, 0 }, { 132, 0, 2, 0, 0, 1, 0, 0 },
    { 133, 0, 2, 0, 0, 1, 0, 0 }, { 134, 0, 2, 0, 0, 1, 0, 0 }, { 135, 0, 2, 0, 0, 1, 0, 0 }, { 136, 0, 2, 0, 0, 0, 0, 0 },
    { 137, 0, 2, 0, 0, 1, 0, 0 }, { 138, 0, 2, 0, 0, 1, 0, 0 }, { 139, 0, 2, 0, 0, 1, 0, 0 }, { 140, 0, 2, 0, 0, 1, 0, 0 },
    { 141, 0, 2, 0, 0, 1, 0, 0 }, { 142, 0, 2, 0, 0, 1, 0, 0 }, { 143, 0, 2, 0, 0, 1, 0, 0 }, { 144, Arg, 2, 1, 0, 0, 0, 0 },
    { 145, Arg, 2, 1, 0, 0, 0, 0 }, { 146, Arg, 2, 1, 0, 0, 0, 0 }, { 147, Arg, 2, 1, 0, 1, 0, 0 }, { 148, Arg, 2, 1, 0, 1, 0, 0 },
    { 149, Arg, 2, 1, 0, 1, 0, 0 }, { 150, Arg, 2, 1, 0, 1, 0, 0 }, { 151, 0, 4, 0, 0, 1, 0, 0 }, { 152, Arg, 4, 0, 0, 0, 0, 0 },
    { 153, 0, 4, 0, 0, 1, 0, 0 }, { 154, Arg, 3, 1, 0, 0, 0, 0 }, { 155, Arg, 3, 1, 0, 1, 0, 0 }, { 156, Arg, 3, 1, 0, 0, 0, 0 },
    { 157, 0, 3, 0, 0, 1, 0, 0 }, { 158, Arg, 3, 0, 0, 1, 0, 0 }, { 159, Arg, 2, 1, 0, 1, 0, 0 }, { 160, Arg, 2, 1, 0, 1, 0, 0 },
    { 161, Arg, 2, 1, 0, 1, 0, 0 }, { 162, Arg, 2, 1, 0, 1, 0, 0 }, { 163, Arg, 2, 1, 0, 1, 0, 0 }, { 164, Arg, 2, 1, 0, 0, 0, 0 },
    { 165, Arg, 2, 1, 0, 0, 0, 0 }, { 166, Arg, 3, 0, 0, 1, 0, 0 }, { 167, Arg, 3, 0, 0, 1, 0, 0 }, { 168, Arg, 3, 0, 0, 1, 0, 0 },
    { 169, Arg, 2, 0, 0, 1, 0, 0 }, { 170, Arg, 2, 0, 0, 1, 0, 0 }, { 171, Arg, 2, 0, 0, 1, 0, 0 }, { 172, Arg, 2, 0, 0, 1, 0, 0 },
    { 173, Arg, 2, 0, 0, 1, 0, 0 }, { 174, Arg, 2, 0, 0, 2, 0, 0 }, { 175, Arg | Jump, 2, 0, 0, 3, 0, 0 }, { 176, Arg | Jump, 2, 0, 0, 3, 0, 0 },
    { 177, Arg | Jump, 2, 0, 0, 3, 0, 0 }, { 178, Arg | Jump, 2, 0, 0, 3, 0, 0 }, { 179, 0, 1, 0, 0, 2, 0, 0 }, { 180, 0, 1, 0, 0, 2, 0, 0 },
    { 181, Arg | Jump, 0, 0, 0, 0, 0, 0 }, { 182, Arg | Jump, 0, 0, 0, 0, 0, 0 }, { 183, Arg, 1, 0, 0, 1, 0, 1 }, { 184, Arg, 1, 0, 0, 1, 0, 1 },
    { 185, Arg | Name, 1, 0, 0, 0, 0, 0 }, { 186, Arg, 1, 0, 0, 1, 0, 1 }, { 187, Arg, 1, 0, 0, 2, 0, 0 }, { 188, Arg, 1, 0, 0, 2, 0, 0 },
    { 189, Arg, 1, 0, 0, 2, 0, 0 }, { 190, Arg, 1, 0, 0, 1, 0, 1 }, { 191, Arg, 1, 0, 0, 1, 0, 0 }, { 192, Arg, 1, 0, 0, 1, 0, 0 },
    { 193, Arg, 1, 0, 0, 0, 0, 0 }, { 194, Arg, 1, 0, 0, 1, 0, 1 }, { 195, Arg | Name, 1, 0, 0, 1, 0, 1 }, { 196, Arg, 0, 0, 0, 1, 0, 1 },
    { 197, Arg, 0, 0, 0, 1, 0, 1 }, { 198, Arg | Name, 3, 0, 0, 1, 0, 0 }, { 199, Arg | Name, 3, 0, 0, 2, 0, 0 }, { 200, 0, 0, 0, 0, 0, 0, 0 },
    { 201, Arg, 0, 0, 0, 0, 0, 0 }, { 202, Arg | Jump, 3, 0, 0, 3, 0, 0 }, { 203, Arg, 3, 0, 0, 2, 0, 0 }, { 204, Arg | Jump, 3, 0, 0, 3, 0, 0 },
    { 205, 0, 2, 0, 0, 0, 0, 0 }, { 206, 0, 2, 0, 0, 0, 0, 0 }, { 207, Arg | Name, 2, 0, 0, 0, 0, 0 }, { 208, 0, 3, 0, 0, 0, 0, 0 },
    { 209, 0, 3, 0, 0, 0, 0, 0 }, { 210, 0, 1, 0, 0, 1, 0, 0 }, { 211, 0, 1, 0, 0, 1, 0, 0 }, { 212, 0, 1, 0, 0, 1, 0, 0 },
    { 213, 0, 1, 0, 0, 1, 0, 0 }, { 214, 0, 1, 0, 0, 1, 0, 0 }, { 215, 0, 1, 0, 0, 1, 0, 0 }, { 216, Arg, 1, 0, 0, 0, 1, 0 },
    { 217, Arg, 1, 0, 0, 0, 1, 0 }, { 218, Arg, 1, 0, 0, 2, 0, 0 }, { 233, Effect, 3, 0, 0, 2, 0, 0 }, { 234, Effect, 2, 0, 0, 0, 0, 0 },
    { 235, Effect, 3, 0, 0, 1, 0, 0 }, { 236, Arg | Jump | Effect, 2, 0, 0, 3, 0, 0 }, { 237, Effect, 0, 0, 0, 0, 0, 0 },
    { 238, Arg | Effect, 0, 0, 0, 0, 0, 0 }, { 239, Effect, 0, 0, 0, 0, 0, 0 }, { 240, Arg | Effect, 1, 0, 0, 0, 0, 0 },
    { 241, Arg | Effect, 1, 0, 0, 0, 0, 0 }, { 242, Arg | Effect, 1, 0, 0, 0, 0, 0 }, { 243, Arg | Effect, 1, 0, 0, 0, 0, 0 },
    { 244, Arg | Effect, 0, 0, 0, 0, 0, 0 }, { 245, Effect, 1, 0, 0, 1, 0, 0 }, { 246, Arg | Effect, 1, 0, 0, 1, 0, 0 },
    { 247, Arg | Jump | Effect, 2, 0, 0, 0, 0, 0 }, { 248, Arg | Name | Effect, 3, 0, 0, 1, 0, 1 }, { 249, Arg | Effect, 2, 1, 0, 1, 0, 0 },
    { 250, Arg | Effect, 3, 1, 0, 1, 0, 0 }, { 251, Effect, 4, 0, 0, 1, 0, 0 }, { 252, Arg | Effect, 0, 0, 0, 0, 0, 0 },
    { 253, Effect, 0, 0, 0, 0, 0, 0 }, { 254, Arg | Effect, 0, 0, 0, 0, 0, 0 }, { 255, Arg | Effect, 0, 0, 0, 0, 0, 0 },
    { 256, Effect, 0, 0, 0, 0, 0, 0 }, { 257, Arg | Jump | Effect, 0, 0, 0, 0, 0, 0 }, { 258, Arg | Jump | Effect, 1, 0, 0, 1, 0, 0 },
    { 259, Arg | Jump | Effect, 1, 0, 0, 1, 0, 0 }, { 260, Arg | Jump | Effect, 0, 0, 0, 0, 0, 0 }, { 261, Arg | Local | Effect, 0, 0, 0, 1, 0, 0 },
    { 262, Effect, 0, 0, 0, 0, 0, 0 }, { 263, Arg | Exc | Effect, 0, 0, 0, 2, 0, 0 }, { 264, Arg | Exc | Effect, 0, 0, 0, 1, 0, 0 },
    { 265, Arg | Exc | Effect, 0, 0, 0, 1, 0, 0 }, { 266, Arg | Local | Effect, 1, 0, 0, 0, 0, 0 },
};

static const OpcodeInfo* opcodeFind(int opcode)
{
    auto* end = std::end(opcodeTable);
    auto* found = std::lower_bound(std::begin(opcodeTable), end, opcode, [](const OpcodeInfo& info, int value) { return info.opcode < value; });
    return found != end && found->opcode == opcode ? found : nullptr;
}

template<uint16_t flag>
static JSValue opcodeHasFlag(JSGlobalObject* globalObject, JSValue opcodeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int opcode = toCInt(globalObject, opcodeValue);
    RETURN_IF_EXCEPTION(scope, { });
    auto* info = opcodeFind(opcode);
    return jsBoolean(info && (!flag || (info->flags & flag)));
}

static JSValue opcodeStackEffect(JSGlobalObject* globalObject, JSValue opcodeValue, JSValue opargValue, JSValue jump)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int opcode = toCInt(globalObject, opcodeValue);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t oparg = 0;
    if (opargValue && !isNone(opargValue)) {
        oparg = static_cast<int>(toInt64(globalObject, opargValue));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (jump && !isNone(jump) && !jump.isBoolean())
        return raiseValueError(globalObject, scope, "stack_effect: jump must be False, True or None"_s);

    auto* info = opcodeFind(opcode);
    if (!info || !(info->flags & Effect))
        return raiseValueError(globalObject, scope, "invalid opcode or oparg"_s);
    int64_t popped = info->poppedConstant + info->poppedPerArg * oparg + info->poppedLowBit * (oparg & 1);
    int64_t pushed = info->flags & UnpackEx ? 1 + (oparg & 0xFF) + (oparg >> 8) : info->pushedConstant + info->pushedPerArg * oparg + info->pushedLowBit * (oparg & 1);
    if (popped < 0 || pushed < 0)
        return raiseValueError(globalObject, scope, "invalid opcode or oparg"_s);
    if ((info->flags & Exc) && jump && jump.isFalse())
        return jsNumber(0);
    return makeInt(globalObject, pushed - popped);
}

static JSValue opcodeGetSpecializationStats(JSGlobalObject*)
{
    return jsUndefined();
}

static JSValue opcodeListOf(JSGlobalObject* globalObject, std::span<const ASCIILiteral> names)
{
    VM& vm = getVM(globalObject);
    JSArray* list = newList(globalObject, names.size());
    if (!list)
        return { };
    for (ASCIILiteral name : names)
        listAppend(globalObject, list, makeStr(vm, name));
    return list;
}

static JSValue opcodeGetNbOps(JSGlobalObject* globalObject)
{
    static constexpr std::pair<ASCIILiteral, ASCIILiteral> operations[] = {
        { "NB_ADD"_s, "+"_s }, { "NB_AND"_s, "&"_s }, { "NB_FLOOR_DIVIDE"_s, "//"_s }, { "NB_LSHIFT"_s, "<<"_s }, { "NB_MATRIX_MULTIPLY"_s, "@"_s },
        { "NB_MULTIPLY"_s, "*"_s }, { "NB_REMAINDER"_s, "%"_s }, { "NB_OR"_s, "|"_s }, { "NB_POWER"_s, "**"_s }, { "NB_RSHIFT"_s, ">>"_s },
        { "NB_SUBTRACT"_s, "-"_s }, { "NB_TRUE_DIVIDE"_s, "/"_s }, { "NB_XOR"_s, "^"_s }, { "NB_INPLACE_ADD"_s, "+="_s }, { "NB_INPLACE_AND"_s, "&="_s },
        { "NB_INPLACE_FLOOR_DIVIDE"_s, "//="_s }, { "NB_INPLACE_LSHIFT"_s, "<<="_s }, { "NB_INPLACE_MATRIX_MULTIPLY"_s, "@="_s }, { "NB_INPLACE_MULTIPLY"_s, "*="_s },
        { "NB_INPLACE_REMAINDER"_s, "%="_s }, { "NB_INPLACE_OR"_s, "|="_s }, { "NB_INPLACE_POWER"_s, "**="_s }, { "NB_INPLACE_RSHIFT"_s, ">>="_s },
        { "NB_INPLACE_SUBTRACT"_s, "-="_s }, { "NB_INPLACE_TRUE_DIVIDE"_s, "/="_s }, { "NB_INPLACE_XOR"_s, "^="_s }, { "NB_SUBSCR"_s, "[]"_s },
    };
    VM& vm = getVM(globalObject);
    JSArray* list = newList(globalObject, std::size(operations));
    if (!list)
        return { };
    for (auto& [name, symbol] : operations)
        listAppend(globalObject, list, PyTuple::create(globalObject, { makeStr(vm, name), makeStr(vm, symbol) }));
    return list;
}

static JSValue opcodeGetIntrinsic1Descs(JSGlobalObject* globalObject)
{
    static constexpr ASCIILiteral names[] = {
        "INTRINSIC_1_INVALID"_s, "INTRINSIC_PRINT"_s, "INTRINSIC_IMPORT_STAR"_s, "INTRINSIC_STOPITERATION_ERROR"_s, "INTRINSIC_ASYNC_GEN_WRAP"_s, "INTRINSIC_UNARY_POSITIVE"_s,
        "INTRINSIC_LIST_TO_TUPLE"_s, "INTRINSIC_TYPEVAR"_s, "INTRINSIC_PARAMSPEC"_s, "INTRINSIC_TYPEVARTUPLE"_s, "INTRINSIC_SUBSCRIPT_GENERIC"_s, "INTRINSIC_TYPEALIAS"_s,
    };
    return opcodeListOf(globalObject, names);
}

static JSValue opcodeGetIntrinsic2Descs(JSGlobalObject* globalObject)
{
    static constexpr ASCIILiteral names[] = {
        "INTRINSIC_2_INVALID"_s, "INTRINSIC_PREP_RERAISE_STAR"_s, "INTRINSIC_TYPEVAR_WITH_BOUND"_s, "INTRINSIC_TYPEVAR_WITH_CONSTRAINTS"_s,
        "INTRINSIC_SET_FUNCTION_TYPE_PARAMS"_s, "INTRINSIC_SET_TYPEPARAM_DEFAULT"_s,
    };
    return opcodeListOf(globalObject, names);
}

static JSValue opcodeGetSpecialMethodNames(JSGlobalObject* globalObject)
{
    static constexpr ASCIILiteral names[] = { "__enter__"_s, "__exit__"_s, "__aenter__"_s, "__aexit__"_s };
    return opcodeListOf(globalObject, names);
}

static JSValue opcodeGetExecutor(JSGlobalObject* globalObject, JSValue code, JSValue offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    toCInt(globalObject, offset);
    RETURN_IF_EXCEPTION(scope, { });
    if (!PyCode::is(code))
        return raiseTypeError(globalObject, scope, "expected a code object, not '"_s, typeName(globalObject, code), '\'');
    return raiseRuntimeError(globalObject, scope, "Executors are not available in this build"_s);
}

static constexpr NativeDef opcodeFunctions[] = {
    PY_FUNCTION("stack_effect", opcodeStackEffect, "(opcode, oparg=None, /, *, jump=None)"),
    PY_FUNCTION("is_valid", opcodeHasFlag<0>, "(opcode)"),
    PY_FUNCTION("has_arg", opcodeHasFlag<Arg>, "(opcode)"),
    PY_FUNCTION("has_const", opcodeHasFlag<Const>, "(opcode)"),
    PY_FUNCTION("has_name", opcodeHasFlag<Name>, "(opcode)"),
    PY_FUNCTION("has_jump", opcodeHasFlag<Jump>, "(opcode)"),
    PY_FUNCTION("has_free", opcodeHasFlag<Free>, "(opcode)"),
    PY_FUNCTION("has_local", opcodeHasFlag<Local>, "(opcode)"),
    PY_FUNCTION("has_exc", opcodeHasFlag<Exc>, "(opcode)"),
    PY_FUNCTION("get_specialization_stats", opcodeGetSpecializationStats, "()"),
    PY_FUNCTION("get_nb_ops", opcodeGetNbOps, "()"),
    PY_FUNCTION("get_intrinsic1_descs", opcodeGetIntrinsic1Descs, "()"),
    PY_FUNCTION("get_intrinsic2_descs", opcodeGetIntrinsic2Descs, "()"),
    PY_FUNCTION("get_executor", opcodeGetExecutor, "(code, offset)"),
    PY_FUNCTION("get_special_method_names", opcodeGetSpecialMethodNames, "()"),
};

} // namespace OpcodeModule

void initializeOpcodeModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, OpcodeModule::opcodeFunctions);
    // Nothing here rewrites those instructions as it runs them.
    module->add(globalObject, "ENABLE_SPECIALIZATION"_s, jsNumber(0));
}

} // namespace Python
} // namespace JSC
