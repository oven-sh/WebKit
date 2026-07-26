// GENERATED FILE — do not edit. Regenerate with:
//   bun src/codegen/generate-primordials.ts --bun=<bun> --webkit=<WebKit>
// (src/codegen in oven-sh/bun). Derived from Node.js's primordials construction
// applied to this engine's intrinsics; see JSCPrimordials.h for the mechanism.

#pragma once

#if USE(BUN_JSC_ADDITIONS)

namespace JSC {

// V(name, key, kind): key is PROP("string"), SYM(wellKnownSymbolName), or SELF
// (the holder object itself); kind is Method | Getter | Setter | Value | Self.

#define JSC_FOREACH_PRIMORDIAL_GlobalObject(V) \
    V(globalThis,         PROP("globalThis"),         Value) \
    V(decodeURI,          PROP("decodeURI"),          Method) \
    V(decodeURIComponent, PROP("decodeURIComponent"), Method) \
    V(encodeURI,          PROP("encodeURI"),          Method) \
    V(encodeURIComponent, PROP("encodeURIComponent"), Method) \
    V(escape,             PROP("escape"),             Method) \
    V(eval,               PROP("eval"),               Method) \
    V(unescape,           PROP("unescape"),           Method) \

#define JSC_FOREACH_PRIMORDIAL_ObjectPrototype(V) \
    V(ObjectPrototype,                     SELF,                         Self) \
    V(ObjectPrototypeToString,             PROP("toString"),             Method) \
    V(ObjectPrototypeToLocaleString,       PROP("toLocaleString"),       Method) \
    V(ObjectPrototypeValueOf,              PROP("valueOf"),              Method) \
    V(ObjectPrototypeHasOwnProperty,       PROP("hasOwnProperty"),       Method) \
    V(ObjectPrototypePropertyIsEnumerable, PROP("propertyIsEnumerable"), Method) \
    V(ObjectPrototypeIsPrototypeOf,        PROP("isPrototypeOf"),        Method) \
    V(ObjectPrototype__defineGetter__,     PROP("__defineGetter__"),     Method) \
    V(ObjectPrototype__defineSetter__,     PROP("__defineSetter__"),     Method) \
    V(ObjectPrototype__lookupGetter__,     PROP("__lookupGetter__"),     Method) \
    V(ObjectPrototype__lookupSetter__,     PROP("__lookupSetter__"),     Method) \
    V(ObjectPrototypeGet__proto__,         PROP("__proto__"),            Getter) \
    V(ObjectPrototypeSet__proto__,         PROP("__proto__"),            Setter) \
    V(ObjectPrototypeConstructor,          PROP("constructor"),          Method) \

#define JSC_FOREACH_PRIMORDIAL_ObjectConstructor(V) \
    V(ObjectGetPrototypeOf,            PROP("getPrototypeOf"),            Method) \
    V(ObjectSetPrototypeOf,            PROP("setPrototypeOf"),            Method) \
    V(ObjectGetOwnPropertyDescriptor,  PROP("getOwnPropertyDescriptor"),  Method) \
    V(ObjectGetOwnPropertyDescriptors, PROP("getOwnPropertyDescriptors"), Method) \
    V(ObjectGetOwnPropertyNames,       PROP("getOwnPropertyNames"),       Method) \
    V(ObjectGetOwnPropertySymbols,     PROP("getOwnPropertySymbols"),     Method) \
    V(ObjectKeys,                      PROP("keys"),                      Method) \
    V(ObjectDefineProperty,            PROP("defineProperty"),            Method) \
    V(ObjectDefineProperties,          PROP("defineProperties"),          Method) \
    V(ObjectCreate,                    PROP("create"),                    Method) \
    V(ObjectSeal,                      PROP("seal"),                      Method) \
    V(ObjectFreeze,                    PROP("freeze"),                    Method) \
    V(ObjectPreventExtensions,         PROP("preventExtensions"),         Method) \
    V(ObjectIsSealed,                  PROP("isSealed"),                  Method) \
    V(ObjectIsFrozen,                  PROP("isFrozen"),                  Method) \
    V(ObjectIsExtensible,              PROP("isExtensible"),              Method) \
    V(ObjectIs,                        PROP("is"),                        Method) \
    V(ObjectAssign,                    PROP("assign"),                    Method) \
    V(ObjectValues,                    PROP("values"),                    Method) \
    V(ObjectEntries,                   PROP("entries"),                   Method) \
    V(ObjectFromEntries,               PROP("fromEntries"),               Method) \
    V(ObjectHasOwn,                    PROP("hasOwn"),                    Method) \
    V(ObjectGroupBy,                   PROP("groupBy"),                   Method) \

#define JSC_FOREACH_PRIMORDIAL_FunctionPrototype(V) \
    V(FunctionPrototypeToString,          PROP("toString"),    Method) \
    V(FunctionPrototypeApply,             PROP("apply"),       Method) \
    V(FunctionPrototypeCall,              PROP("call"),        Method) \
    V(FunctionPrototypeBind,              PROP("bind"),        Method) \
    V(FunctionPrototypeGetArguments,      PROP("arguments"),   Getter) \
    V(FunctionPrototypeSetArguments,      PROP("arguments"),   Setter) \
    V(FunctionPrototypeGetCaller,         PROP("caller"),      Getter) \
    V(FunctionPrototypeSetCaller,         PROP("caller"),      Setter) \
    V(FunctionPrototypeConstructor,       PROP("constructor"), Method) \
    V(FunctionPrototypeSymbolHasInstance, SYM(hasInstance),    Method) \

#define JSC_FOREACH_PRIMORDIAL_FunctionConstructor(V) \
    V(Function,          SELF,              Self) \
    V(FunctionPrototype, PROP("prototype"), Method) \

#define JSC_FOREACH_PRIMORDIAL_ArrayPrototype(V) \
    V(ArrayPrototype,                  SELF,                   Self) \
    V(ArrayPrototypeToString,          PROP("toString"),       Method) \
    V(ArrayPrototypeValues,            PROP("values"),         Method) \
    V(ArrayPrototypeToLocaleString,    PROP("toLocaleString"), Method) \
    V(ArrayPrototypeConcat,            PROP("concat"),         Method) \
    V(ArrayPrototypeFill,              PROP("fill"),           Method) \
    V(ArrayPrototypeJoin,              PROP("join"),           Method) \
    V(ArrayPrototypePop,               PROP("pop"),            Method) \
    V(ArrayPrototypePush,              PROP("push"),           Method) \
    V(ArrayPrototypeReverse,           PROP("reverse"),        Method) \
    V(ArrayPrototypeShift,             PROP("shift"),          Method) \
    V(ArrayPrototypeSlice,             PROP("slice"),          Method) \
    V(ArrayPrototypeSort,              PROP("sort"),           Method) \
    V(ArrayPrototypeSplice,            PROP("splice"),         Method) \
    V(ArrayPrototypeUnshift,           PROP("unshift"),        Method) \
    V(ArrayPrototypeEvery,             PROP("every"),          Method) \
    V(ArrayPrototypeForEach,           PROP("forEach"),        Method) \
    V(ArrayPrototypeSome,              PROP("some"),           Method) \
    V(ArrayPrototypeIndexOf,           PROP("indexOf"),        Method) \
    V(ArrayPrototypeLastIndexOf,       PROP("lastIndexOf"),    Method) \
    V(ArrayPrototypeFilter,            PROP("filter"),         Method) \
    V(ArrayPrototypeFlat,              PROP("flat"),           Method) \
    V(ArrayPrototypeFlatMap,           PROP("flatMap"),        Method) \
    V(ArrayPrototypeReduce,            PROP("reduce"),         Method) \
    V(ArrayPrototypeReduceRight,       PROP("reduceRight"),    Method) \
    V(ArrayPrototypeMap,               PROP("map"),            Method) \
    V(ArrayPrototypeKeys,              PROP("keys"),           Method) \
    V(ArrayPrototypeEntries,           PROP("entries"),        Method) \
    V(ArrayPrototypeFind,              PROP("find"),           Method) \
    V(ArrayPrototypeFindLast,          PROP("findLast"),       Method) \
    V(ArrayPrototypeFindIndex,         PROP("findIndex"),      Method) \
    V(ArrayPrototypeFindLastIndex,     PROP("findLastIndex"),  Method) \
    V(ArrayPrototypeIncludes,          PROP("includes"),       Method) \
    V(ArrayPrototypeCopyWithin,        PROP("copyWithin"),     Method) \
    V(ArrayPrototypeAt,                PROP("at"),             Method) \
    V(ArrayPrototypeToReversed,        PROP("toReversed"),     Method) \
    V(ArrayPrototypeToSorted,          PROP("toSorted"),       Method) \
    V(ArrayPrototypeToSpliced,         PROP("toSpliced"),      Method) \
    V(ArrayPrototypeWith,              PROP("with"),           Method) \
    V(ArrayPrototypeConstructor,       PROP("constructor"),    Method) \
    V(ArrayPrototypeSymbolIterator,    SYM(iterator),          Method) \
    V(ArrayPrototypeSymbolUnscopables, SYM(unscopables),       Value) \

#define JSC_FOREACH_PRIMORDIAL_ArrayConstructor(V) \
    V(ArrayFrom,             PROP("from"),      Method) \
    V(ArrayOf,               PROP("of"),        Method) \
    V(ArrayIsArray,          PROP("isArray"),   Method) \
    V(ArrayFromAsync,        PROP("fromAsync"), Method) \
    V(ArrayGetSymbolSpecies, SYM(species),      Getter) \

#define JSC_FOREACH_PRIMORDIAL_StringPrototype(V) \
    V(StringPrototype,                  SELF,                      Self) \
    V(StringPrototypeAnchor,            PROP("anchor"),            Method) \
    V(StringPrototypeBig,               PROP("big"),               Method) \
    V(StringPrototypeBold,              PROP("bold"),              Method) \
    V(StringPrototypeBlink,             PROP("blink"),             Method) \
    V(StringPrototypeFixed,             PROP("fixed"),             Method) \
    V(StringPrototypeFontcolor,         PROP("fontcolor"),         Method) \
    V(StringPrototypeFontsize,          PROP("fontsize"),          Method) \
    V(StringPrototypeItalics,           PROP("italics"),           Method) \
    V(StringPrototypeLink,              PROP("link"),              Method) \
    V(StringPrototypeSmall,             PROP("small"),             Method) \
    V(StringPrototypeStrike,            PROP("strike"),            Method) \
    V(StringPrototypeSub,               PROP("sub"),               Method) \
    V(StringPrototypeSup,               PROP("sup"),               Method) \
    V(StringPrototypeToString,          PROP("toString"),          Method) \
    V(StringPrototypeValueOf,           PROP("valueOf"),           Method) \
    V(StringPrototypeCharAt,            PROP("charAt"),            Method) \
    V(StringPrototypeCharCodeAt,        PROP("charCodeAt"),        Method) \
    V(StringPrototypeCodePointAt,       PROP("codePointAt"),       Method) \
    V(StringPrototypeConcat,            PROP("concat"),            Method) \
    V(StringPrototypeIndexOf,           PROP("indexOf"),           Method) \
    V(StringPrototypeLastIndexOf,       PROP("lastIndexOf"),       Method) \
    V(StringPrototypeReplace,           PROP("replace"),           Method) \
    V(StringPrototypeReplaceAll,        PROP("replaceAll"),        Method) \
    V(StringPrototypeRepeat,            PROP("repeat"),            Method) \
    V(StringPrototypePadStart,          PROP("padStart"),          Method) \
    V(StringPrototypePadEnd,            PROP("padEnd"),            Method) \
    V(StringPrototypeSlice,             PROP("slice"),             Method) \
    V(StringPrototypeSubstr,            PROP("substr"),            Method) \
    V(StringPrototypeAt,                PROP("at"),                Method) \
    V(StringPrototypeSubstring,         PROP("substring"),         Method) \
    V(StringPrototypeToLowerCase,       PROP("toLowerCase"),       Method) \
    V(StringPrototypeToUpperCase,       PROP("toUpperCase"),       Method) \
    V(StringPrototypeLocaleCompare,     PROP("localeCompare"),     Method) \
    V(StringPrototypeToLocaleLowerCase, PROP("toLocaleLowerCase"), Method) \
    V(StringPrototypeToLocaleUpperCase, PROP("toLocaleUpperCase"), Method) \
    V(StringPrototypeTrim,              PROP("trim"),              Method) \
    V(StringPrototypeStartsWith,        PROP("startsWith"),        Method) \
    V(StringPrototypeEndsWith,          PROP("endsWith"),          Method) \
    V(StringPrototypeIncludes,          PROP("includes"),          Method) \
    V(StringPrototypeMatch,             PROP("match"),             Method) \
    V(StringPrototypeSearch,            PROP("search"),            Method) \
    V(StringPrototypeMatchAll,          PROP("matchAll"),          Method) \
    V(StringPrototypeSplit,             PROP("split"),             Method) \
    V(StringPrototypeNormalize,         PROP("normalize"),         Method) \
    V(StringPrototypeTrimStart,         PROP("trimStart"),         Method) \
    V(StringPrototypeTrimLeft,          PROP("trimLeft"),          Method) \
    V(StringPrototypeTrimEnd,           PROP("trimEnd"),           Method) \
    V(StringPrototypeTrimRight,         PROP("trimRight"),         Method) \
    V(StringPrototypeIsWellFormed,      PROP("isWellFormed"),      Method) \
    V(StringPrototypeToWellFormed,      PROP("toWellFormed"),      Method) \
    V(StringPrototypeConstructor,       PROP("constructor"),       Method) \
    V(StringPrototypeSymbolIterator,    SYM(iterator),             Method) \

#define JSC_FOREACH_PRIMORDIAL_StringConstructor(V) \
    V(StringFromCharCode,  PROP("fromCharCode"),  Method) \
    V(StringFromCodePoint, PROP("fromCodePoint"), Method) \
    V(StringRaw,           PROP("raw"),           Method) \

#define JSC_FOREACH_PRIMORDIAL_RegExpPrototype(V) \
    V(RegExpPrototype,               SELF,                Self) \
    V(RegExpPrototypeCompile,        PROP("compile"),     Method) \
    V(RegExpPrototypeExec,           PROP("exec"),        Method) \
    V(RegExpPrototypeToString,       PROP("toString"),    Method) \
    V(RegExpPrototypeGetGlobal,      PROP("global"),      Getter) \
    V(RegExpPrototypeGetDotAll,      PROP("dotAll"),      Getter) \
    V(RegExpPrototypeGetHasIndices,  PROP("hasIndices"),  Getter) \
    V(RegExpPrototypeGetIgnoreCase,  PROP("ignoreCase"),  Getter) \
    V(RegExpPrototypeGetMultiline,   PROP("multiline"),   Getter) \
    V(RegExpPrototypeGetSticky,      PROP("sticky"),      Getter) \
    V(RegExpPrototypeGetUnicode,     PROP("unicode"),     Getter) \
    V(RegExpPrototypeGetUnicodeSets, PROP("unicodeSets"), Getter) \
    V(RegExpPrototypeGetSource,      PROP("source"),      Getter) \
    V(RegExpPrototypeGetFlags,       PROP("flags"),       Getter) \
    V(RegExpPrototypeTest,           PROP("test"),        Method) \
    V(RegExpPrototypeConstructor,    PROP("constructor"), Method) \
    V(RegExpPrototypeSymbolMatch,    SYM(match),          Method) \
    V(RegExpPrototypeSymbolMatchAll, SYM(matchAll),       Method) \
    V(RegExpPrototypeSymbolReplace,  SYM(replace),        Method) \
    V(RegExpPrototypeSymbolSearch,   SYM(search),         Method) \
    V(RegExpPrototypeSymbolSplit,    SYM(split),          Method) \

#define JSC_FOREACH_PRIMORDIAL_RegExpConstructor(V) \
    V(RegExpGetInput,            PROP("input"),        Getter) \
    V(RegExpSetInput,            PROP("input"),        Setter) \
    V(RegExpGetDollarUnderscore, PROP("$_"),           Getter) \
    V(RegExpSetDollarUnderscore, PROP("$_"),           Setter) \
    V(RegExpGetMultiline,        PROP("multiline"),    Getter) \
    V(RegExpSetMultiline,        PROP("multiline"),    Setter) \
    V(RegExpGetDollarAsterisk,   PROP("$*"),           Getter) \
    V(RegExpSetDollarAsterisk,   PROP("$*"),           Setter) \
    V(RegExpGetLastMatch,        PROP("lastMatch"),    Getter) \
    V(RegExpGetDollarAmpersand,  PROP("$&"),           Getter) \
    V(RegExpGetLastParen,        PROP("lastParen"),    Getter) \
    V(RegExpGetDollarPlus,       PROP("$+"),           Getter) \
    V(RegExpGetLeftContext,      PROP("leftContext"),  Getter) \
    V(RegExpGetDollarBacktick,   PROP("$`"),           Getter) \
    V(RegExpGetRightContext,     PROP("rightContext"), Getter) \
    V(RegExpGetDollarApostrophe, PROP("$'"),           Getter) \
    V(RegExpGetDollar1,          PROP("$1"),           Getter) \
    V(RegExpGetDollar2,          PROP("$2"),           Getter) \
    V(RegExpGetDollar3,          PROP("$3"),           Getter) \
    V(RegExpGetDollar4,          PROP("$4"),           Getter) \
    V(RegExpGetDollar5,          PROP("$5"),           Getter) \
    V(RegExpGetDollar6,          PROP("$6"),           Getter) \
    V(RegExpGetDollar7,          PROP("$7"),           Getter) \
    V(RegExpGetDollar8,          PROP("$8"),           Getter) \
    V(RegExpGetDollar9,          PROP("$9"),           Getter) \
    V(RegExpEscape,              PROP("escape"),       Method) \
    V(RegExpGetSymbolSpecies,    SYM(species),         Getter) \

#define JSC_FOREACH_PRIMORDIAL_SymbolPrototype(V) \
    V(SymbolPrototype,                  SELF,                Self) \
    V(SymbolPrototypeGetDescription,    PROP("description"), Getter) \
    V(SymbolPrototypeToString,          PROP("toString"),    Method) \
    V(SymbolPrototypeValueOf,           PROP("valueOf"),     Method) \
    V(SymbolPrototypeConstructor,       PROP("constructor"), Method) \
    V(SymbolPrototypeSymbolToPrimitive, SYM(toPrimitive),    Method) \

#define JSC_FOREACH_PRIMORDIAL_SymbolConstructor(V) \
    V(Symbol,                   SELF,                       Self) \
    V(SymbolFor,                PROP("for"),                Method) \
    V(SymbolKeyFor,             PROP("keyFor"),             Method) \
    V(SymbolHasInstance,        PROP("hasInstance"),        Value) \
    V(SymbolIsConcatSpreadable, PROP("isConcatSpreadable"), Value) \
    V(SymbolAsyncIterator,      PROP("asyncIterator"),      Value) \
    V(SymbolIterator,           PROP("iterator"),           Value) \
    V(SymbolMatch,              PROP("match"),              Value) \
    V(SymbolMatchAll,           PROP("matchAll"),           Value) \
    V(SymbolReplace,            PROP("replace"),            Value) \
    V(SymbolSearch,             PROP("search"),             Value) \
    V(SymbolSpecies,            PROP("species"),            Value) \
    V(SymbolSplit,              PROP("split"),              Value) \
    V(SymbolToPrimitive,        PROP("toPrimitive"),        Value) \
    V(SymbolToStringTag,        PROP("toStringTag"),        Value) \
    V(SymbolUnscopables,        PROP("unscopables"),        Value) \
    V(SymbolDispose,            PROP("dispose"),            Value) \
    V(SymbolAsyncDispose,       PROP("asyncDispose"),       Value) \

#define JSC_FOREACH_PRIMORDIAL_BigIntPrototype(V) \
    V(BigIntPrototype,               SELF,                   Self) \
    V(BigIntPrototypeToString,       PROP("toString"),       Method) \
    V(BigIntPrototypeToLocaleString, PROP("toLocaleString"), Method) \
    V(BigIntPrototypeValueOf,        PROP("valueOf"),        Method) \
    V(BigIntPrototypeConstructor,    PROP("constructor"),    Method) \

#define JSC_FOREACH_PRIMORDIAL_BigIntConstructor(V) \
    V(BigInt,        SELF,            Self) \
    V(BigIntAsUintN, PROP("asUintN"), Method) \
    V(BigIntAsIntN,  PROP("asIntN"),  Method) \

#define JSC_FOREACH_PRIMORDIAL_PromisePrototype(V) \
    V(PromisePrototype,            SELF,                Self) \
    V(PromisePrototypeFinally,     PROP("finally"),     Method) \
    V(PromisePrototypeThen,        PROP("then"),        Method) \
    V(PromisePrototypeCatch,       PROP("catch"),       Method) \
    V(PromisePrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_PromiseConstructor(V) \
    V(PromiseResolve,          PROP("resolve"),       Method) \
    V(PromiseReject,           PROP("reject"),        Method) \
    V(PromiseRace,             PROP("race"),          Method) \
    V(PromiseAll,              PROP("all"),           Method) \
    V(PromiseAllSettled,       PROP("allSettled"),    Method) \
    V(PromiseAny,              PROP("any"),           Method) \
    V(PromiseWithResolvers,    PROP("withResolvers"), Method) \
    V(PromiseTry,              PROP("try"),           Method) \
    V(PromiseGetSymbolSpecies, SYM(species),          Getter) \

#define JSC_FOREACH_PRIMORDIAL_IteratorPrototype(V) \
    V(IteratorPrototype,                     SELF,                Self) \
    V(IteratorPrototypeGetConstructor,       PROP("constructor"), Getter) \
    V(IteratorPrototypeSetConstructor,       PROP("constructor"), Setter) \
    V(IteratorPrototypeToArray,              PROP("toArray"),     Method) \
    V(IteratorPrototypeForEach,              PROP("forEach"),     Method) \
    V(IteratorPrototypeSome,                 PROP("some"),        Method) \
    V(IteratorPrototypeEvery,                PROP("every"),       Method) \
    V(IteratorPrototypeFind,                 PROP("find"),        Method) \
    V(IteratorPrototypeReduce,               PROP("reduce"),      Method) \
    V(IteratorPrototypeMap,                  PROP("map"),         Method) \
    V(IteratorPrototypeFilter,               PROP("filter"),      Method) \
    V(IteratorPrototypeTake,                 PROP("take"),        Method) \
    V(IteratorPrototypeDrop,                 PROP("drop"),        Method) \
    V(IteratorPrototypeFlatMap,              PROP("flatMap"),     Method) \
    V(IteratorPrototypeSymbolIterator,       SYM(iterator),       Method) \
    V(IteratorPrototypeGetSymbolToStringTag, SYM(toStringTag),    Getter) \
    V(IteratorPrototypeSetSymbolToStringTag, SYM(toStringTag),    Setter) \
    V(IteratorPrototypeSymbolDispose,        SYM(dispose),        Method) \

#define JSC_FOREACH_PRIMORDIAL_IteratorConstructor(V) \
    V(IteratorFrom,   PROP("from"),   Method) \
    V(IteratorConcat, PROP("concat"), Method) \

#define JSC_FOREACH_PRIMORDIAL_ArrayIteratorPrototype(V) \
    V(ArrayIteratorPrototype,     SELF,         Self) \
    V(ArrayIteratorPrototypeNext, PROP("next"), Method) \

#define JSC_FOREACH_PRIMORDIAL_StringIteratorPrototype(V) \
    V(StringIteratorPrototype,     SELF,         Self) \
    V(StringIteratorPrototypeNext, PROP("next"), Method) \

#define JSC_FOREACH_PRIMORDIAL_MapIteratorPrototype(V) \
    V(MapIteratorPrototype,     SELF,         Self) \
    V(MapIteratorPrototypeNext, PROP("next"), Method) \

#define JSC_FOREACH_PRIMORDIAL_SetIteratorPrototype(V) \
    V(SetIteratorPrototype,     SELF,         Self) \
    V(SetIteratorPrototypeNext, PROP("next"), Method) \

#define JSC_FOREACH_PRIMORDIAL_RegExpStringIteratorPrototype(V) \
    V(RegExpStringIteratorPrototype,     SELF,         Self) \
    V(RegExpStringIteratorPrototypeNext, PROP("next"), Method) \

#define JSC_FOREACH_PRIMORDIAL_IteratorHelperPrototype(V) \
    V(IteratorHelperPrototype,       SELF,           Self) \
    V(IteratorHelperPrototypeNext,   PROP("next"),   Method) \
    V(IteratorHelperPrototypeReturn, PROP("return"), Method) \

#define JSC_FOREACH_PRIMORDIAL_WrapForValidIteratorPrototype(V) \
    V(WrapForValidIteratorPrototype,       SELF,           Self) \
    V(WrapForValidIteratorPrototypeNext,   PROP("next"),   Method) \
    V(WrapForValidIteratorPrototypeReturn, PROP("return"), Method) \

#define JSC_FOREACH_PRIMORDIAL_AsyncIteratorPrototype(V) \
    V(AsyncIteratorPrototype,                    SELF,               Self) \
    V(AsyncIteratorPrototypeSymbolAsyncIterator, SYM(asyncIterator), Method) \
    V(AsyncIteratorPrototypeSymbolAsyncDispose,  SYM(asyncDispose),  Method) \

#define JSC_FOREACH_PRIMORDIAL_GeneratorFunctionPrototype(V) \
    V(GeneratorFunctionPrototype,            SELF,                Self) \
    V(GeneratorFunctionPrototypeConstructor, PROP("constructor"), Method) \
    V(GeneratorFunctionPrototypePrototype,   PROP("prototype"),   Value) \

#define JSC_FOREACH_PRIMORDIAL_AsyncFunctionPrototype(V) \
    V(AsyncFunctionPrototype,            SELF,                Self) \
    V(AsyncFunctionPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_AsyncGeneratorFunctionPrototype(V) \
    V(AsyncGeneratorFunctionPrototype,            SELF,                Self) \
    V(AsyncGeneratorFunctionPrototypeConstructor, PROP("constructor"), Method) \
    V(AsyncGeneratorFunctionPrototypePrototype,   PROP("prototype"),   Value) \

#define JSC_FOREACH_PRIMORDIAL_WeakRefPrototype(V) \
    V(WeakRefPrototype,            SELF,                Self) \
    V(WeakRefPrototypeDeref,       PROP("deref"),       Method) \
    V(WeakRefPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_WeakRefConstructor(V) \
    V(WeakRef, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_FinalizationRegistryPrototype(V) \
    V(FinalizationRegistryPrototype,            SELF,                Self) \
    V(FinalizationRegistryPrototypeRegister,    PROP("register"),    Method) \
    V(FinalizationRegistryPrototypeUnregister,  PROP("unregister"),  Method) \
    V(FinalizationRegistryPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_FinalizationRegistryConstructor(V) \
    V(FinalizationRegistry, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_BooleanPrototype(V) \
    V(BooleanPrototype,            SELF,                Self) \
    V(BooleanPrototypeToString,    PROP("toString"),    Method) \
    V(BooleanPrototypeValueOf,     PROP("valueOf"),     Method) \
    V(BooleanPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_BooleanConstructor(V) \
    V(Boolean, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_NumberPrototype(V) \
    V(NumberPrototype,               SELF,                   Self) \
    V(NumberPrototypeToLocaleString, PROP("toLocaleString"), Method) \
    V(NumberPrototypeValueOf,        PROP("valueOf"),        Method) \
    V(NumberPrototypeToFixed,        PROP("toFixed"),        Method) \
    V(NumberPrototypeToExponential,  PROP("toExponential"),  Method) \
    V(NumberPrototypeToPrecision,    PROP("toPrecision"),    Method) \
    V(NumberPrototypeToString,       PROP("toString"),       Method) \
    V(NumberPrototypeConstructor,    PROP("constructor"),    Method) \

#define JSC_FOREACH_PRIMORDIAL_NumberConstructor(V) \
    V(NumberPrimordial,    SELF,                  Self) \
    V(NumberIsFinite,      PROP("isFinite"),      Method) \
    V(NumberIsNaN,         PROP("isNaN"),         Method) \
    V(NumberIsSafeInteger, PROP("isSafeInteger"), Method) \
    V(NumberParseInt,      PROP("parseInt"),      Method) \
    V(NumberParseFloat,    PROP("parseFloat"),    Method) \
    V(NumberIsInteger,     PROP("isInteger"),     Method) \

#define JSC_FOREACH_PRIMORDIAL_DatePrototype(V) \
    V(DatePrototype,                   SELF,                       Self) \
    V(DatePrototypeToString,           PROP("toString"),           Method) \
    V(DatePrototypeToISOString,        PROP("toISOString"),        Method) \
    V(DatePrototypeToDateString,       PROP("toDateString"),       Method) \
    V(DatePrototypeToTimeString,       PROP("toTimeString"),       Method) \
    V(DatePrototypeToLocaleString,     PROP("toLocaleString"),     Method) \
    V(DatePrototypeToLocaleDateString, PROP("toLocaleDateString"), Method) \
    V(DatePrototypeToLocaleTimeString, PROP("toLocaleTimeString"), Method) \
    V(DatePrototypeValueOf,            PROP("valueOf"),            Method) \
    V(DatePrototypeGetTime,            PROP("getTime"),            Method) \
    V(DatePrototypeGetFullYear,        PROP("getFullYear"),        Method) \
    V(DatePrototypeGetUTCFullYear,     PROP("getUTCFullYear"),     Method) \
    V(DatePrototypeGetMonth,           PROP("getMonth"),           Method) \
    V(DatePrototypeGetUTCMonth,        PROP("getUTCMonth"),        Method) \
    V(DatePrototypeGetDate,            PROP("getDate"),            Method) \
    V(DatePrototypeGetUTCDate,         PROP("getUTCDate"),         Method) \
    V(DatePrototypeGetDay,             PROP("getDay"),             Method) \
    V(DatePrototypeGetUTCDay,          PROP("getUTCDay"),          Method) \
    V(DatePrototypeGetHours,           PROP("getHours"),           Method) \
    V(DatePrototypeGetUTCHours,        PROP("getUTCHours"),        Method) \
    V(DatePrototypeGetMinutes,         PROP("getMinutes"),         Method) \
    V(DatePrototypeGetUTCMinutes,      PROP("getUTCMinutes"),      Method) \
    V(DatePrototypeGetSeconds,         PROP("getSeconds"),         Method) \
    V(DatePrototypeGetUTCSeconds,      PROP("getUTCSeconds"),      Method) \
    V(DatePrototypeGetMilliseconds,    PROP("getMilliseconds"),    Method) \
    V(DatePrototypeGetUTCMilliseconds, PROP("getUTCMilliseconds"), Method) \
    V(DatePrototypeGetTimezoneOffset,  PROP("getTimezoneOffset"),  Method) \
    V(DatePrototypeGetYear,            PROP("getYear"),            Method) \
    V(DatePrototypeSetTime,            PROP("setTime"),            Method) \
    V(DatePrototypeSetMilliseconds,    PROP("setMilliseconds"),    Method) \
    V(DatePrototypeSetUTCMilliseconds, PROP("setUTCMilliseconds"), Method) \
    V(DatePrototypeSetSeconds,         PROP("setSeconds"),         Method) \
    V(DatePrototypeSetUTCSeconds,      PROP("setUTCSeconds"),      Method) \
    V(DatePrototypeSetMinutes,         PROP("setMinutes"),         Method) \
    V(DatePrototypeSetUTCMinutes,      PROP("setUTCMinutes"),      Method) \
    V(DatePrototypeSetHours,           PROP("setHours"),           Method) \
    V(DatePrototypeSetUTCHours,        PROP("setUTCHours"),        Method) \
    V(DatePrototypeSetDate,            PROP("setDate"),            Method) \
    V(DatePrototypeSetUTCDate,         PROP("setUTCDate"),         Method) \
    V(DatePrototypeSetMonth,           PROP("setMonth"),           Method) \
    V(DatePrototypeSetUTCMonth,        PROP("setUTCMonth"),        Method) \
    V(DatePrototypeSetFullYear,        PROP("setFullYear"),        Method) \
    V(DatePrototypeSetUTCFullYear,     PROP("setUTCFullYear"),     Method) \
    V(DatePrototypeSetYear,            PROP("setYear"),            Method) \
    V(DatePrototypeToJSON,             PROP("toJSON"),             Method) \
    V(DatePrototypeToUTCString,        PROP("toUTCString"),        Method) \
    V(DatePrototypeToGMTString,        PROP("toGMTString"),        Method) \
    V(DatePrototypeConstructor,        PROP("constructor"),        Method) \
    V(DatePrototypeSymbolToPrimitive,  SYM(toPrimitive),           Method) \

#define JSC_FOREACH_PRIMORDIAL_DateConstructor(V) \
    V(Date,      SELF,          Self) \
    V(DateParse, PROP("parse"), Method) \
    V(DateUTC,   PROP("UTC"),   Method) \
    V(DateNow,   PROP("now"),   Method) \

#define JSC_FOREACH_PRIMORDIAL_ErrorPrototype(V) \
    V(ErrorPrototype,            SELF,                Self) \
    V(ErrorPrototypeToString,    PROP("toString"),    Method) \
    V(ErrorPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_ErrorConstructor(V) \
    V(Error,                  SELF,                      Self) \
    V(ErrorCaptureStackTrace, PROP("captureStackTrace"), Method) \
    V(ErrorIsError,           PROP("isError"),           Method) \
    V(ErrorAppendStackTrace,  PROP("appendStackTrace"),  Method) \
    V(ErrorPrepareStackTrace, PROP("prepareStackTrace"), Method) \

#define JSC_FOREACH_PRIMORDIAL_MapPrototype(V) \
    V(MapPrototype,                    SELF,                        Self) \
    V(MapPrototypeClear,               PROP("clear"),               Method) \
    V(MapPrototypeDelete,              PROP("delete"),              Method) \
    V(MapPrototypeEntries,             PROP("entries"),             Method) \
    V(MapPrototypeForEach,             PROP("forEach"),             Method) \
    V(MapPrototypeGet,                 PROP("get"),                 Method) \
    V(MapPrototypeHas,                 PROP("has"),                 Method) \
    V(MapPrototypeKeys,                PROP("keys"),                Method) \
    V(MapPrototypeSet,                 PROP("set"),                 Method) \
    V(MapPrototypeGetOrInsert,         PROP("getOrInsert"),         Method) \
    V(MapPrototypeGetOrInsertComputed, PROP("getOrInsertComputed"), Method) \
    V(MapPrototypeGetSize,             PROP("size"),                Getter) \
    V(MapPrototypeValues,              PROP("values"),              Method) \
    V(MapPrototypeConstructor,         PROP("constructor"),         Method) \
    V(MapPrototypeSymbolIterator,      SYM(iterator),               Method) \

#define JSC_FOREACH_PRIMORDIAL_MapConstructor(V) \
    V(MapGroupBy,          PROP("groupBy"), Method) \
    V(MapGetSymbolSpecies, SYM(species),    Getter) \

#define JSC_FOREACH_PRIMORDIAL_SetPrototype(V) \
    V(SetPrototype,                    SELF,                        Self) \
    V(SetPrototypeAdd,                 PROP("add"),                 Method) \
    V(SetPrototypeClear,               PROP("clear"),               Method) \
    V(SetPrototypeDelete,              PROP("delete"),              Method) \
    V(SetPrototypeEntries,             PROP("entries"),             Method) \
    V(SetPrototypeForEach,             PROP("forEach"),             Method) \
    V(SetPrototypeHas,                 PROP("has"),                 Method) \
    V(SetPrototypeKeys,                PROP("keys"),                Method) \
    V(SetPrototypeGetSize,             PROP("size"),                Getter) \
    V(SetPrototypeValues,              PROP("values"),              Method) \
    V(SetPrototypeUnion,               PROP("union"),               Method) \
    V(SetPrototypeIntersection,        PROP("intersection"),        Method) \
    V(SetPrototypeDifference,          PROP("difference"),          Method) \
    V(SetPrototypeSymmetricDifference, PROP("symmetricDifference"), Method) \
    V(SetPrototypeIsSubsetOf,          PROP("isSubsetOf"),          Method) \
    V(SetPrototypeIsSupersetOf,        PROP("isSupersetOf"),        Method) \
    V(SetPrototypeIsDisjointFrom,      PROP("isDisjointFrom"),      Method) \
    V(SetPrototypeConstructor,         PROP("constructor"),         Method) \
    V(SetPrototypeSymbolIterator,      SYM(iterator),               Method) \

#define JSC_FOREACH_PRIMORDIAL_SetConstructor(V) \
    V(SetGetSymbolSpecies, SYM(species), Getter) \

#define JSC_FOREACH_PRIMORDIAL_WeakMapPrototype(V) \
    V(WeakMapPrototype,                    SELF,                        Self) \
    V(WeakMapPrototypeDelete,              PROP("delete"),              Method) \
    V(WeakMapPrototypeGet,                 PROP("get"),                 Method) \
    V(WeakMapPrototypeHas,                 PROP("has"),                 Method) \
    V(WeakMapPrototypeSet,                 PROP("set"),                 Method) \
    V(WeakMapPrototypeGetOrInsert,         PROP("getOrInsert"),         Method) \
    V(WeakMapPrototypeGetOrInsertComputed, PROP("getOrInsertComputed"), Method) \
    V(WeakMapPrototypeConstructor,         PROP("constructor"),         Method) \

#define JSC_FOREACH_PRIMORDIAL_WeakMapConstructor(V) \
    V(WeakMap, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_WeakSetPrototype(V) \
    V(WeakSetPrototype,            SELF,                Self) \
    V(WeakSetPrototypeDelete,      PROP("delete"),      Method) \
    V(WeakSetPrototypeHas,         PROP("has"),         Method) \
    V(WeakSetPrototypeAdd,         PROP("add"),         Method) \
    V(WeakSetPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_WeakSetConstructor(V) \
    V(WeakSet, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_ArrayBufferPrototype(V) \
    V(ArrayBufferPrototype,                      SELF,                          Self) \
    V(ArrayBufferPrototypeSlice,                 PROP("slice"),                 Method) \
    V(ArrayBufferPrototypeGetByteLength,         PROP("byteLength"),            Getter) \
    V(ArrayBufferPrototypeResize,                PROP("resize"),                Method) \
    V(ArrayBufferPrototypeTransfer,              PROP("transfer"),              Method) \
    V(ArrayBufferPrototypeTransferToFixedLength, PROP("transferToFixedLength"), Method) \
    V(ArrayBufferPrototypeGetResizable,          PROP("resizable"),             Getter) \
    V(ArrayBufferPrototypeGetMaxByteLength,      PROP("maxByteLength"),         Getter) \
    V(ArrayBufferPrototypeGetDetached,           PROP("detached"),              Getter) \
    V(ArrayBufferPrototypeConstructor,           PROP("constructor"),           Method) \

#define JSC_FOREACH_PRIMORDIAL_ArrayBufferConstructor(V) \
    V(ArrayBufferPrimordial,       SELF,           Self) \
    V(ArrayBufferIsView,           PROP("isView"), Method) \
    V(ArrayBufferGetSymbolSpecies, SYM(species),   Getter) \

#define JSC_FOREACH_PRIMORDIAL_AggregateErrorPrototype(V) \
    V(AggregateErrorPrototype,            SELF,                Self) \
    V(AggregateErrorPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_AggregateErrorConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_EvalErrorPrototype(V) \
    V(EvalErrorPrototype,            SELF,                Self) \
    V(EvalErrorPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_EvalErrorConstructor(V) \
    V(EvalError, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_RangeErrorPrototype(V) \
    V(RangeErrorPrototype,            SELF,                Self) \
    V(RangeErrorPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_RangeErrorConstructor(V) \
    V(RangeError, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_ReferenceErrorPrototype(V) \
    V(ReferenceErrorPrototype,            SELF,                Self) \
    V(ReferenceErrorPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_ReferenceErrorConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_SyntaxErrorPrototype(V) \
    V(SyntaxErrorPrototype,            SELF,                Self) \
    V(SyntaxErrorPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_SyntaxErrorConstructor(V) \
    V(SyntaxError, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_TypeErrorPrototype(V) \
    V(TypeErrorPrototype,            SELF,                Self) \
    V(TypeErrorPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_TypeErrorConstructor(V) \
    V(TypeError, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_URIErrorPrototype(V) \
    V(URIErrorPrototype,            SELF,                Self) \
    V(URIErrorPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_URIErrorConstructor(V) \
    V(URIError, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_TypedArrayPrototype(V) \
    V(TypedArrayPrototype,                     SELF,                   Self) \
    V(TypedArrayPrototypeToString,             PROP("toString"),       Method) \
    V(TypedArrayPrototypeGetBuffer,            PROP("buffer"),         Getter) \
    V(TypedArrayPrototypeGetByteLength,        PROP("byteLength"),     Getter) \
    V(TypedArrayPrototypeGetByteOffset,        PROP("byteOffset"),     Getter) \
    V(TypedArrayPrototypeCopyWithin,           PROP("copyWithin"),     Method) \
    V(TypedArrayPrototypeSort,                 PROP("sort"),           Method) \
    V(TypedArrayPrototypeEvery,                PROP("every"),          Method) \
    V(TypedArrayPrototypeFilter,               PROP("filter"),         Method) \
    V(TypedArrayPrototypeEntries,              PROP("entries"),        Method) \
    V(TypedArrayPrototypeIncludes,             PROP("includes"),       Method) \
    V(TypedArrayPrototypeFill,                 PROP("fill"),           Method) \
    V(TypedArrayPrototypeFind,                 PROP("find"),           Method) \
    V(TypedArrayPrototypeFindLast,             PROP("findLast"),       Method) \
    V(TypedArrayPrototypeFindIndex,            PROP("findIndex"),      Method) \
    V(TypedArrayPrototypeFindLastIndex,        PROP("findLastIndex"),  Method) \
    V(TypedArrayPrototypeForEach,              PROP("forEach"),        Method) \
    V(TypedArrayPrototypeIndexOf,              PROP("indexOf"),        Method) \
    V(TypedArrayPrototypeJoin,                 PROP("join"),           Method) \
    V(TypedArrayPrototypeKeys,                 PROP("keys"),           Method) \
    V(TypedArrayPrototypeLastIndexOf,          PROP("lastIndexOf"),    Method) \
    V(TypedArrayPrototypeGetLength,            PROP("length"),         Getter) \
    V(TypedArrayPrototypeMap,                  PROP("map"),            Method) \
    V(TypedArrayPrototypeReduce,               PROP("reduce"),         Method) \
    V(TypedArrayPrototypeReduceRight,          PROP("reduceRight"),    Method) \
    V(TypedArrayPrototypeReverse,              PROP("reverse"),        Method) \
    V(TypedArrayPrototypeSet,                  PROP("set"),            Method) \
    V(TypedArrayPrototypeSlice,                PROP("slice"),          Method) \
    V(TypedArrayPrototypeSome,                 PROP("some"),           Method) \
    V(TypedArrayPrototypeSubarray,             PROP("subarray"),       Method) \
    V(TypedArrayPrototypeToLocaleString,       PROP("toLocaleString"), Method) \
    V(TypedArrayPrototypeToReversed,           PROP("toReversed"),     Method) \
    V(TypedArrayPrototypeToSorted,             PROP("toSorted"),       Method) \
    V(TypedArrayPrototypeWith,                 PROP("with"),           Method) \
    V(TypedArrayPrototypeAt,                   PROP("at"),             Method) \
    V(TypedArrayPrototypeValues,               PROP("values"),         Method) \
    V(TypedArrayPrototypeConstructor,          PROP("constructor"),    Method) \
    V(TypedArrayPrototypeGetSymbolToStringTag, SYM(toStringTag),       Getter) \
    V(TypedArrayPrototypeSymbolIterator,       SYM(iterator),          Method) \

#define JSC_FOREACH_PRIMORDIAL_TypedArrayConstructor(V) \
    V(TypedArray,                 SELF,         Self) \
    V(TypedArrayOf,               PROP("of"),   Method) \
    V(TypedArrayFrom,             PROP("from"), Method) \
    V(TypedArrayGetSymbolSpecies, SYM(species), Getter) \

#define JSC_FOREACH_PRIMORDIAL_DataViewPrototype(V) \
    V(DataViewPrototype,              SELF,                 Self) \
    V(DataViewPrototypeGetInt8,       PROP("getInt8"),      Method) \
    V(DataViewPrototypeGetUint8,      PROP("getUint8"),     Method) \
    V(DataViewPrototypeGetInt16,      PROP("getInt16"),     Method) \
    V(DataViewPrototypeGetUint16,     PROP("getUint16"),    Method) \
    V(DataViewPrototypeGetInt32,      PROP("getInt32"),     Method) \
    V(DataViewPrototypeGetUint32,     PROP("getUint32"),    Method) \
    V(DataViewPrototypeGetFloat16,    PROP("getFloat16"),   Method) \
    V(DataViewPrototypeGetFloat32,    PROP("getFloat32"),   Method) \
    V(DataViewPrototypeGetFloat64,    PROP("getFloat64"),   Method) \
    V(DataViewPrototypeGetBigInt64,   PROP("getBigInt64"),  Method) \
    V(DataViewPrototypeGetBigUint64,  PROP("getBigUint64"), Method) \
    V(DataViewPrototypeSetInt8,       PROP("setInt8"),      Method) \
    V(DataViewPrototypeSetUint8,      PROP("setUint8"),     Method) \
    V(DataViewPrototypeSetInt16,      PROP("setInt16"),     Method) \
    V(DataViewPrototypeSetUint16,     PROP("setUint16"),    Method) \
    V(DataViewPrototypeSetInt32,      PROP("setInt32"),     Method) \
    V(DataViewPrototypeSetUint32,     PROP("setUint32"),    Method) \
    V(DataViewPrototypeSetFloat16,    PROP("setFloat16"),   Method) \
    V(DataViewPrototypeSetFloat32,    PROP("setFloat32"),   Method) \
    V(DataViewPrototypeSetFloat64,    PROP("setFloat64"),   Method) \
    V(DataViewPrototypeSetBigInt64,   PROP("setBigInt64"),  Method) \
    V(DataViewPrototypeSetBigUint64,  PROP("setBigUint64"), Method) \
    V(DataViewPrototypeGetBuffer,     PROP("buffer"),       Getter) \
    V(DataViewPrototypeGetByteOffset, PROP("byteOffset"),   Getter) \
    V(DataViewPrototypeGetByteLength, PROP("byteLength"),   Getter) \
    V(DataViewPrototypeConstructor,   PROP("constructor"),  Method) \

#define JSC_FOREACH_PRIMORDIAL_DataViewConstructor(V) \
    V(DataView, SELF, Self) \

#define JSC_FOREACH_PRIMORDIAL_Int8ArrayPrototype(V) \
    V(Int8ArrayPrototype,            SELF,                Self) \
    V(Int8ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_Int8ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_Uint8ArrayPrototype(V) \
    V(Uint8ArrayPrototype,              SELF,                  Self) \
    V(Uint8ArrayPrototypeSetFromBase64, PROP("setFromBase64"), Method) \
    V(Uint8ArrayPrototypeSetFromHex,    PROP("setFromHex"),    Method) \
    V(Uint8ArrayPrototypeToBase64,      PROP("toBase64"),      Method) \
    V(Uint8ArrayPrototypeToHex,         PROP("toHex"),         Method) \
    V(Uint8ArrayPrototypeConstructor,   PROP("constructor"),   Method) \

#define JSC_FOREACH_PRIMORDIAL_Uint8ArrayConstructor(V) \
    V(Uint8ArrayFromBase64, PROP("fromBase64"), Method) \
    V(Uint8ArrayFromHex,    PROP("fromHex"),    Method) \

#define JSC_FOREACH_PRIMORDIAL_Uint8ClampedArrayPrototype(V) \
    V(Uint8ClampedArrayPrototype,            SELF,                Self) \
    V(Uint8ClampedArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_Uint8ClampedArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_Int16ArrayPrototype(V) \
    V(Int16ArrayPrototype,            SELF,                Self) \
    V(Int16ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_Int16ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_Uint16ArrayPrototype(V) \
    V(Uint16ArrayPrototype,            SELF,                Self) \
    V(Uint16ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_Uint16ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_Int32ArrayPrototype(V) \
    V(Int32ArrayPrototype,            SELF,                Self) \
    V(Int32ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_Int32ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_Uint32ArrayPrototype(V) \
    V(Uint32ArrayPrototype,            SELF,                Self) \
    V(Uint32ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_Uint32ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_Float16ArrayPrototype(V) \



#define JSC_FOREACH_PRIMORDIAL_Float16ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_Float32ArrayPrototype(V) \
    V(Float32ArrayPrototype,            SELF,                Self) \
    V(Float32ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_Float32ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_Float64ArrayPrototype(V) \
    V(Float64ArrayPrototype,            SELF,                Self) \
    V(Float64ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_Float64ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_BigInt64ArrayPrototype(V) \
    V(BigInt64ArrayPrototype,            SELF,                Self) \
    V(BigInt64ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_BigInt64ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_BigUint64ArrayPrototype(V) \
    V(BigUint64ArrayPrototype,            SELF,                Self) \
    V(BigUint64ArrayPrototypeConstructor, PROP("constructor"), Method) \

#define JSC_FOREACH_PRIMORDIAL_BigUint64ArrayConstructor(V) \



#define JSC_FOREACH_PRIMORDIAL_MathObject(V) \
    V(MathAbs,        PROP("abs"),        Method) \
    V(MathAcos,       PROP("acos"),       Method) \
    V(MathAsin,       PROP("asin"),       Method) \
    V(MathAtan,       PROP("atan"),       Method) \
    V(MathAcosh,      PROP("acosh"),      Method) \
    V(MathAsinh,      PROP("asinh"),      Method) \
    V(MathAtanh,      PROP("atanh"),      Method) \
    V(MathAtan2,      PROP("atan2"),      Method) \
    V(MathCbrt,       PROP("cbrt"),       Method) \
    V(MathCeil,       PROP("ceil"),       Method) \
    V(MathClz32,      PROP("clz32"),      Method) \
    V(MathCos,        PROP("cos"),        Method) \
    V(MathCosh,       PROP("cosh"),       Method) \
    V(MathExp,        PROP("exp"),        Method) \
    V(MathExpm1,      PROP("expm1"),      Method) \
    V(MathFloor,      PROP("floor"),      Method) \
    V(MathFround,     PROP("fround"),     Method) \
    V(MathHypot,      PROP("hypot"),      Method) \
    V(MathLog,        PROP("log"),        Method) \
    V(MathLog10,      PROP("log10"),      Method) \
    V(MathLog1p,      PROP("log1p"),      Method) \
    V(MathLog2,       PROP("log2"),       Method) \
    V(MathMax,        PROP("max"),        Method) \
    V(MathMin,        PROP("min"),        Method) \
    V(MathPow,        PROP("pow"),        Method) \
    V(MathRandom,     PROP("random"),     Method) \
    V(MathRound,      PROP("round"),      Method) \
    V(MathSign,       PROP("sign"),       Method) \
    V(MathSin,        PROP("sin"),        Method) \
    V(MathSinh,       PROP("sinh"),       Method) \
    V(MathSqrt,       PROP("sqrt"),       Method) \
    V(MathTan,        PROP("tan"),        Method) \
    V(MathTanh,       PROP("tanh"),       Method) \
    V(MathTrunc,      PROP("trunc"),      Method) \
    V(MathImul,       PROP("imul"),       Method) \
    V(MathF16round,   PROP("f16round"),   Method) \
    V(MathSumPrecise, PROP("sumPrecise"), Method) \

#define JSC_FOREACH_PRIMORDIAL_JSONObject(V) \
    V(JSONParse,     PROP("parse"),     Method) \
    V(JSONStringify, PROP("stringify"), Method) \
    V(JSONIsRawJSON, PROP("isRawJSON"), Method) \
    V(JSONRawJSON,   PROP("rawJSON"),   Method) \

#define JSC_FOREACH_PRIMORDIAL_ReflectObject(V) \
    V(ReflectApply,                    PROP("apply"),                    Method) \
    V(ReflectConstruct,                PROP("construct"),                Method) \
    V(ReflectDefineProperty,           PROP("defineProperty"),           Method) \
    V(ReflectDeleteProperty,           PROP("deleteProperty"),           Method) \
    V(ReflectGet,                      PROP("get"),                      Method) \
    V(ReflectGetOwnPropertyDescriptor, PROP("getOwnPropertyDescriptor"), Method) \
    V(ReflectGetPrototypeOf,           PROP("getPrototypeOf"),           Method) \
    V(ReflectHas,                      PROP("has"),                      Method) \
    V(ReflectIsExtensible,             PROP("isExtensible"),             Method) \
    V(ReflectOwnKeys,                  PROP("ownKeys"),                  Method) \
    V(ReflectPreventExtensions,        PROP("preventExtensions"),        Method) \
    V(ReflectSet,                      PROP("set"),                      Method) \
    V(ReflectSetPrototypeOf,           PROP("setPrototypeOf"),           Method) \

#define JSC_FOREACH_PRIMORDIAL_AtomicsObject(V) \
    V(AtomicsAdd,             PROP("add"),             Method) \
    V(AtomicsAnd,             PROP("and"),             Method) \
    V(AtomicsCompareExchange, PROP("compareExchange"), Method) \
    V(AtomicsExchange,        PROP("exchange"),        Method) \
    V(AtomicsIsLockFree,      PROP("isLockFree"),      Method) \
    V(AtomicsLoad,            PROP("load"),            Method) \
    V(AtomicsNotify,          PROP("notify"),          Method) \
    V(AtomicsOr,              PROP("or"),              Method) \
    V(AtomicsStore,           PROP("store"),           Method) \
    V(AtomicsSub,             PROP("sub"),             Method) \
    V(AtomicsWait,            PROP("wait"),            Method) \
    V(AtomicsXor,             PROP("xor"),             Method) \
    V(AtomicsPause,           PROP("pause"),           Method) \
    V(AtomicsWaitAsync,       PROP("waitAsync"),       Method) \

#define JSC_FOREACH_PRIMORDIAL_ProxyObject(V) \
    V(Proxy,          SELF,              Self) \
    V(ProxyRevocable, PROP("revocable"), Method) \

// ---------------------------------------------------------------------------
// Holders, eager (exist after JSGlobalObject::init(), snapshotted there) then
// lazy (snapshotted where they are created).
// ---------------------------------------------------------------------------

#define JSC_FOREACH_PRIMORDIAL_EAGER_HOLDER(H) \
    H(GlobalObject) \
    H(ObjectPrototype) \
    H(ObjectConstructor) \
    H(FunctionPrototype) \
    H(FunctionConstructor) \
    H(ArrayPrototype) \
    H(ArrayConstructor) \
    H(StringPrototype) \
    H(StringConstructor) \
    H(RegExpPrototype) \
    H(RegExpConstructor) \
    H(SymbolPrototype) \
    H(SymbolConstructor) \
    H(BigIntPrototype) \
    H(BigIntConstructor) \
    H(PromisePrototype) \
    H(PromiseConstructor) \
    H(IteratorPrototype) \
    H(IteratorConstructor) \
    H(ArrayIteratorPrototype) \
    H(StringIteratorPrototype) \
    H(MapIteratorPrototype) \
    H(SetIteratorPrototype) \
    H(RegExpStringIteratorPrototype) \
    H(IteratorHelperPrototype) \
    H(WrapForValidIteratorPrototype) \
    H(AsyncIteratorPrototype) \
    H(GeneratorFunctionPrototype) \
    H(AsyncFunctionPrototype) \
    H(AsyncGeneratorFunctionPrototype) \
    H(WeakRefPrototype) \
    H(WeakRefConstructor) \
    H(FinalizationRegistryPrototype) \
    H(FinalizationRegistryConstructor) \

#define JSC_FOREACH_PRIMORDIAL_LAZY_HOLDER(H) \
    H(BooleanPrototype) \
    H(BooleanConstructor) \
    H(NumberPrototype) \
    H(NumberConstructor) \
    H(DatePrototype) \
    H(DateConstructor) \
    H(ErrorPrototype) \
    H(ErrorConstructor) \
    H(MapPrototype) \
    H(MapConstructor) \
    H(SetPrototype) \
    H(SetConstructor) \
    H(WeakMapPrototype) \
    H(WeakMapConstructor) \
    H(WeakSetPrototype) \
    H(WeakSetConstructor) \
    H(ArrayBufferPrototype) \
    H(ArrayBufferConstructor) \
    H(AggregateErrorPrototype) \
    H(AggregateErrorConstructor) \
    H(EvalErrorPrototype) \
    H(EvalErrorConstructor) \
    H(RangeErrorPrototype) \
    H(RangeErrorConstructor) \
    H(ReferenceErrorPrototype) \
    H(ReferenceErrorConstructor) \
    H(SyntaxErrorPrototype) \
    H(SyntaxErrorConstructor) \
    H(TypeErrorPrototype) \
    H(TypeErrorConstructor) \
    H(URIErrorPrototype) \
    H(URIErrorConstructor) \
    H(TypedArrayPrototype) \
    H(TypedArrayConstructor) \
    H(DataViewPrototype) \
    H(DataViewConstructor) \
    H(Int8ArrayPrototype) \
    H(Int8ArrayConstructor) \
    H(Uint8ArrayPrototype) \
    H(Uint8ArrayConstructor) \
    H(Uint8ClampedArrayPrototype) \
    H(Uint8ClampedArrayConstructor) \
    H(Int16ArrayPrototype) \
    H(Int16ArrayConstructor) \
    H(Uint16ArrayPrototype) \
    H(Uint16ArrayConstructor) \
    H(Int32ArrayPrototype) \
    H(Int32ArrayConstructor) \
    H(Uint32ArrayPrototype) \
    H(Uint32ArrayConstructor) \
    H(Float16ArrayPrototype) \
    H(Float16ArrayConstructor) \
    H(Float32ArrayPrototype) \
    H(Float32ArrayConstructor) \
    H(Float64ArrayPrototype) \
    H(Float64ArrayConstructor) \
    H(BigInt64ArrayPrototype) \
    H(BigInt64ArrayConstructor) \
    H(BigUint64ArrayPrototype) \
    H(BigUint64ArrayConstructor) \
    H(MathObject) \
    H(JSONObject) \
    H(ReflectObject) \
    H(AtomicsObject) \
    H(ProxyObject) \

#define JSC_FOREACH_PRIMORDIAL_HOLDER(H) \
    JSC_FOREACH_PRIMORDIAL_EAGER_HOLDER(H) \
    JSC_FOREACH_PRIMORDIAL_LAZY_HOLDER(H) \

#define JSC_FOREACH_PRIMORDIAL_NAME(V) \
    JSC_FOREACH_PRIMORDIAL_GlobalObject(V) \
    JSC_FOREACH_PRIMORDIAL_ObjectPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ObjectConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_FunctionPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_FunctionConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_StringPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_StringConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_RegExpPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_RegExpConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_SymbolPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_SymbolConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_BigIntPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_BigIntConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_PromisePrototype(V) \
    JSC_FOREACH_PRIMORDIAL_PromiseConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_IteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_IteratorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_ArrayIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_StringIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_MapIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_SetIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_RegExpStringIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_IteratorHelperPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WrapForValidIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_AsyncIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_GeneratorFunctionPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_AsyncFunctionPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_AsyncGeneratorFunctionPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WeakRefPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WeakRefConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_FinalizationRegistryPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_FinalizationRegistryConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_BooleanPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_BooleanConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_NumberPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_NumberConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_DatePrototype(V) \
    JSC_FOREACH_PRIMORDIAL_DateConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_ErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_MapPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_MapConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_SetPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_SetConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_WeakMapPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WeakMapConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_WeakSetPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WeakSetConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_ArrayBufferPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ArrayBufferConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_AggregateErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_AggregateErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_EvalErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_EvalErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_RangeErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_RangeErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_ReferenceErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ReferenceErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_SyntaxErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_SyntaxErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_TypeErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_TypeErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_URIErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_URIErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_TypedArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_TypedArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_DataViewPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_DataViewConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Int8ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Int8ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Uint8ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Uint8ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Uint8ClampedArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Uint8ClampedArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Int16ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Int16ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Uint16ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Uint16ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Int32ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Int32ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Uint32ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Uint32ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Float16ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Float16ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Float32ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Float32ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_Float64ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_Float64ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_BigInt64ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_BigInt64ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_BigUint64ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_BigUint64ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_MathObject(V) \
    JSC_FOREACH_PRIMORDIAL_JSONObject(V) \
    JSC_FOREACH_PRIMORDIAL_ReflectObject(V) \
    JSC_FOREACH_PRIMORDIAL_AtomicsObject(V) \
    JSC_FOREACH_PRIMORDIAL_ProxyObject(V) \

// V(Holder, expression yielding the holder JSObject* inside a JSGlobalObject member).
#define JSC_FOREACH_PRIMORDIAL_HOLDER_ACCESSOR(V) \
    V(GlobalObject, this) \
    V(ObjectPrototype, objectPrototype()) \
    V(ObjectConstructor, m_objectConstructor.get()) \
    V(FunctionPrototype, functionPrototype()) \
    V(FunctionConstructor, functionConstructor()) \
    V(ArrayPrototype, arrayPrototype()) \
    V(ArrayConstructor, m_arrayConstructor.get()) \
    V(StringPrototype, stringPrototype()) \
    V(StringConstructor, stringConstructor()) \
    V(RegExpPrototype, regExpPrototype()) \
    V(RegExpConstructor, regExpConstructor()) \
    V(SymbolPrototype, symbolPrototype()) \
    V(SymbolConstructor, symbolConstructor()) \
    V(BigIntPrototype, bigIntPrototype()) \
    V(BigIntConstructor, bigIntConstructor()) \
    V(PromisePrototype, promisePrototype()) \
    V(PromiseConstructor, promiseConstructor()) \
    V(IteratorPrototype, iteratorPrototype()) \
    V(IteratorConstructor, iteratorConstructor()) \
    V(ArrayIteratorPrototype, arrayIteratorPrototype()) \
    V(StringIteratorPrototype, m_stringIteratorPrototype.get()) \
    V(MapIteratorPrototype, mapIteratorPrototype()) \
    V(SetIteratorPrototype, setIteratorPrototype()) \
    V(RegExpStringIteratorPrototype, m_regExpStringIteratorStructure.get()->storedPrototypeObject()) \
    V(IteratorHelperPrototype, iteratorHelperPrototype()) \
    V(WrapForValidIteratorPrototype, m_wrapForValidIteratorStructure.get()->storedPrototypeObject()) \
    V(AsyncIteratorPrototype, asyncIteratorPrototype()) \
    V(GeneratorFunctionPrototype, generatorFunctionPrototype()) \
    V(AsyncFunctionPrototype, asyncFunctionPrototype()) \
    V(AsyncGeneratorFunctionPrototype, asyncGeneratorFunctionPrototype()) \
    V(WeakRefPrototype, m_weakObjectRefPrototype.get()) \
    V(WeakRefConstructor, weakObjectRefConstructor()) \
    V(FinalizationRegistryPrototype, m_finalizationRegistryPrototype.get()) \
    V(FinalizationRegistryConstructor, finalizationRegistryConstructor()) \
    V(BooleanPrototype, booleanPrototype()) \
    V(BooleanConstructor, booleanObjectConstructor()) \
    V(NumberPrototype, numberPrototype()) \
    V(NumberConstructor, numberObjectConstructor()) \
    V(DatePrototype, datePrototype()) \
    V(DateConstructor, dateConstructor()) \
    V(ErrorPrototype, errorPrototype()) \
    V(ErrorConstructor, errorConstructor()) \
    V(MapPrototype, mapPrototype()) \
    V(MapConstructor, mapConstructor()) \
    V(SetPrototype, jsSetPrototype()) \
    V(SetConstructor, setConstructor()) \
    V(WeakMapPrototype, m_weakMapStructure.prototype(this)) \
    V(WeakMapConstructor, weakMapConstructor()) \
    V(WeakSetPrototype, m_weakSetStructure.prototype(this)) \
    V(WeakSetConstructor, weakSetConstructor()) \
    V(ArrayBufferPrototype, arrayBufferPrototype(ArrayBufferSharingMode::Default)) \
    V(ArrayBufferConstructor, arrayBufferConstructor(ArrayBufferSharingMode::Default)) \
    V(AggregateErrorPrototype, m_aggregateErrorStructure.prototype(this)) \
    V(AggregateErrorConstructor, m_aggregateErrorStructure.constructor(this)) \
    V(EvalErrorPrototype, m_evalErrorStructure.prototype(this)) \
    V(EvalErrorConstructor, m_evalErrorStructure.constructor(this)) \
    V(RangeErrorPrototype, m_rangeErrorStructure.prototype(this)) \
    V(RangeErrorConstructor, m_rangeErrorStructure.constructor(this)) \
    V(ReferenceErrorPrototype, m_referenceErrorStructure.prototype(this)) \
    V(ReferenceErrorConstructor, m_referenceErrorStructure.constructor(this)) \
    V(SyntaxErrorPrototype, m_syntaxErrorStructure.prototype(this)) \
    V(SyntaxErrorConstructor, m_syntaxErrorStructure.constructor(this)) \
    V(TypeErrorPrototype, m_typeErrorStructure.prototype(this)) \
    V(TypeErrorConstructor, m_typeErrorStructure.constructor(this)) \
    V(URIErrorPrototype, m_URIErrorStructure.prototype(this)) \
    V(URIErrorConstructor, m_URIErrorStructure.constructor(this)) \
    V(TypedArrayPrototype, m_typedArrayProto.get(this)) \
    V(TypedArrayConstructor, m_typedArraySuperConstructor.get(this)) \
    V(DataViewPrototype, typedArrayPrototype(TypeDataView)) \
    V(DataViewConstructor, typedArrayConstructor(TypeDataView)) \
    V(Int8ArrayPrototype, typedArrayPrototype(TypeInt8)) \
    V(Int8ArrayConstructor, typedArrayConstructor(TypeInt8)) \
    V(Uint8ArrayPrototype, typedArrayPrototype(TypeUint8)) \
    V(Uint8ArrayConstructor, typedArrayConstructor(TypeUint8)) \
    V(Uint8ClampedArrayPrototype, typedArrayPrototype(TypeUint8Clamped)) \
    V(Uint8ClampedArrayConstructor, typedArrayConstructor(TypeUint8Clamped)) \
    V(Int16ArrayPrototype, typedArrayPrototype(TypeInt16)) \
    V(Int16ArrayConstructor, typedArrayConstructor(TypeInt16)) \
    V(Uint16ArrayPrototype, typedArrayPrototype(TypeUint16)) \
    V(Uint16ArrayConstructor, typedArrayConstructor(TypeUint16)) \
    V(Int32ArrayPrototype, typedArrayPrototype(TypeInt32)) \
    V(Int32ArrayConstructor, typedArrayConstructor(TypeInt32)) \
    V(Uint32ArrayPrototype, typedArrayPrototype(TypeUint32)) \
    V(Uint32ArrayConstructor, typedArrayConstructor(TypeUint32)) \
    V(Float16ArrayPrototype, typedArrayPrototype(TypeFloat16)) \
    V(Float16ArrayConstructor, typedArrayConstructor(TypeFloat16)) \
    V(Float32ArrayPrototype, typedArrayPrototype(TypeFloat32)) \
    V(Float32ArrayConstructor, typedArrayConstructor(TypeFloat32)) \
    V(Float64ArrayPrototype, typedArrayPrototype(TypeFloat64)) \
    V(Float64ArrayConstructor, typedArrayConstructor(TypeFloat64)) \
    V(BigInt64ArrayPrototype, typedArrayPrototype(TypeBigInt64)) \
    V(BigInt64ArrayConstructor, typedArrayConstructor(TypeBigInt64)) \
    V(BigUint64ArrayPrototype, typedArrayPrototype(TypeBigUint64)) \
    V(BigUint64ArrayConstructor, typedArrayConstructor(TypeBigUint64)) \
    V(MathObject, pristineNamespaceObject<MathObject>(vm, this, Identifier::fromString(vm, "Math"_s), createMathProperty)) \
    V(JSONObject, pristineNamespaceObject<JSONObject>(vm, this, Identifier::fromString(vm, "JSON"_s), createJSONProperty)) \
    V(ReflectObject, pristineNamespaceObject<ReflectObject>(vm, this, Identifier::fromString(vm, "Reflect"_s), createReflectProperty)) \
    V(AtomicsObject, pristineNamespaceObject<AtomicsObject>(vm, this, Identifier::fromString(vm, "Atomics"_s), createAtomicsProperty)) \
    V(ProxyObject, pristineNamespaceObject<ProxyConstructor>(vm, this, Identifier::fromString(vm, "Proxy"_s), createProxyProperty)) \

// Names CREATE_PROTOTYPE_FOR_LAZY_TYPE's hook uses for its capitalName parameter.
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_Boolean PrimordialHolder::BooleanPrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_Boolean PrimordialHolder::BooleanConstructor
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_Date PrimordialHolder::DatePrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_Date PrimordialHolder::DateConstructor
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_Error PrimordialHolder::ErrorPrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_Error PrimordialHolder::ErrorConstructor
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_Map PrimordialHolder::MapPrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_Map PrimordialHolder::MapConstructor
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_Number PrimordialHolder::NumberPrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_Number PrimordialHolder::NumberConstructor
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_Set PrimordialHolder::SetPrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_Set PrimordialHolder::SetConstructor
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_WeakMap PrimordialHolder::WeakMapPrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_WeakMap PrimordialHolder::WeakMapConstructor
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_WeakSet PrimordialHolder::WeakSetPrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_WeakSet PrimordialHolder::WeakSetConstructor
#define JSC_PRIMORDIAL_LAZY_TYPE_PROTOTYPE_HOLDER_JSArrayBuffer PrimordialHolder::ArrayBufferPrototype
#define JSC_PRIMORDIAL_LAZY_TYPE_CONSTRUCTOR_HOLDER_JSArrayBuffer PrimordialHolder::ArrayBufferConstructor

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
