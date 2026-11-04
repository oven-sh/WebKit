/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations and DirectCalls, a proxy in sight: `const p = new Proxy(t, { get(t, k) { return 1 } }); p.x`. The receiver IS the record that this unit built for `new Proxy(..)`, and such a proxy is never revoked,
// so target and handler are the two operands of that record for good. The access is written out as the engine does it: the trap is read off the handler, called, and its answer held against the target.
// Every way is built: a trap that is missing asks the target, one that cannot be called throws. At a suspension: nothing is kept. At a throw: each part throws where the engine would. Withheld: the access stays as written.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "JSObject.h"
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// ToPropertyKey of any other key may run code of the program, and the access as written runs it once.
static constexpr TypeID keysHandedToTrapsWithoutCodeInKeyOrdersProxies = typeOfPrimitives(Primitive::String | Primitive::Symbol | Primitive::Integer);

namespace {

enum class TrapInKeyOrdersProxies : uint8_t { Get, Set, Has, DeleteProperty, GetOwnPropertyDescriptor };

// The operation of the body, by its parts.
struct AccessInKeyOrdersProxies {
    TrapInKeyOrdersProxies trap;
    ValueID object;
    // Null: the access names the property by a value.
    PropertyKeyID key;
    ValueID property;
    ValueID stored;
    ECMAMode mode { ECMAMode::Sloppy };
};

struct ProxyAccessInKeyOrdersProxies {
    Walker& walker;
    const BodyValue& operation;
    const AccessInKeyOrdersProxies& access;
    Origin origin;
    Reference proxy;
    Reference target;
    Reference handler;
    // What an access by key names the property by, under which the target is asked too. Empty: the access names it by a PropertyKeyID.
    Reference key;
    // The string or the symbol that a trap is handed.
    Reference name;
    Reference stored;
    // What the handler was made with under the name of the trap. A guess: whatever is read off the handler is tested against it.
    FunctionID trapThatHandlerWasMadeWith;
};

// The ways through a part of the access. Each begins where a test has passed, or where every test in front of it has failed, and all that come through meet.
class ArmsInKeyOrdersProxies {
    WTF_MAKE_NONCOPYABLE(ArmsInKeyOrdersProxies);
public:
    ArmsInKeyOrdersProxies(Walker& walker, Origin origin)
        : m_walker(walker)
        , m_origin(origin)
        , m_rest(walker.currentBlock())
    {
    }

    // False: nothing comes there.
    bool begin(Reference test, BlockFrequency frequency = BlockFrequency::Normal, BlockFrequency frequencyOfRest = BlockFrequency::Normal)
    {
        if (!standInRest() || !test)
            return false;
        BlockID passed = m_walker.addBlock(frequency);
        m_rest = m_walker.addBlock(frequencyOfRest);
        m_walker.branch(test, passed, m_rest);
        m_walker.continueIn(passed);
        return !m_walker.isUnreachable();
    }

    // It answers with the value under a name that says so. Null: nothing comes there. What the type of the value says is not tested.
    Reference beginWhereValueIs(Reference value, TypeID tested, BlockFrequency frequency = BlockFrequency::Normal, BlockFrequency frequencyOfRest = BlockFrequency::Normal)
    {
        if (!standInRest() || !value)
            return { };
        TypeArena& types = m_walker.types();
        TypeID proven = m_walker.typeOf(value);
        if (types.isSubset(proven, tested)) {
            m_rest = { };
            return value;
        }
        if (!types.overlap(proven, tested))
            return { };

        Draft test(Opcode::TypeTest, m_origin);
        test.setValueOperand(value);
        test.setTestedType(tested);
        Reference isOfType = m_walker.build(WTF::move(test));
        if (!begin(isOfType, frequency, frequencyOfRest))
            return { };
        Draft proof(Opcode::Narrowed, m_origin);
        proof.setValueOperand(value);
        proof.setTestOperand(isOfType);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        return m_walker.build(WTF::move(proof));
    }

    bool beginOtherwise() { return standInRest(); }

    void end(Reference value = { })
    {
        if (!m_walker.isUnreachable() && !m_walker.hasFailed())
            m_ends.append(End { m_walker.currentBlock(), value });
    }

    // What the arms have given, where each that comes through has given something. Where none comes through the walk stands nowhere.
    Reference join()
    {
        if (m_walker.hasFailed())
            return { };
        if (m_ends.size() == 1) {
            m_walker.continueIn(m_ends[0].block);
            return m_ends[0].value;
        }

        BlockID meeting = m_walker.addBlock();
        Reference met;
        bool everyArmGivesValue = !m_ends.isEmpty() && std::ranges::all_of(m_ends, [](const End& end) {
            return !!end.value;
        });
        if (everyArmGivesValue) {
            const Code& code = m_walker.code();
            TypeID type = code.at(m_ends[0].value.id()).type;
            Vector<Representation, 4> forms;
            for (const End& end : m_ends) {
                type = joinOf(m_walker.types(), m_walker.facts().families(), type, code.at(end.value.id()).type);
                forms.append(m_walker.representationOf(end.value));
            }
            met = m_walker.addParameter(meeting, type, m_walker.formOfParameter(type, forms.span()));
        }
        for (const End& end : m_ends) {
            m_walker.continueIn(end.block);
            if (met)
                m_walker.jumpTo(meeting, singleElementSpan(end.value));
            else
                m_walker.jumpTo(meeting);
            if (m_walker.hasFailed())
                return { };
        }
        m_walker.continueIn(meeting);
        return m_walker.isUnreachable() ? Reference() : met;
    }

private:
    struct End {
        BlockID block;
        Reference value;
    };

    bool standInRest()
    {
        if (!m_rest || m_walker.hasFailed())
            return false;
        m_walker.continueIn(m_rest);
        return !m_walker.isUnreachable();
    }

    Walker& m_walker;
    Origin m_origin;
    // Where every test so far has failed. Null: nothing comes there.
    BlockID m_rest;
    Vector<End, 4> m_ends;
};

} // anonymous namespace

static BuiltinName nameOfTrapInKeyOrdersProxies(TrapInKeyOrdersProxies trap)
{
    switch (trap) {
    case TrapInKeyOrdersProxies::Get:
        return BuiltinNames::get;
    case TrapInKeyOrdersProxies::Set:
        return BuiltinNames::set;
    case TrapInKeyOrdersProxies::Has:
        return BuiltinNames::has;
    case TrapInKeyOrdersProxies::DeleteProperty:
        return BuiltinNames::deleteProperty;
    case TrapInKeyOrdersProxies::GetOwnPropertyDescriptor:
        return BuiltinNames::getOwnPropertyDescriptor;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// As ProxyObject spells them.
static ASCIILiteral messageOfTrapThatIsNotCallableInKeyOrdersProxies(TrapInKeyOrdersProxies trap)
{
    switch (trap) {
    case TrapInKeyOrdersProxies::Get:
        return "'get' property of a Proxy's handler should be callable"_s;
    case TrapInKeyOrdersProxies::Set:
        return "'set' property of a Proxy's handler should be callable"_s;
    case TrapInKeyOrdersProxies::Has:
        return "'has' property of a Proxy's handler should be callable"_s;
    case TrapInKeyOrdersProxies::DeleteProperty:
        return "'deleteProperty' property of a Proxy's handler should be callable"_s;
    case TrapInKeyOrdersProxies::GetOwnPropertyDescriptor:
        return "'getOwnPropertyDescriptor' property of a Proxy's handler should be callable"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinID onlyBuiltinNamedByInKeyOrdersProxies(Walker& walker, Reference value)
{
    TypeID proven = value ? walker.typeOf(value) : TypeID();
    if (!proven)
        return { };
    Type type = walker.types().at(proven);
    BuiltinID builtin = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    return builtin ? walker.program().builtins().canonicalID(builtin) : BuiltinID();
}

// A free name has no receiver.
static std::optional<AccessInKeyOrdersProxies> partsOfAccessInKeyOrdersProxies(Walker& walker, const BodyValue& operation)
{
    switch (operation.opcode()) {
    case Opcode::PropGet: {
        auto read = operation.as<Opcode::PropGet>();
        if (read.isFreeName())
            return std::nullopt;
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::Get, read.objectOperand(), read.key(), { }, { }, ECMAMode::Sloppy };
    }
    case Opcode::PropSet: {
        auto store = operation.as<Opcode::PropSet>();
        if (store.isFreeName())
            return std::nullopt;
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::Set, store.objectOperand(), store.key(), { }, store.valueOperand(), store.mode() };
    }
    case Opcode::PropHas: {
        auto question = operation.as<Opcode::PropHas>();
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::Has, question.objectOperand(), question.key(), { }, { }, ECMAMode::Sloppy };
    }
    case Opcode::PropDelete: {
        auto deletion = operation.as<Opcode::PropDelete>();
        if (deletion.isFreeName())
            return std::nullopt;
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::DeleteProperty, deletion.objectOperand(), deletion.key(), { }, { }, deletion.mode() };
    }
    case Opcode::ElemGet: {
        auto read = operation.as<Opcode::ElemGet>();
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::Get, read.objectOperand(), { }, read.propertyOperand(), { }, ECMAMode::Sloppy };
    }
    case Opcode::ElemSet: {
        auto store = operation.as<Opcode::ElemSet>();
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::Set, store.objectOperand(), { }, store.propertyOperand(), store.valueOperand(), store.mode() };
    }
    case Opcode::ElemHas: {
        auto question = operation.as<Opcode::ElemHas>();
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::Has, question.objectOperand(), { }, question.propertyOperand(), { }, ECMAMode::Sloppy };
    }
    case Opcode::ElemDelete: {
        auto deletion = operation.as<Opcode::ElemDelete>();
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::DeleteProperty, deletion.objectOperand(), { }, deletion.propertyOperand(), { }, deletion.mode() };
    }
    case Opcode::Call: {
        // `Object.hasOwn(p, "x")`, by binding. The question has an operation only under a name.
        auto call = operation.as<Opcode::Call>();
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (call.isDoneInPlace() || arguments.size() < 2 || onlyBuiltinNamedByInKeyOrdersProxies(walker, walker.referenceOf(call.calleeOperand())) != Builtins::ObjectHasOwn)
            return std::nullopt;
        Reference name = walker.referenceOf(arguments[1]);
        if (!name || !name.isConstant() || walker.code().at(name.id()).opcode != Opcode::String)
            return std::nullopt;
        return AccessInKeyOrdersProxies { TrapInKeyOrdersProxies::GetOwnPropertyDescriptor, arguments[0], walker.types().textKey(walker.code().at(name.id()).string()), { }, { }, ECMAMode::Sloppy };
    }
    default:
        return std::nullopt;
    }
}

static Reference referenceToRecordInKeyOrdersProxies(const Code& code, ValueID record)
{
    return code.isConstant(record) ? Reference::constant(record) : Reference::value(record);
}

// Only Proxy.revocable hands out the function that empties a proxy. False: something else has made the receiver, or nobody can tell.
static bool findPartsOfProxyMadeHereInKeyOrdersProxies(Walker& walker, Reference receiver, Reference& target, Reference& handler)
{
    if (!receiver || !receiver.isValue())
        return false;
    const Code& code = walker.code();
    const BuiltinTable& builtins = walker.program().builtins();
    const Operation& made = code.at(rootOfCellInSweptCode(code, receiver.id()));
    std::span<const ValueID> arguments;
    if (made.opcode == Opcode::Construct) {
        // With another new.target the proxy is made all the same: ProxyCreate() looks at neither its prototype nor anything else of it.
        if (onlyBuiltinNamedByInKeyOrdersProxies(walker, referenceToRecordInKeyOrdersProxies(code, made.calleeOperand())) != Builtins::Proxy)
            return false;
        arguments = made.argumentsOperands();
    } else if (made.opcode == Opcode::CallBuiltin) {
        const BuiltinOverload& overload = builtins.at(made.overload());
        if (overload.mode != BuiltinCallMode::Construct || made.builtinCallForm() != BuiltinCallForm::Whole || builtins.canonicalID(overload.builtin) != Builtins::Proxy)
            return false;
        arguments = made.argumentsOperands();
    } else
        return false;
    if (arguments.size() < 2)
        return false;

    // `new Proxy()` has thrown for whatever is no object. Where the type does not say so, the operations that take an object cannot be handed it.
    target = referenceToRecordInKeyOrdersProxies(code, arguments[0]);
    handler = referenceToRecordInKeyOrdersProxies(code, arguments[1]);
    return walker.types().at(walker.typeOf(target)).isObject() && walker.types().at(walker.typeOf(handler)).isObject();
}

static FunctionID findTrapThatHandlerWasMadeWithInKeyOrdersProxies(Walker& walker, Reference handler, PropertyKeyID nameOfTrap)
{
    if (!handler.isValue())
        return { };
    const Code& code = walker.code();
    const Operation& made = code.at(rootOfCellInSweptCode(code, handler.id()));
    if (made.opcode != Opcode::NewObject || made.listsOnlyStoredValues())
        return { };
    std::optional<FieldReference> field = findField(walker.program(), made.layout(), nameOfTrap);
    auto fields = made.fields();
    std::span<const ValueID> values = made.valuesOperands();
    if (!field || fields.size() != values.size())
        return { };
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index] != field->field)
            continue;
        Type type = walker.types().at(code.at(values[index]).type);
        FunctionID function = type.isObject() ? singleFunctionOf(walker.types(), type) : FunctionID();
        return function && walker.facts().linked().mayBeMentionedBy(walker.unit().function, function) ? function : FunctionID();
    }
    return { };
}

// The state still knows every key of the handler and what lies under this one: nothing has run since that could have changed either. Null: the trap is read by name.
static Reference trapInSightInKeyOrdersProxies(Walker& walker, Reference handler, PropertyKeyID nameOfTrap)
{
    if (!walker.has(Mechanism::Folding) || !handler.isValue())
        return { };
    std::optional<OwnKeyOrder> keys = ownKeysInSight(walker, handler);
    std::optional<FieldReference> field = keys && keys->layout ? findField(walker.program(), keys->layout, nameOfTrap) : std::nullopt;
    if (!field || field->layout != keys->layout || !keys->fields.contains(field->field))
        return { };
    Reference names[] { handler, Reference::value(rootOfCellInSweptCode(walker.code(), handler.id())) };
    for (Reference name : names) {
        Reference held = walker.state().slot(name, *field);
        TypeID type = held ? walker.typeOf(held) : TypeID();
        if (type && !(walker.types().at(type).primitives & (Primitive::Absent | Primitive::Unspecified)))
            return held;
    }
    return { };
}

static Reference booleanInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, bool value)
{
    Draft constant(Opcode::Boolean, inProgress.origin);
    constant.setBoolean(value);
    return inProgress.walker.constant(WTF::move(constant));
}

static void buildThrowOfTypeErrorInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, ASCIILiteral message)
{
    Draft error(Opcode::ThrowNewError, inProgress.origin);
    error.setErrorKind(ErrorKind::TypeError);
    error.setMessage(inProgress.walker.types().internText(asBytes(message.span())));
    inProgress.walker.build(WTF::move(error));
}

// The receiver is a proxy all the same, so no form has anything to say: the run time goes through it.
static Reference buildAccessAsWrittenInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress)
{
    Walker& walker = inProgress.walker;
    Draft written = walker.draftOf(inProgress.operation);
    if (walker.hasFailed())
        return { };
    written.resultIsReturnedAtOnce = false;
    written.isTailCall = false;
    walker.chooseConservativeFields(written);
    return walker.build(WTF::move(written));
}

static Reference buildTestOfTargetInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, Opcode test)
{
    Draft mayBind(test, inProgress.origin);
    mayBind.setTargetOperand(inProgress.target);
    return inProgress.walker.build(WTF::move(mayBind));
}

// `operation` takes the target and the name, and the answer of the trap where it has one.
static Reference buildLookAtTargetInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, Opcode operation, Reference answer = { })
{
    Draft look(operation, inProgress.origin);
    look.setTargetOperand(inProgress.target);
    look.setPropertyOperand(inProgress.name);
    if (answer)
        look.setAnswerOperand(answer);
    return inProgress.walker.build(WTF::move(look));
}

static void buildLookAtTargetWhereItMayBindInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, Opcode test, Opcode operation, Reference answer = { })
{
    Reference mayBind = buildTestOfTargetInKeyOrdersProxies(inProgress, test);
    ArmsInKeyOrdersProxies arms(inProgress.walker, inProgress.origin);
    if (arms.begin(mayBind, BlockFrequency::Rare)) {
        buildLookAtTargetInKeyOrdersProxies(inProgress, operation, answer);
        arms.end();
    }
    if (arms.beginOtherwise())
        arms.end();
    arms.join();
}

static void buildFinishOfStoreByTrapInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, Reference answer)
{
    Draft finish(Opcode::FinishStoreByTrap, inProgress.origin);
    finish.setMode(inProgress.access.mode);
    finish.setTargetOperand(inProgress.target);
    finish.setPropertyOperand(inProgress.name);
    finish.setValueOperand(inProgress.stored);
    finish.setAnswerOperand(answer);
    inProgress.walker.build(WTF::move(finish));
}

static Reference buildTruthOfAnswerInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, Reference answer)
{
    Draft truth(Opcode::ToBoolean, inProgress.origin);
    truth.setValueOperand(answer);
    inProgress.walker.chooseConservativeFields(truth);
    return inProgress.walker.build(WTF::move(truth));
}

// Behind the call of the trap: what the engine makes of the answer, and what it holds the answer against.
static Reference buildAnswerOfAccessInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, Reference answer)
{
    Walker& walker = inProgress.walker;
    Origin origin = inProgress.origin;
    switch (inProgress.access.trap) {
    case TrapInKeyOrdersProxies::Get:
        buildLookAtTargetWhereItMayBindInKeyOrdersProxies(inProgress, Opcode::MayBindAnswersOfGetAndSetTraps, Opcode::ValidateAnswerOfGetTrap, answer);
        return walker.isUnreachable() ? Reference() : answer;
    case TrapInKeyOrdersProxies::Set: {
        Reference consents = buildTruthOfAnswerInKeyOrdersProxies(inProgress, answer);
        ArmsInKeyOrdersProxies arms(walker, origin);
        if (arms.begin(consents, BlockFrequency::Normal, BlockFrequency::Rare)) {
            Reference mayBind = buildTestOfTargetInKeyOrdersProxies(inProgress, Opcode::MayBindAnswersOfGetAndSetTraps);
            ArmsInKeyOrdersProxies armsOfConsent(walker, origin);
            if (armsOfConsent.begin(mayBind, BlockFrequency::Rare)) {
                buildFinishOfStoreByTrapInKeyOrdersProxies(inProgress, answer);
                armsOfConsent.end();
            }
            if (armsOfConsent.beginOtherwise())
                armsOfConsent.end();
            armsOfConsent.join();
            arms.end();
        }
        if (arms.beginOtherwise()) {
            buildFinishOfStoreByTrapInKeyOrdersProxies(inProgress, answer);
            arms.end();
        }
        return arms.join();
    }
    case TrapInKeyOrdersProxies::Has: {
        Reference saysYes = buildTruthOfAnswerInKeyOrdersProxies(inProgress, answer);
        ArmsInKeyOrdersProxies arms(walker, origin);
        if (arms.begin(saysYes))
            arms.end(booleanInKeyOrdersProxies(inProgress, true));
        if (arms.beginOtherwise()) {
            buildLookAtTargetWhereItMayBindInKeyOrdersProxies(inProgress, Opcode::MayBindAnswersOfHasTraps, Opcode::ValidateDenialOfHasTrap);
            arms.end(booleanInKeyOrdersProxies(inProgress, false));
        }
        return arms.join();
    }
    case TrapInKeyOrdersProxies::DeleteProperty: {
        bool isStrict = inProgress.access.mode == ECMAMode::Strict;
        Reference consents = buildTruthOfAnswerInKeyOrdersProxies(inProgress, answer);
        ArmsInKeyOrdersProxies arms(walker, origin);
        if (arms.begin(consents, BlockFrequency::Normal, isStrict ? BlockFrequency::Rare : BlockFrequency::Normal)) {
            buildLookAtTargetWhereItMayBindInKeyOrdersProxies(inProgress, Opcode::MayBindAnswersOfHasTraps, Opcode::ValidateConsentOfDeleteTrap);
            arms.end(booleanInKeyOrdersProxies(inProgress, true));
        }
        if (arms.beginOtherwise()) {
            if (isStrict)
                buildThrowOfTypeErrorInKeyOrdersProxies(inProgress, UnableToDeletePropertyError);
            arms.end(booleanInKeyOrdersProxies(inProgress, false));
        }
        return arms.join();
    }
    case TrapInKeyOrdersProxies::GetOwnPropertyDescriptor: {
        ArmsInKeyOrdersProxies arms(walker, origin);
        if (arms.beginWhereValueIs(answer, TypeTable::undefined)) {
            Reference mayBind = buildTestOfTargetInKeyOrdersProxies(inProgress, Opcode::MayBindAnswersOfHasTraps);
            ArmsInKeyOrdersProxies armsOfDenial(walker, origin);
            if (armsOfDenial.begin(mayBind, BlockFrequency::Rare))
                armsOfDenial.end(buildLookAtTargetInKeyOrdersProxies(inProgress, Opcode::HasOwnByAnswerOfDescriptorTrap, answer));
            if (armsOfDenial.beginOtherwise())
                armsOfDenial.end(booleanInKeyOrdersProxies(inProgress, false));
            arms.end(armsOfDenial.join());
        }
        if (arms.beginOtherwise())
            arms.end(buildLookAtTargetInKeyOrdersProxies(inProgress, Opcode::HasOwnByAnswerOfDescriptorTrap, answer));
        return arms.join();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The trap is called like any function: where its type names one, a copy of it or a direct call.
static Reference buildCallOfTrapInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress, Reference trap)
{
    Walker& walker = inProgress.walker;
    Vector<Reference, 4> arguments { inProgress.target, inProgress.name };
    switch (inProgress.access.trap) {
    case TrapInKeyOrdersProxies::Get:
        arguments.append(inProgress.proxy);
        break;
    case TrapInKeyOrdersProxies::Set:
        arguments.append(inProgress.stored);
        arguments.append(inProgress.proxy);
        break;
    case TrapInKeyOrdersProxies::Has:
    case TrapInKeyOrdersProxies::DeleteProperty:
    case TrapInKeyOrdersProxies::GetOwnPropertyDescriptor:
        break;
    }
    Draft call(Opcode::Call, inProgress.origin);
    call.setCalleeOperand(trap);
    call.setThisValueOperand(inProgress.handler);
    call.setArgumentsOperands(arguments.span());
    if (callForwardedDirectly(walker, inProgress.operation, call))
        return walker.isUnreachable() ? Reference() : walker.referenceOf(inProgress.operation.id());
    return walker.build(WTF::move(call));
}

// The handler has no such trap: the target is asked, for the proxy where the question has a receiver.
static Reference buildAccessOnTargetInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress)
{
    Walker& walker = inProgress.walker;
    const AccessInKeyOrdersProxies& access = inProgress.access;
    switch (access.trap) {
    case TrapInKeyOrdersProxies::Get:
    case TrapInKeyOrdersProxies::Set: {
        bool isStore = access.trap == TrapInKeyOrdersProxies::Set;
        Draft onTarget(isStore ? Opcode::SuperSet : Opcode::SuperGet, inProgress.origin);
        onTarget.setBaseOperand(inProgress.target);
        onTarget.setReceiverOperand(inProgress.proxy);
        if (isStore) {
            onTarget.setMode(access.mode);
            onTarget.setValueOperand(inProgress.stored);
        }
        if (inProgress.key)
            onTarget.setComputedKeyOperand(inProgress.key);
        else
            onTarget.setKey(access.key);
        return walker.build(WTF::move(onTarget));
    }
    case TrapInKeyOrdersProxies::Has:
    case TrapInKeyOrdersProxies::DeleteProperty: {
        bool isDeletion = access.trap == TrapInKeyOrdersProxies::DeleteProperty;
        Opcode byName = isDeletion ? Opcode::PropDelete : Opcode::PropHas;
        Opcode byKey = isDeletion ? Opcode::ElemDelete : Opcode::ElemHas;
        Draft onTarget(inProgress.key ? byKey : byName, inProgress.origin);
        onTarget.setObjectOperand(inProgress.target);
        if (isDeletion)
            onTarget.setMode(access.mode);
        if (inProgress.key)
            onTarget.setPropertyOperand(inProgress.key);
        else
            onTarget.setKey(access.key);
        return walker.build(WTF::move(onTarget));
    }
    case TrapInKeyOrdersProxies::GetOwnPropertyDescriptor: {
        Draft onTarget(Opcode::PropHasOwn, inProgress.origin);
        onTarget.setObjectOperand(inProgress.target);
        onTarget.setKey(access.key);
        onTarget.setCalledBuiltin(Builtins::ObjectHasOwn);
        return walker.build(WTF::move(onTarget));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The name is at hand.
static Reference buildAccessThroughHandlerInKeyOrdersProxies(const ProxyAccessInKeyOrdersProxies& inProgress)
{
    Walker& walker = inProgress.walker;
    Origin origin = inProgress.origin;
    PropertyKeyID nameOfTrap = walker.program().keyOf(nameOfTrapInKeyOrdersProxies(inProgress.access.trap));
    Reference trap = trapInSightInKeyOrdersProxies(walker, inProgress.handler, nameOfTrap);
    if (!trap) {
        Draft readOfTrap(Opcode::PropGet, origin);
        readOfTrap.setObjectOperand(inProgress.handler);
        readOfTrap.setKey(nameOfTrap);
        trap = walker.build(WTF::move(readOfTrap));
    }
    if (!trap)
        return { };

    ArmsInKeyOrdersProxies arms(walker, origin);
    if (Reference callable = arms.beginWhereValueIs(trap, TypeTable::function)) {
        // Whichever function it is, the answer is held against the target once.
        ArmsInKeyOrdersProxies armsOfCall(walker, origin);
        if (inProgress.trapThatHandlerWasMadeWith) {
            if (Reference closure = armsOfCall.beginWhereValueIs(callable, walker.types().forFunction(inProgress.trapThatHandlerWasMadeWith), BlockFrequency::Normal, BlockFrequency::Unlikely))
                armsOfCall.end(buildCallOfTrapInKeyOrdersProxies(inProgress, closure));
        }
        if (armsOfCall.beginOtherwise())
            armsOfCall.end(buildCallOfTrapInKeyOrdersProxies(inProgress, callable));
        Reference answer = armsOfCall.join();
        arms.end(answer ? buildAnswerOfAccessInKeyOrdersProxies(inProgress, answer) : Reference());
    }
    if (arms.beginWhereValueIs(trap, TypeTable::nullish, BlockFrequency::Normal, BlockFrequency::Rare))
        arms.end(buildAccessOnTargetInKeyOrdersProxies(inProgress));
    if (arms.beginOtherwise())
        buildThrowOfTypeErrorInKeyOrdersProxies(inProgress, messageOfTrapThatIsNotCallableInKeyOrdersProxies(inProgress.access.trap));
    return arms.join();
}

static Reference buildAccessOfProxyInKeyOrdersProxies(ProxyAccessInKeyOrdersProxies inProgress)
{
    Walker& walker = inProgress.walker;
    if (!inProgress.key) {
        Draft name(Opcode::String, inProgress.origin);
        name.setString(walker.types().at(inProgress.access.key).text);
        inProgress.name = walker.constant(WTF::move(name));
        return buildAccessThroughHandlerInKeyOrdersProxies(inProgress);
    }

    ArmsInKeyOrdersProxies arms(walker, inProgress.origin);
    if (Reference key = arms.beginWhereValueIs(inProgress.key, keysHandedToTrapsWithoutCodeInKeyOrdersProxies, BlockFrequency::Normal, BlockFrequency::Rare)) {
        ProxyAccessInKeyOrdersProxies underName = inProgress;
        underName.key = key;
        underName.name = key;
        if (walker.types().at(walker.typeOf(key)).primitives & Primitive::Integer) {
            Draft toName(Opcode::ToPropertyKey, inProgress.origin);
            toName.setValueOperand(key);
            underName.name = walker.build(WTF::move(toName));
        }
        arms.end(underName.name ? buildAccessThroughHandlerInKeyOrdersProxies(underName) : Reference());
    }
    if (arms.beginOtherwise())
        arms.end(buildAccessAsWrittenInKeyOrdersProxies(inProgress));
    return arms.join();
}

bool buildAccessThroughProxyInSight(Walker& walker, const BodyValue& operation)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::DirectCalls))
        return false;
    std::optional<AccessInKeyOrdersProxies> access = partsOfAccessInKeyOrdersProxies(walker, operation);
    if (!access)
        return false;
    Reference target;
    Reference handler;
    Reference proxy = walker.referenceOf(access->object);
    if (!findPartsOfProxyMadeHereInKeyOrdersProxies(walker, proxy, target, handler))
        return false;

    TypeArena& types = walker.types();
    Reference key;
    if (access->property) {
        key = walker.referenceOf(access->property);
        if (!key || !types.overlap(walker.typeOf(key), keysHandedToTrapsWithoutCodeInKeyOrdersProxies))
            return false;
    } else if (!access->key || liesInThread(access->key) || types.at(access->key).kind != PropertyKeyKind::Text)
        return false;
    Reference stored = access->stored ? walker.referenceOf(access->stored) : Reference();
    if (access->stored && !stored)
        return false;

    PropertyKeyID nameOfTrap = walker.program().keyOf(nameOfTrapInKeyOrdersProxies(access->trap));
    if (!nameOfTrap)
        return false;
    // `get(target, key, receiver) { return receiver.fallback }`: the trap that is copied for the access has come to an access through the same proxy. No count of calls as written knows of that circle.
    FunctionID trapThatHandlerWasMadeWith = findTrapThatHandlerWasMadeWithInKeyOrdersProxies(walker, handler, nameOfTrap);
    if (trapThatHandlerWasMadeWith && walker.isBeingWalked(trapThatHandlerWasMadeWith))
        return false;

    Origin origin = walker.originOf(operation);
    origin.isTailCall = false;
    ProxyAccessInKeyOrdersProxies inProgress { walker, operation, *access, origin, proxy, target, handler, key, { }, stored, trapThatHandlerWasMadeWith };
    Reference answer = buildAccessOfProxyInKeyOrdersProxies(inProgress);
    if (answer && access->trap != TrapInKeyOrdersProxies::Set)
        walker.bind(operation.id(), answer);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
