/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BIRModule.h"

#if USE(BUN_JSC_ADDITIONS)

#include <wtf/HashSet.h>
#include <wtf/LEBDecoder.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace FFI { namespace BIR {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Module);

namespace {

constexpr uint32_t parameterBlock = UINT32_MAX;
constexpr uint64_t maxCount = 1u << 24;

bool isInt(Type type) { return type == Type::I32 || type == Type::I64; }
bool isFloat(Type type) { return type == Type::F32 || type == Type::F64; }

class Decoder {
public:
    Decoder(std::span<const uint8_t> bytes)
        : m_bytes(bytes)
    {
    }

    std::expected<std::unique_ptr<Module>, String> run()
    {
        auto module = makeUnique<Module>();
        m_module = module.get();
        if (!decodeModule())
            return std::unexpected<String>(makeString("BIR: "_s, m_error, " (at byte "_s, m_offset, ')'));
        return module;
    }

private:
    bool fail(const String& message)
    {
        if (m_error.isNull())
            m_error = message;
        return false;
    }

    bool u8(uint8_t& result)
    {
        if (m_offset >= m_bytes.size())
            return fail("unexpected end of input"_s);
        result = m_bytes[m_offset++];
        return true;
    }

    bool varuint(uint64_t& result)
    {
        if (!WTF::LEBDecoder::decodeUInt<uint64_t>(m_bytes, m_offset, result))
            return fail("bad varuint"_s);
        return true;
    }

    bool varuint32(uint32_t& result)
    {
        uint64_t wide;
        if (!varuint(wide))
            return false;
        if (wide >= UINT32_MAX)
            return fail("index too large"_s);
        result = static_cast<uint32_t>(wide);
        return true;
    }

    bool count(uint32_t& result)
    {
        if (!varuint32(result))
            return false;
        if (result > maxCount)
            return fail("count too large"_s);
        return true;
    }

    bool varint(int64_t& result)
    {
        if (!WTF::LEBDecoder::decodeInt<int64_t>(m_bytes, m_offset, result))
            return fail("bad varint"_s);
        return true;
    }

    bool fixed(size_t size, uint64_t& result)
    {
        if (m_bytes.size() - m_offset < size)
            return fail("unexpected end of input"_s);
        result = 0;
        for (size_t i = 0; i < size; ++i)
            result |= static_cast<uint64_t>(m_bytes[m_offset + i]) << (8 * i);
        m_offset += size;
        return true;
    }

    bool bytes(size_t size, std::span<const uint8_t>& result)
    {
        if (m_bytes.size() - m_offset < size)
            return fail("unexpected end of input"_s);
        result = m_bytes.subspan(m_offset, size);
        m_offset += size;
        return true;
    }

    bool str(std::span<const uint8_t>& result)
    {
        uint32_t length;
        return count(length) && bytes(length, result);
    }

    bool type(Type& result, bool allowVoid)
    {
        uint8_t raw;
        if (!u8(raw))
            return false;
        if (raw > static_cast<uint8_t>(Type::V128) || (!allowVoid && !raw))
            return fail("bad type"_s);
        result = static_cast<Type>(raw);
        if (result == Type::V128)
            m_module->usesVectors = true;
        return true;
    }

    bool ffiType(FFI::Type& result)
    {
        uint8_t raw;
        if (!u8(raw))
            return false;
        if (raw >= numberOfTypes)
            return fail("bad FFI type"_s);
        result = static_cast<FFI::Type>(raw);
        return true;
    }

    bool decodeModule()
    {
        std::span<const uint8_t> header;
        if (!bytes(4, header))
            return false;
        if (memcmp(header.data(), magic, 4))
            return fail("bad magic"_s);
        uint8_t arch, os, pointerBytes, reserved;
        if (!u8(arch) || !u8(os) || !u8(pointerBytes) || !u8(reserved))
            return false;
        if (arch > static_cast<uint8_t>(Arch::ARM64) || os > static_cast<uint8_t>(OS::FreeBSD) || pointerBytes != 8)
            return fail("unsupported target"_s);
        m_module->arch = static_cast<Arch>(arch);
        m_module->os = static_cast<OS>(os);

        uint32_t signatureCount;
        if (!count(signatureCount))
            return false;
        for (uint32_t i = 0; i < signatureCount; ++i) {
            Signature signature;
            uint8_t flags;
            uint32_t resultCount, parameterCount;
            if (!count(resultCount))
                return false;
            if (resultCount > 4)
                return fail("too many results"_s);
            for (uint32_t j = 0; j < resultCount; ++j) {
                Type result;
                if (!type(result, false))
                    return false;
                signature.results.append(result);
            }
            if (!u8(flags) || !count(parameterCount))
                return false;
            signature.isVariadic = flags & 1;
            for (uint32_t j = 0; j < parameterCount; ++j) {
                Parameter parameter;
                uint8_t kind;
                if (!u8(kind))
                    return false;
                if (kind > static_cast<uint8_t>(ParamKind::IndirectResult))
                    return fail("bad parameter kind"_s);
                parameter.kind = static_cast<ParamKind>(kind);
                switch (parameter.kind) {
                case ParamKind::Value:
                    if (!type(parameter.type, false))
                        return false;
                    break;
                case ParamKind::ByValStack: {
                    uint8_t exhausts;
                    if (!varuint(parameter.size) || !varuint(parameter.alignment) || !u8(exhausts))
                        return false;
                    if (!parameter.size || parameter.size > (1u << 20) || (parameter.size & 7))
                        return fail("bad by-value argument size"_s);
                    if (parameter.alignment != 8 && parameter.alignment != 16)
                        return fail("bad by-value argument alignment"_s);
                    if (exhausts > static_cast<uint8_t>(Exhausts::FloatRegisters))
                        return fail("bad exhausts"_s);
                    parameter.exhausts = static_cast<Exhausts>(exhausts);
                    break;
                }
                case ParamKind::IndirectResult:
                    break;
                }
                signature.parameters.append(parameter);
            }
            m_module->signatures.append(WTF::move(signature));
        }

        uint32_t externCount;
        if (!count(externCount))
            return false;
        for (uint32_t i = 0; i < externCount; ++i) {
            Extern entry;
            std::span<const uint8_t> name;
            uint8_t kind = 0;
            if (!str(name) || !u8(kind) || !varuint32(entry.signature))
                return false;
            entry.isWeak = kind & weakExtern;
            kind &= ~weakExtern;
            if (kind > static_cast<uint8_t>(ExternKind::Data))
                return fail("bad extern kind"_s);
            entry.kind = static_cast<ExternKind>(kind);
            if (entry.kind == ExternKind::Function && entry.signature >= signatureCount)
                return fail("extern signature out of range"_s);
            entry.name = CString(name);
            m_module->externs.append(WTF::move(entry));
        }

        Data& data = m_module->data;
        uint32_t initializedSize;
        std::span<const uint8_t> initialized;
        if (!varuint(data.size) || !varuint(data.alignment) || !count(initializedSize) || !bytes(initializedSize, initialized))
            return false;
        if (data.size > (1ull << 32) || initializedSize > data.size)
            return fail("bad data segment size"_s);
        if (!data.alignment || (data.alignment & (data.alignment - 1)) || data.alignment > 4096)
            return fail("bad data segment alignment"_s);
        data.initialized.append(initialized);
        auto relocs = [&](Vector<Reloc>& list, uint64_t segmentSize, RelocKind lastKind) -> bool {
            uint32_t relocCount;
            if (!count(relocCount))
                return false;
            for (uint32_t i = 0; i < relocCount; ++i) {
                Reloc reloc;
                uint8_t kind;
                if (!varuint(reloc.offset) || !u8(kind) || !varuint(reloc.index) || !varint(reloc.addend))
                    return false;
                if (kind > static_cast<uint8_t>(lastKind))
                    return fail("bad reloc kind"_s);
                reloc.kind = static_cast<RelocKind>(kind);
                if (reloc.offset > segmentSize || segmentSize - reloc.offset < 8)
                    return fail("reloc offset out of range"_s);
                list.append(reloc);
            }
            return true;
        };
        if (!relocs(m_module->data.relocs, data.size, RelocKind::Extern))
            return false;

        {
            ThreadLocalData& tls = m_module->tls;
            uint32_t tlsInitializedSize;
            std::span<const uint8_t> tlsInitialized;
            if (!varuint(tls.size) || !varuint(tls.alignment) || !count(tlsInitializedSize) || !bytes(tlsInitializedSize, tlsInitialized))
                return false;
            if (tls.size > (1ull << 28) || tlsInitializedSize > tls.size)
                return fail("bad thread-local segment size"_s);
            if (!tls.alignment || (tls.alignment & (tls.alignment - 1)) || tls.alignment > 4096)
                return fail("bad thread-local segment alignment"_s);
            tls.initialized.append(tlsInitialized);
            if (!relocs(tls.relocs, tls.size, RelocKind::Tls))
                return false;
        }

        uint32_t functionCount;
        if (!count(functionCount))
            return false;
        for (uint32_t i = 0; i < functionCount; ++i) {
            Function function;
            std::span<const uint8_t> name;
            uint8_t flags;
            if (!str(name) || !varuint32(function.signature) || !u8(flags))
                return false;
            if (function.signature >= signatureCount)
                return fail("function signature out of range"_s);
            function.name = String::fromUTF8(name);
            function.isExported = flags & 1;
            function.callsReturnsTwice = flags & 2;
            function.isAlwaysInline = flags & 4;
            function.isNeverInline = flags & 8;
            function.hasInlineHint = flags & 16;
            m_module->functions.append(WTF::move(function));
        }

        for (Vector<Reloc>* list : { &m_module->data.relocs, &m_module->tls.relocs }) {
            for (auto& reloc : *list) {
                uint64_t limit = 0;
                switch (reloc.kind) {
                case RelocKind::Data:
                    limit = data.size + 1;
                    break;
                case RelocKind::Func:
                    limit = functionCount;
                    break;
                case RelocKind::Extern:
                    limit = externCount;
                    break;
                case RelocKind::Tls:
                    limit = m_module->tls.size + 1;
                    break;
                }
                if (reloc.index >= limit)
                    return fail("reloc index out of range"_s);
                if (reloc.kind == RelocKind::Func)
                    m_module->functions[reloc.index].isAddressTaken = true;
            }
        }

        for (uint32_t i = 0; i < functionCount; ++i) {
            if (!decodeBody(m_module->functions[i])) {
                m_error = makeString("in function '"_s, m_module->functions[i].name, "': "_s, m_error);
                return false;
            }
        }

        uint32_t exportCount;
        if (!count(exportCount))
            return false;
        for (uint32_t i = 0; i < exportCount; ++i) {
            Export entry;
            std::span<const uint8_t> name;
            uint32_t argumentCount;
            if (!str(name) || !varuint32(entry.function) || !ffiType(entry.returnType) || !count(argumentCount))
                return false;
            if (entry.function >= functionCount)
                return fail("export function out of range"_s);
            const Signature& signature = m_module->signatures[m_module->functions[entry.function].signature];
            if (!signature.isScalar())
                return fail("exported function does not have a scalar signature"_s);
            if (argumentCount != signature.parameters.size())
                return fail("export argument count does not match the function"_s);
            entry.name = String::fromUTF8(name);
            for (uint32_t j = 0; j < argumentCount; ++j) {
                FFI::Type argument;
                if (!ffiType(argument))
                    return false;
                entry.arguments.append(argument);
            }
            m_module->exports.append(WTF::move(entry));
        }

        uint32_t libraryCount;
        if (!count(libraryCount))
            return false;
        for (uint32_t i = 0; i < libraryCount; ++i) {
            std::span<const uint8_t> name;
            if (!str(name))
                return false;
            m_module->libraries.append(CString(name));
        }

        for (Vector<uint32_t>* list : { &m_module->constructors, &m_module->destructors }) {
            uint32_t listLength;
            if (!count(listLength))
                return false;
            for (uint32_t i = 0; i < listLength; ++i) {
                uint32_t function;
                if (!varuint32(function))
                    return false;
                if (function >= functionCount)
                    return fail("constructor function out of range"_s);
                const Signature& signature = m_module->signatures[m_module->functions[function].signature];
                if (!signature.results.isEmpty() || !signature.parameters.isEmpty() || signature.isVariadic)
                    return fail("a constructor must be void f(void)"_s);
                m_module->functions[function].isAddressTaken = true; // Called by the loader, not by a Call.
                list->append(function);
            }
        }

        if (m_offset != m_bytes.size())
            return fail("trailing bytes"_s);
        return true;
    }

    // Per-function state.
    Function* m_function { nullptr };
    Vector<Type> m_valueTypes;
    Vector<uint32_t> m_valueBlocks;
    uint32_t m_currentBlock { 0 };

    bool use(uint32_t& id, Type& valueType)
    {
        if (!varuint32(id))
            return false;
        if (id >= m_valueTypes.size())
            return fail("use of an undefined value"_s);
        if (m_valueBlocks[id] != parameterBlock && m_valueBlocks[id] != m_currentBlock)
            return fail("value used outside its defining block"_s);
        valueType = m_valueTypes[id];
        return true;
    }

    bool useTyped(uint32_t& id, Type expected)
    {
        Type actual;
        if (!use(id, actual))
            return false;
        if (actual != expected)
            return fail("operand has the wrong type"_s);
        return true;
    }

    void define(Inst& inst, Type resultType)
    {
        if (!inst.resultCount++) {
            inst.resultType = resultType;
            inst.result = static_cast<uint32_t>(m_valueTypes.size());
        }
        m_valueTypes.append(resultType);
        m_valueBlocks.append(m_currentBlock);
    }

    bool blockIndex(uint32_t& result)
    {
        if (!varuint32(result))
            return false;
        if (result >= m_function->blocks.size())
            return fail("block out of range"_s);
        return true;
    }

    bool callArguments(Inst& inst, const Signature& signature, bool allowVariadic)
    {
        uint32_t argumentCount;
        if (!count(argumentCount))
            return false;
        size_t fixedCount = signature.parameters.size();
        bool isVariadic = allowVariadic && signature.isVariadic;
        if (argumentCount < fixedCount || (!isVariadic && argumentCount != fixedCount))
            return fail("call has the wrong number of arguments"_s);
        inst.extraOffset = static_cast<uint32_t>(m_function->extra.size());
        inst.extraCount = argumentCount;
        for (uint32_t i = 0; i < argumentCount; ++i) {
            uint32_t id;
            Type actual;
            if (!use(id, actual))
                return false;
            if (i < fixedCount && actual != signature.parameters[i].type)
                return fail("call argument has the wrong type"_s);
            m_function->extra.append(id);
        }
        for (Type result : signature.results)
            define(inst, result);
        return true;
    }

    bool decodeBody(Function& function)
    {
        m_function = &function;
        const Signature& signature = m_module->signatures[function.signature];
        m_valueTypes.shrink(0);
        m_valueBlocks.shrink(0);
        for (const Parameter& parameter : signature.parameters) {
            m_valueTypes.append(parameter.type);
            m_valueBlocks.append(parameterBlock);
        }

        uint32_t localCount;
        if (!count(localCount))
            return false;
        for (uint32_t i = 0; i < localCount; ++i) {
            Type local;
            if (!type(local, false))
                return false;
            function.locals.append(local);
        }

        uint32_t slotCount;
        if (!count(slotCount))
            return false;
        for (uint32_t i = 0; i < slotCount; ++i) {
            Slot slot;
            if (!varuint(slot.size) || !varuint(slot.alignment))
                return false;
            if (slot.size > (1u << 28) || !slot.alignment || (slot.alignment & (slot.alignment - 1)) || slot.alignment > 4096)
                return fail("bad stack slot"_s);
            function.slots.append(slot);
        }

        uint32_t blockCount;
        if (!count(blockCount))
            return false;
        if (!blockCount)
            return fail("function has no blocks"_s);
        function.blocks.grow(blockCount);

        for (uint32_t blockNumber = 0; blockNumber < blockCount; ++blockNumber) {
            m_currentBlock = blockNumber;
            uint32_t instCount;
            if (!count(instCount))
                return false;
            if (!instCount)
                return fail("empty block"_s);
            function.blocks[blockNumber].firstInst = static_cast<uint32_t>(function.insts.size());
            function.blocks[blockNumber].instCount = instCount;
            for (uint32_t i = 0; i < instCount; ++i) {
                Inst inst;
                if (!decodeInst(inst, signature))
                    return false;
                if (isTerminator(inst.op) != (i + 1 == instCount))
                    return fail("block must end in exactly one terminator"_s);
                function.insts.append(inst);
            }
        }
        function.valueCount = static_cast<uint32_t>(m_valueTypes.size());
        return true;
    }

    bool decodeInst(Inst& inst, const Signature& functionSignature)
    {
        uint8_t rawOp;
        if (!u8(rawOp))
            return false;
        inst.op = static_cast<Op>(rawOp);
        Type a, b;
        switch (inst.op) {
        case Op::ConstI32:
            if (!varint(inst.imm))
                return false;
            if (inst.imm < INT32_MIN || inst.imm > INT32_MAX)
                return fail("ConstI32 out of range"_s);
            define(inst, Type::I32);
            return true;
        case Op::ConstI64:
            if (!varint(inst.imm))
                return false;
            define(inst, Type::I64);
            return true;
        case Op::ConstF32:
        case Op::ConstF64: {
            uint64_t bits;
            if (!fixed(inst.op == Op::ConstF32 ? 4 : 8, bits))
                return false;
            inst.imm = static_cast<int64_t>(bits);
            define(inst, inst.op == Op::ConstF32 ? Type::F32 : Type::F64);
            return true;
        }
        case Op::ConstV128:
            if (!sixteenBytes(inst))
                return false;
            define(inst, Type::V128);
            return true;
        case Op::VSplat:
            if (!lane(inst) || !useTyped(inst.a, laneScalarType(static_cast<Lane>(inst.aux))))
                return false;
            define(inst, Type::V128);
            return true;
        case Op::VExtract: {
            uint8_t index;
            if (!lane(inst) || !signedness(inst) || !u8(index) || !useTyped(inst.a, Type::V128))
                return false;
            if (index >= laneCount(static_cast<Lane>(inst.aux)))
                return fail("lane index out of range"_s);
            inst.imm |= static_cast<int64_t>(index) << 8;
            define(inst, laneScalarType(static_cast<Lane>(inst.aux)));
            return true;
        }
        case Op::VReplace: {
            uint8_t index;
            if (!lane(inst) || !u8(index) || !useTyped(inst.a, Type::V128) || !useTyped(inst.b, laneScalarType(static_cast<Lane>(inst.aux))))
                return false;
            if (index >= laneCount(static_cast<Lane>(inst.aux)))
                return fail("lane index out of range"_s);
            inst.imm |= static_cast<int64_t>(index) << 8;
            define(inst, Type::V128);
            return true;
        }
        case Op::VAdd:
        case Op::VSub:
        case Op::VMul:
            if (!lane(inst) || !useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128))
                return false;
            define(inst, Type::V128);
            return true;
        case Op::VDiv:
        case Op::VRem:
        case Op::VMin:
        case Op::VMax:
        case Op::VEq:
        case Op::VNe:
        case Op::VLt:
        case Op::VLe:
        case Op::VGt:
        case Op::VGe:
            if (!lane(inst, inst.op == Op::VRem) || !signedness(inst) || !useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128))
                return false;
            define(inst, Type::V128);
            return true;
        case Op::VNeg:
        case Op::VAbs:
        case Op::VSqrt:
            if (!lane(inst, false, inst.op == Op::VSqrt) || !useTyped(inst.a, Type::V128))
                return false;
            define(inst, Type::V128);
            return true;
        case Op::VAnd:
        case Op::VOr:
        case Op::VXor:
            if (!useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128))
                return false;
            m_module->usesVectors = true;
            define(inst, Type::V128);
            return true;
        case Op::VNot:
            if (!useTyped(inst.a, Type::V128))
                return false;
            define(inst, Type::V128);
            return true;
        case Op::VShl:
        case Op::VShrS:
        case Op::VShrU:
            if (!lane(inst, true) || !useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::I32))
                return false;
            define(inst, Type::V128);
            return true;
        case Op::VSelect:
            if (!useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128) || !useTyped(inst.c, Type::V128))
                return false;
            define(inst, Type::V128);
            return true;
        case Op::VShuffle: {
            if (!useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128) || !sixteenBytes(inst))
                return false;
            for (unsigned i = 0; i < 2; ++i) {
                uint64_t word = static_cast<uint64_t>(m_function->extra[inst.extraOffset + i]);
                for (unsigned byte = 0; byte < 8; ++byte) {
                    if (((word >> (byte * 8)) & 0xff) > 31)
                        return fail("shuffle index out of range"_s);
                }
            }
            define(inst, Type::V128);
            return true;
        }
        case Op::VConvert:
            if (!u8(inst.aux) || !useTyped(inst.a, Type::V128))
                return false;
            if (inst.aux > static_cast<uint8_t>(VConvertKind::F64x2ToI64x2U))
                return fail("bad vector conversion"_s);
            define(inst, Type::V128);
            return true;
        case Op::VAddSat:
        case Op::VSubSat:
        case Op::VNarrow: {
            if (!lane(inst, true) || !signedness(inst) || !useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128))
                return false;
            Lane shape = static_cast<Lane>(inst.aux);
            bool ok = inst.op == Op::VNarrow ? (shape == Lane::I16x8 || shape == Lane::I32x4) : (shape == Lane::I8x16 || shape == Lane::I16x8);
            if (!ok)
                return fail("operation is not defined for this lane shape"_s);
            define(inst, Type::V128);
            return true;
        }
        case Op::VAvgU: {
            if (!lane(inst, true) || !useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128))
                return false;
            Lane shape = static_cast<Lane>(inst.aux);
            if (shape != Lane::I8x16 && shape != Lane::I16x8)
                return fail("operation is not defined for this lane shape"_s);
            define(inst, Type::V128);
            return true;
        }
        case Op::VExtMul: {
            uint8_t high;
            if (!lane(inst, true) || !signedness(inst) || !u8(high) || !useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128))
                return false;
            if (static_cast<Lane>(inst.aux) == Lane::I8x16 || high > 1)
                return fail("bad VExtMul"_s);
            inst.imm |= static_cast<int64_t>(high) << 8;
            define(inst, Type::V128);
            return true;
        }
        case Op::VDot:
        case Op::VSwizzle:
            if (!useTyped(inst.a, Type::V128) || !useTyped(inst.b, Type::V128))
                return false;
            m_module->usesVectors = true;
            define(inst, Type::V128);
            return true;
        case Op::VBitmask:
        case Op::VAllTrue:
            if (!lane(inst, true) || !useTyped(inst.a, Type::V128))
                return false;
            define(inst, Type::I32);
            return true;
        case Op::VAnyTrue:
            if (!useTyped(inst.a, Type::V128))
                return false;
            define(inst, Type::I32);
            return true;
        case Op::AtomicLoad:
            if (!atomicKind(inst, true) || !memoryOrder(inst, 0) || !useTyped(inst.a, Type::I64))
                return false;
            define(inst, atomicValueType(static_cast<MemKind>(inst.aux)));
            return true;
        case Op::AtomicStore:
            return atomicKind(inst, false) && memoryOrder(inst, 0) && useTyped(inst.a, atomicValueType(static_cast<MemKind>(inst.aux))) && useTyped(inst.b, Type::I64);
        case Op::AtomicRmw: {
            uint8_t op;
            if (!u8(op))
                return false;
            if (op > static_cast<uint8_t>(AtomicOp::Exchange))
                return fail("bad atomic operation"_s);
            inst.imm |= static_cast<int64_t>(op) << 16;
            if (!atomicKind(inst, false) || !memoryOrder(inst, 0) || !useTyped(inst.a, atomicValueType(static_cast<MemKind>(inst.aux))) || !useTyped(inst.b, Type::I64))
                return false;
            define(inst, atomicValueType(static_cast<MemKind>(inst.aux)));
            return true;
        }
        case Op::AtomicCas: {
            if (!atomicKind(inst, false) || !memoryOrder(inst, 0) || !memoryOrder(inst, 8))
                return false;
            Type valueType = atomicValueType(static_cast<MemKind>(inst.aux));
            if (!useTyped(inst.a, valueType) || !useTyped(inst.b, valueType) || !useTyped(inst.c, Type::I64))
                return false;
            define(inst, valueType);
            return true;
        }
        case Op::Fence:
            return memoryOrder(inst, 0);
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::UDiv:
        case Op::Rem:
        case Op::URem:
        case Op::And:
        case Op::Or:
        case Op::Xor:
            if (!use(inst.a, a) || !use(inst.b, b))
                return false;
            if (a != b)
                return fail("binary operands differ in type"_s);
            if (a == Type::V128)
                return fail("scalar operation on a vector"_s);
            if (inst.op >= Op::UDiv && !isInt(a))
                return fail("integer operation on a float"_s);
            define(inst, a);
            return true;
        case Op::Shl:
        case Op::ShrS:
        case Op::ShrU:
            if (!use(inst.a, a) || !useTyped(inst.b, Type::I32))
                return false;
            if (!isInt(a))
                return fail("shift of a float"_s);
            define(inst, a);
            return true;
        case Op::Neg:
            if (!use(inst.a, a))
                return false;
            if (a == Type::V128)
                return fail("scalar operation on a vector"_s);
            define(inst, a);
            return true;
        case Op::RotL:
        case Op::RotR:
            if (!use(inst.a, a) || !useTyped(inst.b, Type::I32))
                return false;
            if (!isInt(a))
                return fail("rotate of a non-integer"_s);
            define(inst, a);
            return true;
        case Op::MulHigh:
        case Op::UMulHigh:
            if (!use(inst.a, a) || !use(inst.b, b))
                return false;
            if (a != b || !isInt(a))
                return fail("MulHigh operands must be the same integer type"_s);
            define(inst, a);
            return true;
        case Op::Clz:
        case Op::Ctz:
        case Op::Popcnt:
        case Op::Bswap:
            if (!use(inst.a, a))
                return false;
            if (!isInt(a))
                return fail("bit operation on a float"_s);
            define(inst, a);
            return true;
        case Op::Eq:
        case Op::Ne:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge:
        case Op::ULt:
        case Op::ULe:
        case Op::UGt:
        case Op::UGe:
            if (!use(inst.a, a) || !use(inst.b, b))
                return false;
            if (a != b)
                return fail("compare operands differ in type"_s);
            if (a == Type::V128)
                return fail("scalar operation on a vector"_s);
            if (inst.op >= Op::ULt && !isInt(a))
                return fail("unsigned compare of a float"_s);
            define(inst, Type::I32);
            return true;
        case Op::SExt8:
        case Op::SExt16:
            if (!useTyped(inst.a, Type::I32))
                return false;
            define(inst, Type::I32);
            return true;
        case Op::SExt32:
        case Op::ZExt32:
            if (!useTyped(inst.a, Type::I32))
                return false;
            define(inst, Type::I64);
            return true;
        case Op::Trunc:
            if (!useTyped(inst.a, Type::I64))
                return false;
            define(inst, Type::I32);
            return true;
        case Op::SToF:
        case Op::UToF: {
            Type result;
            if (!type(result, false) || !use(inst.a, a))
                return false;
            if (!isFloat(result) || !isInt(a))
                return fail("bad int-to-float conversion"_s);
            define(inst, result);
            return true;
        }
        case Op::FToS:
        case Op::FToU: {
            Type result;
            if (!type(result, false) || !use(inst.a, a))
                return false;
            if (!isInt(result) || !isFloat(a))
                return fail("bad float-to-int conversion"_s);
            define(inst, result);
            return true;
        }
        case Op::FPromote:
            if (!useTyped(inst.a, Type::F32))
                return false;
            define(inst, Type::F64);
            return true;
        case Op::FDemote:
            if (!useTyped(inst.a, Type::F64))
                return false;
            define(inst, Type::F32);
            return true;
        case Op::Bitcast: {
            Type result;
            if (!type(result, false) || !use(inst.a, a))
                return false;
            bool ok = (result == Type::I32 && a == Type::F32) || (result == Type::F32 && a == Type::I32)
                || (result == Type::I64 && a == Type::F64) || (result == Type::F64 && a == Type::I64);
            if (!ok)
                return fail("bad bitcast"_s);
            define(inst, result);
            return true;
        }
        case Op::Load: {
            if (!u8(inst.aux) || !useTyped(inst.a, Type::I64) || !varint(inst.imm))
                return false;
            inst.isVolatile = inst.aux & volatileAccess;
            inst.aux &= ~volatileAccess;
            if (inst.aux > static_cast<uint8_t>(MemKind::V128))
                return fail("bad memory kind"_s);
            if (inst.imm < INT32_MIN || inst.imm > INT32_MAX)
                return fail("memory offset out of range"_s);
            if (inst.aux == static_cast<uint8_t>(MemKind::V128))
                m_module->usesVectors = true;
            define(inst, memoryValueType(static_cast<MemKind>(inst.aux)));
            return true;
        }
        case Op::Store: {
            if (!u8(inst.aux) || !use(inst.a, a) || !useTyped(inst.b, Type::I64) || !varint(inst.imm))
                return false;
            inst.isVolatile = inst.aux & volatileAccess;
            inst.aux &= ~volatileAccess;
            if (inst.aux > static_cast<uint8_t>(MemKind::V128) || inst.aux == static_cast<uint8_t>(MemKind::I8S) || inst.aux == static_cast<uint8_t>(MemKind::I16S))
                return fail("bad memory kind"_s);
            if (inst.imm < INT32_MIN || inst.imm > INT32_MAX)
                return fail("memory offset out of range"_s);
            if (a != memoryValueType(static_cast<MemKind>(inst.aux)))
                return fail("stored value has the wrong type"_s);
            return true;
        }
        case Op::SlotAddr:
            if (!varuint32(inst.a))
                return false;
            if (inst.a >= m_function->slots.size())
                return fail("slot out of range"_s);
            define(inst, Type::I64);
            return true;
        case Op::DataAddr: {
            uint64_t offset;
            if (!varuint(offset))
                return false;
            if (offset > m_module->data.size)
                return fail("data offset out of range"_s);
            inst.imm = static_cast<int64_t>(offset);
            define(inst, Type::I64);
            return true;
        }
        case Op::InlineAsm: {
            // extra: flags, nbytes, byte*, ninputs, (value, register)*, noutputs, (type, register)*, nclobbers, register*
            auto& extra = m_function->extra;
            inst.extraOffset = static_cast<uint32_t>(extra.size());
            bool isX86 = m_module->arch == Arch::X86_64;
            auto registerIsValid = [&](uint8_t reg, bool mustBeVector, bool mustBeInteger) {
                bool isVector = isX86 ? (reg >= 16 && reg < 32) : (reg >= 32 && reg < 64);
                bool isInteger = isX86 ? (reg < 16 && reg != 4 && reg != 5) : (reg < 31 && reg != 18 && reg != 29 && reg != 30);
                if (!isVector && !isInteger)
                    return false;
                return !(mustBeVector && !isVector) && !(mustBeInteger && !isInteger);
            };
            uint8_t flags;
            uint32_t byteCount, inputCount, outputCount, clobberCount;
            std::span<const uint8_t> code;
            if (!u8(flags) || !count(byteCount) || !bytes(byteCount, code))
                return false;
            if (flags > 1 || byteCount > 4096)
                return fail("bad InlineAsm"_s);
#if CPU(ARM64)
            if (byteCount % 4)
                return fail("InlineAsm code is not whole instructions"_s);
#endif
            extra.append(flags);
            extra.append(byteCount);
            for (uint8_t byte : code)
                extra.append(byte);
            if (!count(inputCount) || inputCount > 16)
                return fail("bad InlineAsm input count"_s);
            extra.append(inputCount);
            for (uint32_t i = 0; i < inputCount; ++i) {
                uint32_t id;
                Type type;
                uint8_t reg;
                if (!use(id, type) || !u8(reg))
                    return false;
                bool isIntegerValue = type == Type::I32 || type == Type::I64;
                if (!registerIsValid(reg, !isIntegerValue, isIntegerValue))
                    return fail("bad InlineAsm input register"_s);
                extra.append(id);
                extra.append(reg);
            }
            if (!count(outputCount) || outputCount > 16)
                return fail("bad InlineAsm output count"_s);
            extra.append(outputCount);
            for (uint32_t i = 0; i < outputCount; ++i) {
                uint8_t type, reg;
                if (!u8(type) || !u8(reg))
                    return false;
                if (type < static_cast<uint8_t>(Type::I32) || type > static_cast<uint8_t>(Type::V128))
                    return fail("bad InlineAsm output type"_s);
                bool isIntegerValue = static_cast<Type>(type) == Type::I32 || static_cast<Type>(type) == Type::I64;
                if (!registerIsValid(reg, !isIntegerValue, isIntegerValue))
                    return fail("bad InlineAsm output register"_s);
                if (static_cast<Type>(type) == Type::V128)
                    m_module->usesVectors = true;
                extra.append(type);
                extra.append(reg);
            }
            if (!count(clobberCount) || clobberCount > 64)
                return fail("bad InlineAsm clobber count"_s);
            extra.append(clobberCount);
            for (uint32_t i = 0; i < clobberCount; ++i) {
                uint8_t reg;
                if (!u8(reg))
                    return false;
                if (!registerIsValid(reg, false, false))
                    return fail("bad InlineAsm clobber"_s);
                extra.append(reg);
            }
            inst.extraCount = static_cast<uint32_t>(extra.size()) - inst.extraOffset;
            // Outputs are defined last: `define` numbers them after the inputs were checked against
            // earlier values only.
            size_t cursor = inst.extraOffset + 2 + byteCount + 1 + 2 * inputCount + 1;
            for (uint32_t i = 0; i < outputCount; ++i)
                define(inst, static_cast<Type>(extra[cursor + 2 * i]));
            return true;
        }
        case Op::CpuId:
            if (m_module->arch != Arch::X86_64)
                return fail("CpuId is an x86-64 instruction"_s);
            if (!useTyped(inst.a, Type::I32) || !useTyped(inst.b, Type::I32))
                return false;
            for (unsigned i = 0; i < 4; ++i)
                define(inst, Type::I32);
            return true;
        case Op::FrameAddress:
            m_function->usesFrameAddress = true;
            define(inst, Type::I64);
            return true;
        case Op::TlsAddr: {
            uint64_t offset;
            if (!varuint(offset))
                return false;
            if (offset > m_module->tls.size)
                return fail("thread-local offset out of range"_s);
            inst.imm = static_cast<int64_t>(offset);
            m_function->hasCalls = true; // Reaching a thread's copy may call into the runtime.
            define(inst, Type::I64);
            return true;
        }
        case Op::FuncAddr:
            if (!varuint32(inst.a))
                return false;
            if (inst.a >= m_module->functions.size())
                return fail("function out of range"_s);
            m_module->functions[inst.a].isAddressTaken = true;
            define(inst, Type::I64);
            return true;
        case Op::ExternAddr:
            if (!varuint32(inst.a))
                return false;
            if (inst.a >= m_module->externs.size())
                return fail("extern out of range"_s);
            define(inst, Type::I64);
            return true;
        case Op::LocalGet:
            if (!varuint32(inst.a))
                return false;
            if (inst.a >= m_function->locals.size())
                return fail("local out of range"_s);
            define(inst, m_function->locals[inst.a]);
            return true;
        case Op::LocalSet:
            if (!varuint32(inst.a))
                return false;
            if (inst.a >= m_function->locals.size())
                return fail("local out of range"_s);
            return useTyped(inst.b, m_function->locals[inst.a]);
        case Op::Call:
            m_function->hasCalls = true;
            if (!varuint32(inst.a))
                return false;
            if (inst.a >= m_module->functions.size())
                return fail("function out of range"_s);
            ++m_module->functions[inst.a].callSiteCount;
            return callArguments(inst, m_module->signatures[m_module->functions[inst.a].signature], true);
        case Op::CallExtern:
            m_function->hasCalls = true;
            if (!varuint32(inst.a))
                return false;
            if (inst.a >= m_module->externs.size())
                return fail("extern out of range"_s);
            if (m_module->externs[inst.a].kind != ExternKind::Function)
                return fail("call of a data extern"_s);
            return callArguments(inst, m_module->signatures[m_module->externs[inst.a].signature], true);
        case Op::CallIndirect:
            m_function->hasCalls = true;
            if (!varuint32(inst.a))
                return false;
            if (inst.a >= m_module->signatures.size())
                return fail("signature out of range"_s);
            if (!useTyped(inst.b, Type::I64))
                return false;
            return callArguments(inst, m_module->signatures[inst.a], true);
        case Op::Select:
            if (!useTyped(inst.a, Type::I32) || !use(inst.b, a) || !use(inst.c, b))
                return false;
            if (a != b)
                return fail("select arms differ in type"_s);
            define(inst, a);
            return true;
        case Op::MemCopy:
            return useTyped(inst.a, Type::I64) && useTyped(inst.b, Type::I64) && useTyped(inst.c, Type::I64);
        case Op::MemSet:
            return useTyped(inst.a, Type::I64) && useTyped(inst.b, Type::I32) && useTyped(inst.c, Type::I64);
        case Op::StackAlloc: {
            uint64_t alignment;
            if (!useTyped(inst.a, Type::I64) || !varuint(alignment))
                return false;
            if (!alignment || (alignment & (alignment - 1)) || alignment > 4096)
                return fail("bad StackAlloc alignment"_s);
            inst.imm = static_cast<int64_t>(alignment);
            m_function->movesStackPointer = true;
            define(inst, Type::I64);
            return true;
        }
        case Op::StackSave:
            define(inst, Type::I64);
            return true;
        case Op::StackRestore:
            m_function->movesStackPointer = true;
            return useTyped(inst.a, Type::I64);
        case Op::VaStart:
            if (!functionSignature.isVariadic)
                return fail("VaStart in a function that is not variadic"_s);
            return useTyped(inst.a, Type::I64);
        case Op::Jump:
            return blockIndex(inst.a);
        case Op::Br:
            return useTyped(inst.a, Type::I32) && blockIndex(inst.b) && blockIndex(inst.c);
        case Op::Switch: {
            uint32_t caseCount;
            if (!use(inst.a, a) || !blockIndex(inst.b) || !count(caseCount))
                return false;
            if (!isInt(a))
                return fail("switch on a float"_s);
            inst.extraOffset = static_cast<uint32_t>(m_function->extra.size());
            inst.extraCount = caseCount * 2;
            HashSet<int64_t, WTF::IntHash<int64_t>, WTF::UnsignedWithZeroKeyHashTraits<int64_t>> seen;
            for (uint32_t i = 0; i < caseCount; ++i) {
                int64_t caseValue;
                uint32_t target;
                if (!varint(caseValue) || !blockIndex(target))
                    return false;
                if (a == Type::I32 && (caseValue < INT32_MIN || caseValue > INT32_MAX))
                    return fail("switch case out of range"_s);
                if (caseValue == INT64_MAX || caseValue == INT64_MAX - 1)
                    return fail("unsupported switch case value"_s);
                if (!seen.add(caseValue).isNewEntry)
                    return fail("duplicate switch case"_s);
                m_function->extra.append(caseValue);
                m_function->extra.append(target);
            }
            return true;
        }
        case Op::Ret:
            if (functionSignature.results.isEmpty())
                return fail("Ret in a void function"_s);
            inst.extraOffset = static_cast<uint32_t>(m_function->extra.size());
            inst.extraCount = functionSignature.results.size();
            for (Type result : functionSignature.results) {
                uint32_t id;
                if (!useTyped(id, result))
                    return false;
                m_function->extra.append(id);
            }
            return true;
        case Op::RetVoid:
            if (!functionSignature.results.isEmpty())
                return fail("RetVoid in a non-void function"_s);
            return true;
        case Op::Unreachable:
        case Op::Trap:
            return true;
        }
        return fail(makeString("unknown opcode "_s, rawOp));
    }


    static Type laneScalarType(Lane lane)
    {
        switch (lane) {
        case Lane::I8x16:
        case Lane::I16x8:
        case Lane::I32x4:
            return Type::I32;
        case Lane::I64x2:
            return Type::I64;
        case Lane::F32x4:
            return Type::F32;
        case Lane::F64x2:
            return Type::F64;
        }
        return Type::Void;
    }
    static unsigned laneCount(Lane lane)
    {
        switch (lane) {
        case Lane::I8x16:
            return 16;
        case Lane::I16x8:
            return 8;
        case Lane::I32x4:
        case Lane::F32x4:
            return 4;
        case Lane::I64x2:
        case Lane::F64x2:
            return 2;
        }
        return 0;
    }
    static bool isIntegerLane(Lane lane) { return lane <= Lane::I64x2; }

    bool lane(Inst& inst, bool integerOnly = false, bool floatOnly = false)
    {
        if (!u8(inst.aux))
            return false;
        if (inst.aux > static_cast<uint8_t>(Lane::F64x2))
            return fail("bad lane"_s);
        Lane result = static_cast<Lane>(inst.aux);
        if ((integerOnly && !isIntegerLane(result)) || (floatOnly && isIntegerLane(result)))
            return fail("operation is not defined for this lane shape"_s);
        m_module->usesVectors = true;
        return true;
    }
    bool signedness(Inst& inst)
    {
        uint8_t raw;
        if (!u8(raw))
            return false;
        if (raw > 1)
            return fail("bad signedness"_s);
        inst.imm |= raw;
        return true;
    }
    bool sixteenBytes(Inst& inst)
    {
        uint64_t low, high;
        if (!fixed(8, low) || !fixed(8, high))
            return false;
        inst.extraOffset = static_cast<uint32_t>(m_function->extra.size());
        inst.extraCount = 2;
        m_function->extra.append(static_cast<int64_t>(low));
        m_function->extra.append(static_cast<int64_t>(high));
        m_module->usesVectors = true;
        return true;
    }
    static Type atomicValueType(MemKind kind) { return kind == MemKind::I64 ? Type::I64 : Type::I32; }
    bool atomicKind(Inst& inst, bool allowSignedLoads)
    {
        if (!u8(inst.aux))
            return false;
        if (inst.aux > static_cast<uint8_t>(MemKind::I64))
            return fail("bad atomic kind"_s);
        MemKind kind = static_cast<MemKind>(inst.aux);
        if (!allowSignedLoads && (kind == MemKind::I8S || kind == MemKind::I16S))
            return fail("bad atomic kind"_s);
        return true;
    }
    bool memoryOrder(Inst& inst, unsigned shift)
    {
        uint8_t raw;
        if (!u8(raw))
            return false;
        if (raw > static_cast<uint8_t>(MemOrder::SequentiallyConsistent))
            return fail("bad memory order"_s);
        inst.imm |= static_cast<int64_t>(raw) << shift;
        return true;
    }

    static Type memoryValueType(MemKind kind)
    {
        switch (kind) {
        case MemKind::I8S:
        case MemKind::I8U:
        case MemKind::I16S:
        case MemKind::I16U:
        case MemKind::I32:
            return Type::I32;
        case MemKind::I64:
            return Type::I64;
        case MemKind::F32:
            return Type::F32;
        case MemKind::F64:
            return Type::F64;
        case MemKind::V128:
            return Type::V128;
        }
        return Type::Void;
    }

    std::span<const uint8_t> m_bytes;
    size_t m_offset { 0 };
    String m_error;
    Module* m_module { nullptr };
};

} // anonymous namespace

std::expected<std::unique_ptr<Module>, String> Module::decode(std::span<const uint8_t> bytes)
{
    return Decoder(bytes).run();
}

} } } // namespace JSC::FFI::BIR

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(BUN_JSC_ADDITIONS)
