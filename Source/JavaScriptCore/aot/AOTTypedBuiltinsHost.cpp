/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTFunction.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedThreads.h"
#include "JSCInlines.h"
#include <wtf/UnalignedAccess.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/unicode/UTF8Conversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constinit Typed::HostClassEntryPoints s_hostClassEntryPoints;

static constexpr unsigned minRopeLengthEncodedWithoutFlattening = 256;
static constexpr uint32_t bodyIsUnsharedString = 1u << 31;

static ALWAYS_INLINE bool copyAndCheckASCII(uint8_t* to, const Latin1Character* from, size_t size)
{
    constexpr uint64_t highBits = 0x8080808080808080ull;
    if (size >= 16) {
        if (size > 128) {
            memcpy(to, from, size);
            return charactersAreAllASCII(std::span { from, size });
        }
        uint64_t bits = 0;
        auto copy = [&](size_t at) ALWAYS_INLINE_LAMBDA {
            uint64_t first = WTF::unalignedLoad<uint64_t>(from + at);
            uint64_t second = WTF::unalignedLoad<uint64_t>(from + at + 8);
            WTF::unalignedStore<uint64_t>(to + at, first);
            WTF::unalignedStore<uint64_t>(to + at + 8, second);
            bits |= first | second;
        };
        for (size_t at = 0; at + 16 <= size; at += 16)
            copy(at);
        copy(size - 16);
        return !(bits & highBits);
    }
    if (size >= 8) {
        uint64_t first = WTF::unalignedLoad<uint64_t>(from);
        uint64_t last = WTF::unalignedLoad<uint64_t>(from + size - 8);
        WTF::unalignedStore<uint64_t>(to, first);
        WTF::unalignedStore<uint64_t>(to + size - 8, last);
        return !((first | last) & highBits);
    }
    if (size >= 4) {
        uint32_t first = WTF::unalignedLoad<uint32_t>(from);
        uint32_t last = WTF::unalignedLoad<uint32_t>(from + size - 4);
        WTF::unalignedStore<uint32_t>(to, first);
        WTF::unalignedStore<uint32_t>(to + size - 4, last);
        return !((first | last) & static_cast<uint32_t>(highBits));
    }
    unsigned bits = 0;
    for (size_t at = 0; at < size; ++at) {
        to[at] = from[at];
        bits |= from[at];
    }
    return !(bits & 0x80);
}

static bool tryEncodeRopeAsUTF8(JSString* root, Vector<uint8_t>& bytes)
{
    static_assert(sizeof(void*) == 8 && std::endian::native == std::endian::little);
    size_t length = root->length();
    if (!bytes.tryGrow(length + length / 8 + 64))
        return false;
    uint8_t* start = bytes.mutableSpan().data();
    uint8_t* cursor = start;
    uint8_t* end = start + bytes.size();
    auto ensureRoom = [&](size_t room) ALWAYS_INLINE_LAMBDA {
        if (room <= static_cast<size_t>(end - cursor)) [[likely]]
            return true;
        size_t size = cursor - start;
        if (!bytes.tryGrow(std::max(bytes.size() * 2, size + room)))
            return false;
        start = bytes.mutableSpan().data();
        cursor = start + size;
        end = start + bytes.size();
        return true;
    };
    auto appendConverted = [&](auto characters, size_t mostBytesPerCharacter) {
        if (!ensureRoom(characters.size() * mostBytesPerCharacter))
            return false;
        auto converted = WTF::Unicode::convert(characters, std::span { std::bit_cast<char8_t*>(cursor), characters.size() * mostBytesPerCharacter });
        if (converted.code != WTF::Unicode::ConversionResultCode::Success)
            return false;
        cursor += converted.buffer.size();
        return true;
    };
    auto append = [&](const StringImpl& impl, size_t offset, size_t count) ALWAYS_INLINE_LAMBDA {
        if (!impl.is8Bit()) [[unlikely]]
            return appendConverted(impl.span16().subspan(offset, count), 3);
        const Latin1Character* characters = impl.span8().data() + offset;
        if (!ensureRoom(count))
            return false;
        if (!copyAndCheckASCII(cursor, characters, count)) [[unlikely]]
            return appendConverted(std::span { characters, count }, 2);
        cursor += count;
        return true;
    };
    Vector<JSString*, 128> pending;
    for (JSString* string = root;;) {
        if (const StringImpl* impl = string->tryGetValueImpl()) {
            if (!append(*impl, 0, impl->length()))
                return false;
        } else {
            auto* words = std::bit_cast<const uint8_t*>(string);
            auto* second = std::bit_cast<JSString*>(static_cast<uintptr_t>(WTF::unalignedLoad<uint32_t>(words + JSRopeString::offsetOfFiber1Lower())) | static_cast<uintptr_t>(WTF::unalignedLoad<uint16_t>(words + JSRopeString::offsetOfFiber2())) << 32);
            uintptr_t third = WTF::unalignedLoad<uint64_t>(words + JSRopeString::offsetOfFiber2()) >> 16;
            if (!string->isSubstring()) {
                if (third)
                    pending.append(std::bit_cast<JSString*>(third));
                if (second)
                    pending.append(second);
                string = std::bit_cast<JSString*>(WTF::unalignedLoad<uintptr_t>(words + JSRopeString::offsetOfFiber0()) & ~static_cast<uintptr_t>(7));
                continue;
            }
            const StringImpl* whole = second->tryGetValueImpl();
            if (!whole || !append(*whole, third, string->length()))
                return false;
        }
        if (pending.isEmpty())
            break;
        string = pending.takeLast();
    }
    size_t size = cursor - start;
    bytes.shrink(size);
    return size < 3 || start[0] != 0xef || start[1] != 0xbb || start[2] != 0xbf;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewResponse, EncodedJSValue, (Instance* instance, EncodedJSValue body, EncodedJSValue status, EncodedJSValue statusText, EncodedJSValue headers, uint32_t headerCount, const EncodedJSValue* headerNamesAndValues))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    bool isUnshared = headerCount & bodyIsUnsharedString;
    headerCount &= ~bodyIsUnsharedString;
    if (JSValue text = JSValue::decode(body); isUnshared && s_hostClassEntryPoints.newResponseFromUTF8 && text.isString() && asString(text)->isRope() && !asString(text)->isSubstring() && asString(text)->length() >= minRopeLengthEncodedWithoutFlattening) {
        Vector<uint8_t> bytes;
        if (tryEncodeRopeAsUTF8(asString(text), bytes)) {
            EncodedJSValue response = s_hostClassEntryPoints.newResponseFromUTF8(globalObject, bytes.span().data(), bytes.size(), status, statusText, headers, headerCount, headerNamesAndValues);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            OPERATION_RETURN(scope, response);
        }
    }
    EncodedJSValue response = s_hostClassEntryPoints.newResponse(globalObject, body, status, statusText, headers, headerCount, headerNamesAndValues);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, response);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewResponseFromResponse, EncodedJSValue, (Instance* instance, EncodedJSValue other))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    EncodedJSValue response = s_hostClassEntryPoints.newResponseFromResponse(globalObject, other);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, response);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetRequestOrResponseHeader, EncodedJSValue, (Instance* instance, EncodedJSValue holder, EncodedJSValue name, uint32_t knownName))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    EncodedJSValue header = s_hostClassEntryPoints.getRequestOrResponseHeader(globalObject, holder, name, knownName);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, header);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCallHeadersMethod, EncodedJSValue, (Instance* instance, EncodedJSValue headersOrHolder, EncodedJSValue name, EncodedJSValue value, uint32_t methodAndKnownName))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    EncodedJSValue answer = s_hostClassEntryPoints.callHeadersMethod(globalObject, headersOrHolder, name, value, methodAndKnownName);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, answer);
}

namespace Typed {

using namespace B3;

void setHostClassEntryPoints(const HostClassEntryPoints& entryPoints)
{
    s_hostClassEntryPoints = entryPoints;
}

const HostClassEntryPoints& hostClassEntryPoints()
{
    return s_hostClassEntryPoints;
}

std::optional<Writes> writesOfInlineHost(InlineBuiltin which, std::span<const Type>)
{
    RELEASE_ASSERT(which == InlineBuiltin::NewResponse || which == InlineBuiltin::NewResponseFromResponse || which == InlineBuiltin::GetRequestOrResponseHeader || which == InlineBuiltin::CallHeadersMethod);
    return Writes::Heap;
}

std::optional<Operand> Lowering::lowerInlineHost(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    if (which == InlineBuiltin::NewResponseFromResponse)
        return ensure(AOT_TYPED_OPERATION(NewResponseFromResponse, in<Boxed>(value.arguments[0])), typeOf(result));
    if (which == InlineBuiltin::GetRequestOrResponseHeader || which == InlineBuiltin::CallHeadersMethod) {
        ValueID number = value.arguments.last();
        while (m_function.at(number).opcode == Opcode::Cast)
            number = m_function.at(number).child(0);
        Word32 known = m_function.at(number).opcode == Opcode::Integer ? word32(static_cast<int32_t>(m_function.at(number).integer())) : in<Word32>(number);
        if (which == InlineBuiltin::CallHeadersMethod)
            return ensure(AOT_TYPED_OPERATION(CallHeadersMethod, in<Boxed>(value.arguments[0]), in<Boxed>(value.arguments[1]), in<Boxed>(value.arguments[2]), known), typeOf(result));
        return ensure(AOT_TYPED_OPERATION(GetRequestOrResponseHeader, in<Boxed>(value.arguments[0]), in<Boxed>(value.arguments[1]), known), typeOf(result));
    }
    RELEASE_ASSERT(which == InlineBuiltin::NewResponse);
    constexpr unsigned firstHeaderOperand = 4;
    auto& arguments = value.arguments;
    unsigned headerOperandCount = arguments.size() - firstHeaderOperand;
    LValue buffer = transientBuffer(std::max(headerOperandCount, 1u) * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < headerOperandCount; ++i)
        m_out.store64(in<Boxed>(arguments[firstHeaderOperand + i]).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    return ensure(AOT_TYPED_OPERATION(NewResponse, in<Boxed>(arguments[0]), in<Boxed>(arguments[1]), in<Boxed>(arguments[2]), in<Boxed>(arguments[3]), word32(static_cast<int32_t>(headerOperandCount / 2 | (value.takesUnsharedString ? bodyIsUnsharedString : 0))), Pointer(buffer)), typeOf(result));
}

void findUnsharedResponseBodies(Program& program)
{
    constexpr unsigned maxDepth = 16;
    constexpr unsigned maxRounds = 4;
    constexpr unsigned result = UINT_MAX;
    constexpr uint32_t unlisted = UINT32_MAX;
    auto& constProgram = std::as_const(program);
    auto& functions = constProgram.functions;
    unsigned count = functions.size();
    static constexpr OpcodeSet builtinCalls { Opcode::CallBuiltin };
    static constexpr OpcodeSet directCalls { Opcode::CallDirect };

    Vector<Vector<ValueID, 1>> sites(count);
    std::atomic<bool> hasSites { false };
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        if (function.blocks.isEmpty() || !constProgram.hasCode(FunctionID(i)))
            return;
        functions.forEachIndexedValue(i, builtinCalls, [&](ValueID id) {
            if (function.at(id).builtin() != Operations::NewResponse)
                return;
            sites[i].append(id);
            hasSites.store(true, std::memory_order_relaxed);
        });
    });
    if (!hasSites.load())
        return;

    struct Facts {
        Vector<uint32_t> numberOfUses;
        Vector<uint32_t> blockOf;
        Vector<uint32_t> numberOfPredecessors;
        BitVector isInLoop;
    };
    UncheckedKeyHashMap<unsigned, std::unique_ptr<Facts>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> factsOfFunctions;
    auto factsOf = [&](unsigned i) -> const Facts& {
        auto& facts = factsOfFunctions.add(i, nullptr).iterator->value;
        if (facts)
            return *facts;
        facts = makeUniqueWithoutFastMallocCheck<Facts>();
        auto& function = functions[i];
        facts->numberOfUses.fill(0, function.values.size());
        facts->blockOf.fill(unlisted, function.values.size());
        facts->numberOfPredecessors.fill(0, function.blocks.size());
        facts->isInLoop = blocksInLoops(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            for (ValueID parameter : block.parameters)
                facts->blockOf[parameter.index()] = b;
            for (ValueID id : block.values) {
                facts->blockOf[id.index()] = b;
                function.at(id).forEachChild([&](ValueID child) {
                    ++facts->numberOfUses[child.index()];
                });
            }
            if (block.terminator.value)
                ++facts->numberOfUses[block.terminator.value.index()];
            for (auto& target : block.terminator.targets) {
                ++facts->numberOfPredecessors[target.block.index()];
                for (ValueID argument : target.arguments)
                    ++facts->numberOfUses[argument.index()];
            }
        }
        return *facts;
    };

    BitVector hasOtherEntries(count);
    for (auto& layout : constProgram.layouts) {
        if (layout.constructor)
            hasOtherEntries.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                hasOtherEntries.set(method.function.index());
        }
    }
    for (auto& module : constProgram.builtinModules)
        hasOtherEntries.set(module.second.index());
    for (auto& builtin : constProgram.builtinFunctions)
        hasOtherEntries.set(builtin.function.index());
    for (FunctionID function : constProgram.preludeFunctions) {
        if (function)
            hasOtherEntries.set(function.index());
    }
    auto isOnlyCalledDirectly = [&](unsigned i) {
        auto& function = functions[i];
        constexpr uint32_t disqualifyingFlags = Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasNewTarget | Format::FunctionIsBuiltin | Format::FunctionIsModuleBody | Format::FunctionIsSloppy | Format::FunctionIsDeclaredGenericEntrypoint | Format::FunctionIsUsedAsValue | FunctionHasUnknownCallers | FunctionIsEnteredByValue | FunctionUsesArgumentBuffer | FunctionIsPartOfHiddenCoroutine;
        return (function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow) && !function.has(disqualifyingFlags) && !function.isGenericEntrypoint() && !hasOtherEntries.get(i) && !constProgram.isEntry(FunctionID(i)) && !constProgram.isResumeFunction(function);
    };

    struct Claim {
        unsigned function;
        unsigned parameter;
        bool holds { true };
    };
    Vector<Claim> claims;
    auto holds = [&](unsigned function, unsigned parameter) {
        size_t index = claims.findIf([&](auto& claim) { return claim.function == function && claim.parameter == parameter; });
        if (index != notFound)
            return claims[index].holds;
        claims.append({ function, parameter, true });
        return true;
    };
    struct Caller {
        unsigned function;
        ValueID call;
    };
    UncheckedKeyHashMap<unsigned, Vector<Caller>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> callersOf;
    BitVector isWanted(count);

    auto isNonEmptyConstant = [&](const Function& function, ValueID id) {
        auto& value = function.at(id);
        return value.opcode == Opcode::String && !constProgram.string(value.string()).empty();
    };
    auto isUnshared = [&](auto& isUnshared, unsigned i, ValueID id, unsigned userBlock, unsigned allowedUses, unsigned depth) -> bool {
        if (depth > maxDepth)
            return false;
        auto& function = functions[i];
        auto& facts = factsOf(i);
        auto& value = function.at(id);
        unsigned block = facts.blockOf[id.index()];
        if (facts.numberOfUses[id.index()] > allowedUses || block == unlisted || (block != userBlock && facts.isInLoop.get(userBlock)))
            return false;
        auto isNewOrUnshared = [&](std::span<const ValueID> operands) {
            if (std::ranges::any_of(operands, [&](ValueID operand) { return isNonEmptyConstant(function, operand); }))
                return true;
            return std::ranges::all_of(operands, [&](ValueID operand) {
                return !(constProgram.typeOf(function, operand).primitives & Primitive::String) || isUnshared(isUnshared, i, operand, block, 1, depth + 1);
            });
        };
        switch (value.opcode) {
        case Opcode::String:
            return true;
        case Opcode::Strcat:
            return isNewOrUnshared(value.arguments.span());
        case Opcode::Add:
            return constProgram.at(value.type).isString() && isNewOrUnshared(std::span { value.children }.first(2));
        case Opcode::Cast:
        case Opcode::ToString:
            return isUnshared(isUnshared, i, value.child(0), block, 1, depth + 1);
        case Opcode::CallDirect: {
            if (value.constructs || value.isJumpToContinuation())
                return false;
            auto& callee = functions[value.function().index()];
            if (callee.blocks.size() == 1 && callee.blocks[0].terminator.kind == TerminatorKind::Return && callee.blocks[0].terminator.value) {
                auto& body = callee.at(callee.blocks[0].terminator.value);
                if (body.opcode == Opcode::Strcat) {
                    for (ValueID operand : body.arguments) {
                        while (callee.at(operand).opcode == Opcode::Cast || callee.at(operand).opcode == Opcode::ToString)
                            operand = callee.at(operand).child(0);
                        auto& passed = callee.at(operand);
                        if (passed.opcode == Opcode::Parameter && passed.parameterIndex() < value.arguments.size() && isNonEmptyConstant(function, value.arguments[passed.parameterIndex()]))
                            return true;
                    }
                }
            }
            return holds(value.function().index(), result);
        }
        case Opcode::Parameter: {
            if (!block)
                return holds(i, value.parameterIndex());
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                for (auto& target : function.blocks[b].terminator.targets) {
                    if (target.block.index() != block)
                        continue;
                    if (value.parameterIndex() >= target.arguments.size() || !isUnshared(isUnshared, i, target.arguments[value.parameterIndex()], b, 1, depth + 1))
                        return false;
                }
            }
            return true;
        }
        default:
            return false;
        }
    };
    auto evaluate = [&](Claim claim) {
        auto& function = functions[claim.function];
        if (function.blocks.isEmpty())
            return false;
        if (claim.parameter == result) {
            if (function.declaredAs & (functionIsAsync | functionIsGenerator) || !function.resultTypes.isEmpty() || function.resumeFunction || function.startFunction)
                return false;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                auto& terminator = function.blocks[b].terminator;
                if (terminator.kind == TerminatorKind::Return && (!terminator.value || !isUnshared(isUnshared, claim.function, terminator.value, b, 1, 0)))
                    return false;
            }
            return true;
        }
        if (!isOnlyCalledDirectly(claim.function))
            return false;
        auto callers = callersOf.find(claim.function);
        if (callers == callersOf.end()) {
            isWanted.set(claim.function);
            return true;
        }
        for (auto& caller : callers->value) {
            auto& call = functions[caller.function].at(caller.call);
            unsigned block = factsOf(caller.function).blockOf[caller.call.index()];
            if (block == unlisted)
                continue;
            if (call.constructs || call.isJumpToContinuation() || claim.parameter >= call.arguments.size() || !isUnshared(isUnshared, caller.function, call.arguments[claim.parameter], block, 1, 0))
                return false;
        }
        return true;
    };
    auto takesUnsharedString = [&](unsigned i, ValueID site) {
        auto& function = functions[i];
        auto& facts = factsOf(i);
        unsigned block = facts.blockOf[site.index()];
        if (block == unlisted || facts.isInLoop.get(block))
            return false;
        ValueID body = function.at(site).arguments[0];
        unsigned allowedUses = 1;
        if (facts.numberOfUses[body.index()] == 2 && facts.numberOfPredecessors[block] == 1) {
            for (auto& fork : function.blocks) {
                if (fork.terminator.kind != TerminatorKind::Branch || !fork.terminator.targets.containsIf([&](auto& target) { return target.block.index() == block; }))
                    continue;
                for (auto& target : fork.terminator.targets) {
                    unsigned other = target.block.index();
                    if (other == block || facts.numberOfPredecessors[other] != 1 || facts.isInLoop.get(other))
                        continue;
                    for (ValueID id : function.blocks[other].values) {
                        auto& value = function.at(id);
                        if (value.opcode == Opcode::Construct && value.arguments.size() && value.arguments[0] == body)
                            allowedUses = 2;
                    }
                }
            }
        }
        return isUnshared(isUnshared, i, body, block, allowedUses, 0);
    };

    for (unsigned i = 0; i < count; ++i) {
        for (ValueID site : sites[i])
            takesUnsharedString(i, site);
    }
    for (unsigned round = 0;; ++round) {
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned k = 0; k < claims.size(); ++k) {
                if (claims[k].holds && !evaluate(claims[k])) {
                    claims[k].holds = false;
                    changed = true;
                }
            }
        }
        if (isWanted.isEmpty())
            break;
        if (round == maxRounds) {
            for (auto& claim : claims) {
                if (claim.parameter != result && isWanted.get(claim.function))
                    callersOf.add(claim.function, Vector<Caller> { });
                if (claim.parameter != result && isWanted.get(claim.function))
                    claim.holds = false;
            }
            isWanted.clearAll();
            continue;
        }
        Vector<Vector<std::pair<unsigned, Caller>>> found(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = functions[i];
            if (function.blocks.isEmpty() || !constProgram.hasCode(FunctionID(i)))
                return;
            functions.forEachIndexedValue(i, directCalls, [&](ValueID id) {
                unsigned callee = function.at(id).function().index();
                if (isWanted.get(callee))
                    found[i].append({ callee, Caller { i, id } });
            });
        });
        for (size_t callee : isWanted)
            callersOf.add(static_cast<unsigned>(callee), Vector<Caller> { });
        for (auto& list : found) {
            for (auto& [callee, caller] : list)
                callersOf.find(callee)->value.append(caller);
        }
        isWanted.clearAll();
    }
    for (unsigned i = 0; i < count; ++i) {
        for (ValueID site : sites[i]) {
            if (takesUnsharedString(i, site))
                program.functions.atKeepingValueIndex(i).at(site).takesUnsharedString = true;
        }
    }
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
