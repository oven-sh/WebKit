/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableWords.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "JSONObject.h"
#include <concepts>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

extern "C" const char* Bun__removableFeatureName(uint16_t featureIndex);

namespace JSC { namespace AOT {

namespace {

struct Null { };
struct PlainName { BuiltinName name; };
struct OptionalName { BuiltinName name; };

struct FeatureUseSummary {
    RemovableFeature feature;
    bool isConditional;
};

class TableWriter {
public:
    explicit TableWriter(const BuiltinTable& table)
        : m_table(table)
    {
    }

    JSValue parse(JSGlobalObject* realm) { return JSONParse(realm, m_out.toString()); }

    void object(const auto&... nameThenValue)
    {
        open('{');
        members(nameThenValue...);
        close('}');
    }

    void list(const auto& range, const auto& writeElement)
    {
        open('[');
        for (auto&& element : range) {
            separate();
            writeElement(element);
        }
        close(']');
    }

    void write(Null) { m_out.append("null"_s); }
    void write(bool value) { m_out.append(value ? "true"_s : "false"_s); }
    void write(const String& text) { m_out.appendQuotedJSONString(text); }
    void write(ASCIILiteral text) { write(String(text)); }
    void write(const std::invocable auto& writeValue) { writeValue(); }
    void write(PlainName plain) { write(String::fromUTF8(m_table.textOf(plain.name))); }
    void write(RuntimeFunctionID id) { id ? write(id.index()) : write(Null { }); }
    void write(OptionalName optional) { optional.name ? write(optional.name) : write(Null { }); }

    template<std::integral Number> requires (!std::same_as<Number, bool>)
    void write(Number number) { m_out.append(static_cast<double>(number)); }

    template<typename Value>
    void write(const std::optional<Value>& value) { value ? write(*value) : write(Null { }); }

    template<typename Record, BuiltinRecordPool pool>
    void write(BuiltinRecordPointer<Record, pool> record) { record ? write(*record) : write(Null { }); }

    template<typename Enum> requires std::is_enum_v<Enum>
    void write(Enum value) { write(wordsOf(value)[static_cast<unsigned>(value)]); }

    template<typename Enum>
    void write(OptionSet<Enum> set)
    {
        open('[');
        std::span<const ASCIILiteral> words = wordsOf(Enum { });
        for (size_t position = 0; position < words.size(); ++position) {
            if ((set.toRaw() >> position) & 1) {
                separate();
                write(words[position]);
            }
        }
        close(']');
    }

    template<typename ID> requires requires(const BuiltinTable& table, ID id) { table.tokenOf(id); }
    void write(ID id) { id ? write(makeString(m_table.tokenOf(id))) : write(Null { }); }

    template<typename Range> requires requires(const Range& range) { range.begin(); range.end(); }
    void write(const Range& range)
    {
        list(range, [&](const auto& element) {
            write(element);
        });
    }

    void write(BuiltinName name)
    {
        ASCIILiteral prefix = m_table.isRegisteredSymbolName(name) ? "@@@"_s : m_table.isSymbolName(name) ? "@@"_s : ""_s;
        write(makeString(prefix, String::fromUTF8(m_table.textOf(name))));
    }

    void write(BuiltinOperand operand)
    {
        switch (operand.kind()) {
        case BuiltinOperand::Kind::Receiver:
            return write("this"_s);
        case BuiltinOperand::Kind::Argument:
            return object("argument"_s, operand.argumentIndex());
        case BuiltinOperand::Kind::ArgumentsFrom:
            return object("argumentsFrom"_s, operand.argumentIndex());
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void write(const BuiltinSlotContents& contents) { object("slot"_s, contents.slot, "type"_s, contents.type); }

    void write(const BuiltinTypePart& part)
    {
        open('{');
        members("part"_s, part.kind);
        switch (part.kind) {
        case BuiltinTypePartKind::Instance:
            members("class"_s, part.builtinClass, "pristine"_s, part.pristine, "contents"_s, part.contents);
            break;
        case BuiltinTypePartKind::InstanceOfReceiverClass:
        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            break;
        case BuiltinTypePartKind::Layout:
            members("layout"_s, part.layout);
            break;
        case BuiltinTypePartKind::Builtin:
            members("builtin"_s, part.builtin);
            break;
        case BuiltinTypePartKind::Operand:
        case BuiltinTypePartKind::CallbackResult:
        case BuiltinTypePartKind::Prototype:
            members("operand"_s, part.operand);
            break;
        case BuiltinTypePartKind::IteratedValues:
            members("operand"_s, part.operand, "isSettled"_s, part.isSettled);
            break;
        case BuiltinTypePartKind::Slot:
            members("operand"_s, part.operand, "slot"_s, part.slot);
            break;
        }
        close('}');
    }

    void write(BuiltinTypeID id)
    {
        if (!id)
            return write(Null { });
        const BuiltinType& type = m_table.at(id);
        auto has = [&](BuiltinTypeFlag flag) {
            return type.flags.contains(flag);
        };
        auto integers = [&] {
            has(BuiltinTypeFlag::HasOnlyIntegers) ? object("minimum"_s, type.integers.minimum, "maximum"_s, type.integers.maximum) : write(Null { });
        };
        object("isAnything"_s, has(BuiltinTypeFlag::IsAnything), "hasAnyObject"_s, has(BuiltinTypeFlag::HasAnyObject), "hasAnyFunction"_s, has(BuiltinTypeFlag::HasAnyFunction),
            "hasPlainData"_s, has(BuiltinTypeFlag::HasPlainData), "primitives"_s, type.primitives, "integers"_s, integers, "stringLiterals"_s, type.stringLiterals,
            "freshness"_s, type.freshness, "parts"_s, type.parts);
    }

    void write(const BuiltinPlace& place)
    {
        open('{');
        members("place"_s, place.kind);
        switch (place.kind) {
        case BuiltinPlaceKind::None:
        case BuiltinPlaceKind::Result:
        case BuiltinPlaceKind::NativeMemory:
            break;
        case BuiltinPlaceKind::Operand:
        case BuiltinPlaceKind::OwnValues:
        case BuiltinPlaceKind::Prototype:
            members("operand"_s, place.operand);
            break;
        case BuiltinPlaceKind::IteratedValues:
            members("operand"_s, place.operand, "isSettled"_s, place.isSettled);
            break;
        case BuiltinPlaceKind::Slot:
            members("operand"_s, place.operand, "slot"_s, place.slot);
            break;
        case BuiltinPlaceKind::ResultSlot:
            members("slot"_s, place.slot);
            break;
        case BuiltinPlaceKind::ResultField:
            members("index"_s, place.fieldIndex, "holdsOnlyWhatIsStored"_s, place.holdsOnlyWhatIsStored);
            break;
        case BuiltinPlaceKind::NewValue:
            members("type"_s, place.newValueType);
            break;
        case BuiltinPlaceKind::Member: {
            auto path = [&] {
                list(place.memberPath, [&](BuiltinName step) {
                    write(OptionalName { step });
                });
            };
            members("operand"_s, place.operand, "path"_s, path, "firstStep"_s, place.firstMemberStep);
            break;
        }
        case BuiltinPlaceKind::NativeCell:
            members("cell"_s, PlainName { place.nativeCell });
            break;
        }
        close('}');
    }

    void write(const BuiltinKey& key)
    {
        open('{');
        members("key"_s, key.kind);
        switch (key.kind) {
        case BuiltinKeyKind::None:
        case BuiltinKeyKind::Any:
        case BuiltinKeyKind::Indexes:
            break;
        case BuiltinKeyKind::Names:
            members("names"_s, key.names);
            break;
        case BuiltinKeyKind::Operand:
        case BuiltinKeyKind::ElementsOf:
            members("operand"_s, key.operand);
            break;
        }
        close('}');
    }

    void write(const BuiltinTransfer& transfer) { object("from"_s, transfer.from, "to"_s, transfer.to); }

    void write(const BuiltinCallback& callback)
    {
        object("callee"_s, callback.callee, "times"_s, callback.times, "constructs"_s, callback.constructs, "newTarget"_s, callback.newTarget, "thisValue"_s, callback.thisValue,
            "arguments"_s, callback.arguments, "spreadArguments"_s, callback.spreadArguments, "resultUse"_s, callback.resultUse, "resultTo"_s, callback.resultTo);
    }

    void write(const BuiltinHandlerCall& call)
    {
        object("callee"_s, call.callee, "times"_s, call.times, "thisValue"_s, call.thisValue, "arguments"_s, call.arguments, "resultUse"_s, call.resultUse);
    }

    void write(const BuiltinPropertyAction& action)
    {
        bool hasIntegrityLevel = action.kind == BuiltinPropertyActionKind::ChangesIntegrity;
        object("action"_s, action.kind, "object"_s, action.object, "key"_s, action.key, "value"_s, action.value,
            "integrityLevel"_s, hasIntegrityLevel ? std::optional(action.integrityLevel) : std::nullopt);
    }

    void write(const BuiltinParameter& parameter)
    {
        object("operand"_s, parameter.operand, "mode"_s, parameter.mode, "accepted"_s, parameter.accepted, "conversion"_s, parameter.conversion, "converted"_s, parameter.converted,
            "refused"_s, parameter.refused, "admitted"_s, parameter.admitted, "uses"_s, parameter.uses, "roles"_s, parameter.roles, "protocols"_s, parameter.protocols,
            "namedMembers"_s, parameter.namedMembers, "calledMembers"_s, parameter.calledMembers);
    }

    void write(const BuiltinResult& result)
    {
        object("type"_s, result.type, "rangeRule"_s, result.rangeRule, "rangeOperand"_s, result.rangeOperand, "substringOf"_s, result.substringOf, "rangeOf"_s, result.rangeOf,
            "typedResultForm"_s, result.typedResultForm, "typedResultSource"_s, result.typedResultSource, "iteratedOperand"_s, result.iteratedOperand,
            "iterationKind"_s, result.iteratedOperand ? std::optional(result.iterationKind) : std::nullopt, "prototypeSource"_s, result.prototypeSource);
    }

    void write(const BuiltinImplementation& implementation)
    {
        object("how"_s, implementation.kind, "otherwise"_s, implementation.otherwise, "callee"_s, implementation.callee, "inlineBuiltin"_s, implementation.inlineBuiltin,
            "maximumArgumentCount"_s, implementation.maximumArgumentCount, "function"_s, implementation.function);
    }

    void writeOverload(BuiltinOverloadID id)
    {
        const BuiltinOverload& overload = m_table.at(id);
        auto has = [&](BuiltinOverloadFlag flag) {
            return overload.flags.contains(flag);
        };
        open('{');
        members("token"_s, m_table.tokenOf(id), "path"_s, m_table.textOf(overload.builtin), "mode"_s, overload.mode,
            "isProvenByConstruction"_s, has(BuiltinOverloadFlag::IsProvenByConstruction), "isUndeclared"_s, overload.isUndeclared(), "receiver"_s, overload.receiver,
            "parameters"_s, overload.parameters, "result"_s, overload.result, "capability"_s, overload.effects.capability, "throws"_s, overload.throws(),
            "onlyPropagatesErrors"_s, overload.onlyPropagatesErrors(), "runsOnlyOperandCode"_s, has(BuiltinOverloadFlag::RunsOnlyOperandCode),
            "readsStateBeforeConversions"_s, has(BuiltinOverloadFlag::ReadsStateBeforeConversions), "isSpecialCase"_s, has(BuiltinOverloadFlag::IsSpecialCase));
        if (overload.resultIsDeclaredOnly())
            members("resultIsDeclaredOnly"_s, true);
        members("reads"_s, overload.effects.reads, "writes"_s, overload.effects.writes, "transfers"_s, overload.flow.transfers, "callbacks"_s, overload.flow.callbacks,
            "propertyActions"_s, overload.flow.propertyActions, "handlerCalls"_s, overload.flow.handlerCalls, "implementation"_s, overload.code.implementation, "model"_s, overload.code.model);
        close('}');
    }

    void writeEntry(BuiltinID id)
    {
        const Builtin& builtin = m_table.at(id);
        auto overloads = [&] {
            list(m_table.overloadIDsOf(id), [&](BuiltinOverloadID overload) {
                writeOverload(overload);
            });
        };
        object("id"_s, id.index(), "token"_s, id, "path"_s, m_table.textOf(id), "kind"_s, builtin.kind, "origin"_s, builtin.origin, "scope"_s, builtin.scope,
            "mutability"_s, builtin.mutability, "platforms"_s, builtin.platforms, "flags"_s, builtin.flags, "holder"_s, builtin.holder, "name"_s, PlainName { builtin.name },
            "isSymbolName"_s, m_table.isSymbolName(builtin.name), "prototypeLink"_s, builtin.prototypeLink, "inheritsFrom"_s, builtin.inheritsFrom,
            "instanceClass"_s, builtin.instanceClass, "sameObjectAs"_s, builtin.sameObjectAs, "overloads"_s, overloads);
    }

    void writeSlot(BuiltinSlotID id)
    {
        const BuiltinSlot& slot = m_table.at(id);
        object("token"_s, id, "name"_s, PlainName { slot.name }, "owner"_s, slot.owner, "heap"_s, slot.heap, "contentsSlot"_s, slot.contentsSlot, "holds"_s, slot.holds);
    }

    void writeClass(BuiltinClassID id)
    {
        const BuiltinClass& builtinClass = m_table.at(id);
        auto names = [&] {
            builtinClass.listsNamesDefinedOnNewInstances ? write(builtinClass.namesDefinedOnNewInstances) : write(Null { });
        };
        auto slots = [&] {
            list(builtinClass.slots, [&](BuiltinSlotID slot) {
                writeSlot(slot);
            });
        };
        object("token"_s, id, "origin"_s, builtinClass.origin, "parent"_s, builtinClass.parent, "constructor"_s, builtinClass.constructor, "prototype"_s, builtinClass.prototype,
            "instanceTest"_s, builtinClass.instanceTest, "isPrimitiveWrapper"_s, builtinClass.isPrimitiveWrapper, "isNeverPristine"_s, builtinClass.isNeverPristine,
            "pristineTest"_s, builtinClass.pristineTest, "hasClosureInstances"_s, builtinClass.hasClosureInstances,
            "definesCharactersOnNewInstances"_s, builtinClass.definesCharactersOnNewInstances, "namesDefinedOnNewInstances"_s, names, "slots"_s, slots, "stateHeap"_s, builtinClass.stateHeap);
    }

    void write(const BuiltinLayoutField& field) { object("name"_s, OptionalName { field.name }, "isOptional"_s, field.isOptional, "type"_s, field.type); }

    void writeLayout(BuiltinLayoutID id)
    {
        const BuiltinLayout& layout = m_table.at(id);
        object("token"_s, id, "isTuple"_s, layout.isTuple, "fields"_s, layout.fields);
    }

    void write(const FeatureUseSummary& summary)
    {
        const char* name = Bun__removableFeatureName(static_cast<uint16_t>(summary.feature));
        object("feature"_s, name ? std::optional(String::fromUTF8(name)) : std::nullopt, "isConditional"_s, summary.isConditional);
    }

private:
    void open(char bracket)
    {
        m_out.append(bracket);
        m_isFirst = true;
    }

    void close(char bracket)
    {
        m_out.append(bracket);
        m_isFirst = false;
    }

    void separate()
    {
        if (!std::exchange(m_isFirst, false))
            m_out.append(',');
    }

    void members() { }
    void members(ASCIILiteral name, const auto& value, const auto&... rest)
    {
        separate();
        m_out.append('"', name, "\":"_s);
        write(value);
        members(rest...);
    }

    const BuiltinTable& m_table;
    StringBuilder m_out;
    bool m_isFirst { true };
};

} // anonymous namespace

template<typename ID>
static BuiltinTableIDRange<ID> idsBelow(unsigned count)
{
    return { ID(0), count };
}

JSValue BuiltinTable::describeForTesting(JSGlobalObject* realm) const
{
    TableWriter writer(*this);
    auto entries = [&] {
        writer.list(idsBelow<BuiltinID>(builtinCount()), [&](BuiltinID id) {
            writer.writeEntry(id);
        });
    };
    auto classes = [&] {
        writer.list(idsBelow<BuiltinClassID>(classCount()), [&](BuiltinClassID id) {
            writer.writeClass(id);
        });
    };
    auto layouts = [&] {
        writer.list(idsBelow<BuiltinLayoutID>(layoutCount()), [&](BuiltinLayoutID id) {
            writer.writeLayout(id);
        });
    };
    writer.object("entries"_s, entries, "classes"_s, classes, "layouts"_s, layouts);
    return writer.parse(realm);
}

static void addFeatureUse(Vector<FeatureUseSummary>& summaries, const BuiltinFeatureUse& use)
{
    bool isConditional = use.overload || use.condition;
    size_t position = summaries.findIf([&](const FeatureUseSummary& summary) {
        return summary.feature == use.feature;
    });
    if (position == notFound) {
        summaries.append({ use.feature, isConditional });
        return;
    }
    summaries[position].isConditional = summaries[position].isConditional && isConditional;
}

static Vector<FeatureUseSummary> summarizeFeatureUses(std::span<const BuiltinFeatureUse> uses)
{
    Vector<FeatureUseSummary> summaries;
    for (const BuiltinFeatureUse& use : uses) {
        if (!use.overload)
            addFeatureUse(summaries, use);
    }
    for (const BuiltinFeatureUse& use : uses) {
        if (use.overload)
            addFeatureUse(summaries, use);
    }
    return summaries;
}

static Vector<RuntimeModuleID> findModulesByRoot(const BuiltinTable& table)
{
    Vector<RuntimeModuleID> modules(FillWith { }, table.builtinCount(), RuntimeModuleID());
    for (RuntimeModuleID module : idsBelow<RuntimeModuleID>(table.moduleCount())) {
        BuiltinID namespaceObject = table.namespaceOf(module);
        if (namespaceObject && !table.at(namespaceObject).holder)
            modules[namespaceObject.index()] = module;
    }
    return modules;
}

static bool hasHiddenRoot(const BuiltinTable& table, const Vector<RuntimeModuleID>& modulesByRoot, BuiltinID id)
{
    BuiltinID root = table.rootOf(id);
    const Builtin& builtin = table.at(root);
    if (builtin.holder || root == Builtins::GlobalThis)
        return false;
    return builtin.kind != BuiltinKind::NativeBinding && !modulesByRoot[root.index()];
}

JSValue BuiltinTable::describeFeatureUsesForTesting(JSGlobalObject* realm, BuiltinPlatform platform) const
{
    TableWriter writer(*this);
    Vector<RuntimeModuleID> modulesByRoot = findModulesByRoot(*this);
    Vector<BuiltinID> users;
    for (BuiltinID id : idsBelow<BuiltinID>(builtinCount())) {
        if (existsOn(id, platform) && !featureUsesOf(id).empty())
            users.append(id);
    }
    writer.list(users, [&](BuiltinID id) {
        RuntimeModuleID module = modulesByRoot[id.index()];
        auto kind = [&] {
            if (!module)
                return writer.write(at(id).kind);
            writer.write(at(module).flags.contains(RuntimeModuleFlag::IsPublic) ? "Module"_s : "InternalModule"_s);
        };
        writer.object("id"_s, id.index(), "path"_s, textOf(id), "kind"_s, kind, "uses"_s, summarizeFeatureUses(featureUsesOf(id)), "nativeSymbols"_s, Vector<String>(),
            "hasHiddenRoot"_s, hasHiddenRoot(*this, modulesByRoot, id));
    });
    return writer.parse(realm);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
