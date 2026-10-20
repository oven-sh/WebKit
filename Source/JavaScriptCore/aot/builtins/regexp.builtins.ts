import * as b from "bun-builtins-table";

const state = "RegExpState";
const maximumInt32 = b.checked(2 ** 31 - 1, "INT32_MAX");
const nonNegativeInt32 = b.int(0, maximumInt32);
const self = b.operand("this");
const stringReceiver = b.string.loose.refusing(b.null);
const regExpConstructor = b.builtin("RegExp");
const pristineRegExp = b.instance("RegExp").pristine("Names");
const newRegExp = b.instance("RegExp").fresh;
const newMatchIterator = b.instance("RegExpStringIterator").fresh;
const capture = b.union(b.string, b.undefined);
const matchResult = b.union(b.instance("Array").holding({ element: capture }).unaliased, b.strictNull);
const newArrayOf = (element: b.TableType) => b.instance("Array").holding({ element }).fresh;

const characters = [b.readAsCharacters];
const patternText = [b.contentKey, b.readAsCharacters];
const usesLastIndex = [b.readsContents("lastIndex"), b.writesContents("lastIndex"), b.runsOnContents("ToPrimitive")];
const escapesToOperandCode = [b.retained, b.runs("ToPrimitive")];
const storesLastIndex = b.storedByKey(b.keysNamed("lastIndex"), b.length);
const restoresLastIndex = b.storedByKey(b.keysNamed("lastIndex"), b.any);
const runsMembers = (...protocols: readonly b.ProtocolSpec[]) => b.runsTransitively("ToPrimitive", ...protocols);

const flagNames = ["hasIndices", "global", "ignoreCase", "multiline", "dotAll", "unicode", "unicodeSets", "sticky"];
const namesReadByConstruct = ["@@match", "source", "flags"];

const createRegExp = (resultForObject: b.TableType, namesReadFromPattern: readonly string[]) =>
  [
    {
      args: { pattern: b.string, flags: b.string.loose.optional },
      returns: newRegExp,
      capability: "heap",
      throws: true,
      uses: { pattern: patternText, flags: patternText },
      implementation: b.inline("NewRegExp", { otherwise: b.hostCall }),
    },
    {
      args: { pattern: pristineRegExp, flags: b.string.loose.optional },
      returns: resultForObject,
      capability: "heap",
      throws: true,
      readsStateBeforeConversions: true,
      uses: { pattern: [b.contentKey], flags: patternText },
      reads: [state],
      implementation: b.hostCall,
    },
    {
      args: { pattern: b.primitiveValue.optional, flags: b.string.loose.optional },
      returns: newRegExp,
      capability: "heap",
      throws: true,
      uses: { pattern: patternText, flags: patternText },
      implementation: b.hostCall,
    },
    {
      args: { pattern: b.any, flags: b.any.optional },
      returns: resultForObject,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: {
        pattern: [b.contentKey, runsMembers(b.named(...namesReadFromPattern))],
        flags: [b.contentKey, b.runs("ToPrimitive")],
      },
      reads: [state],
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const legacyText = {
  this: regExpConstructor,
  getter: b.string,
  capability: "heap",
  throws: true,
  uses: { this: b.inspected },
  reads: [state],
  writes: [state],
  mutability: "ChangesOnItsOwn",
  overloadUsesFeature: "RegExp",
  implementation: b.generic,
} satisfies b.GetterField;

const legacyInput = {
  ...legacyText,
  setter: {
    this: regExpConstructor,
    value: b.string.loose,
    capability: "heap",
    throws: true,
    uses: { this: b.inspected },
    writes: [state],
    overloadUsesFeature: "RegExp",
    implementation: b.generic,
  },
} satisfies b.GetterField;

const legacyMultiline = {
  this: regExpConstructor,
  getter: b.bool,
  capability: "none",
  uses: { this: b.inspected },
  reads: [state],
  mutability: "ChangesOnItsOwn",
  overloadUsesFeature: "RegExp",
  implementation: b.generic,
  setter: {
    this: regExpConstructor,
    value: b.bool.loose,
    capability: "none",
    uses: { this: b.inspected },
    writes: [state],
    overloadUsesFeature: "RegExp",
    implementation: b.generic,
  },
} satisfies b.GetterField;

const flagGetter = (flag: string) =>
  [
    {
      getter: b.bool,
      capability: "none",
      uses: { this: b.inspected },
      reads: [state],
      usesFeature: "RegExp",
      nativeSymbols: [`regExpProtoGetter${flag}`],
      implementation: b.inline("RegExpFlag", { parameters: [flag] }),
    },
    {
      this: b.any,
      getter: b.undefined,
      capability: "none",
      throws: true,
      uses: { this: b.inspected },
      implementation: b.hostCall,
    },
  ] satisfies readonly b.GetterField[];

type SymbolMethodColumns = Pick<b.SignatureOptions, "args" | "returns" | "uses" | "runsOnlyOperandCode"> &
  Pick<b.EntryColumns, "usesFeature">;

const symbolMethod = (nativeSymbol: string, columns: SymbolMethodColumns) =>
  ({
    this: b.object,
    capability: "js",
    throws: true,
    reads: [state],
    writes: [state],
    nativeSymbols: [nativeSymbol],
    implementation: b.hostCall,
    ...columns,
  }) satisfies b.FunctionField;

const invokeSymbolMethod = (...membersOfRegExp: readonly b.ProtocolSpec[]) =>
  ({
    this: b.any,
    args: { regexp: b.any.optional },
    returns: b.any,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: { this: escapesToOperandCode, regexp: [runsMembers(...membersOfRegExp)] },
    reads: [state],
    writes: [state],
    overloadUsesFeature: ["RegExpFromString", "RegExp", "RegExpUnicodeProperties"],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const replace = (inlineBuiltin: string, times: b.CallbackTimes, ...membersOfSearchValue: readonly b.ProtocolSpec[]) =>
  [
    {
      this: stringReceiver,
      args: { searchValue: pristineRegExp, replaceValue: b.any },
      returns: b.string,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      readsStateBeforeConversions: true,
      uses: {
        searchValue: usesLastIndex,
        replaceValue: [
          b.runs("ToPrimitive"),
          b.called({
            times: "Repeatedly",
            args: [b.string],
            spreadArgs: b.union(capture, nonNegativeInt32, b.object.fresh),
            result: "Escapes",
          }),
        ],
      },
      overloadUsesFeature: "RegExp",
      implementation: b.inline(inlineBuiltin, { parameters: ["RegExp"], otherwise: b.hostCall }),
    },
    {
      this: stringReceiver,
      args: { searchValue: b.string, replaceValue: b.string },
      returns: b.string,
      capability: "heap",
      throws: true,
      uses: { searchValue: characters, replaceValue: characters },
      implementation: b.inline(inlineBuiltin, { parameters: ["String"] }),
    },
    {
      this: stringReceiver,
      args: { searchValue: b.primitiveValue, replaceValue: b.any },
      returns: b.string,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: {
        searchValue: characters,
        replaceValue: [
          b.runs("ToPrimitive"),
          b.called({ times, args: [b.string, nonNegativeInt32, b.string], result: "Escapes" }),
        ],
      },
      implementation: b.hostCall,
    },
    {
      this: b.any,
      args: { searchValue: b.any.optional, replaceValue: b.any.optional },
      returns: b.any,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: {
        this: escapesToOperandCode,
        searchValue: [runsMembers(...membersOfSearchValue)],
        replaceValue: [
          ...escapesToOperandCode,
          b.called({ times: "Repeatedly", args: [b.string], spreadArgs: b.any, result: "Escapes" }),
        ],
      },
      reads: [state],
      writes: [state],
      overloadUsesFeature: "RegExp",
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const splitAtString = (limit: b.TableType) =>
  ({
    this: stringReceiver,
    args: { separator: b.string, limit },
    returns: newArrayOf(b.string).dense,
    capability: "heap",
    throws: true,
    uses: { separator: characters },
    implementation: b.inline("StringSplit", { parameters: ["String"] }),
  }) satisfies b.FunctionField;

const splitAtRegExp = (limit: b.TableType) =>
  ({
    this: stringReceiver,
    args: { separator: pristineRegExp, limit },
    returns: newArrayOf(capture).dense,
    capability: "heap",
    throws: true,
    readsStateBeforeConversions: true,
    uses: { separator: b.inspected },
    reads: [state],
    writes: [state],
    overloadUsesFeature: "RegExp",
    implementation: b.inline("StringSplit", { parameters: ["RegExp"], otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

export default b.declarations(
  b.class("RegExp", {
    jsType: "RegExpObjectType",
    slots: { lastIndex: b.state(state, b.any) },
    stateHeap: state,
    lockedObjects: ["prototype"],
    lockedMembers: ["constructor"],
    mutability: "AlwaysLocked",
    types: [b.exactType("RegExp")],
    constructorTable: b.staticTable("runtime/RegExpConstructor.cpp", "regExpConstructorTable"),
    usesFeature: ["RegExpFromString", "RegExp", "RegExpUnicodeProperties"],
    nativeSymbols: ["callRegExpConstructor", "constructWithRegExpConstructor"],
    call: createRegExp(b.union(b.operand("pattern"), b.instance("RegExp")), [...namesReadByConstruct, "constructor"]),
    construct: createRegExp(newRegExp, namesReadByConstruct),
    static: {
      "@@species": {
        this: b.any,
        getter: self,
        capability: "none",
        uses: { this: b.inspected },
        mutability: "AlwaysLocked",
        overloadUsesFeature: ["RegExpFromString", "RegExp", "RegExpUnicodeProperties"],
        implementation: b.generic,
      },
      escape: {
        args: { string: b.string },
        returns: b.string,
        capability: "heap",
        throws: true,
        uses: { string: characters },
        usesFeature: "RegExp",
        nativeSymbols: ["regExpConstructorEscape"],
        implementation: b.hostCall,
      },
      input: legacyInput,
      $_: legacyInput,
      multiline: legacyMultiline,
      "$*": legacyMultiline,
      lastMatch: legacyText,
      "$&": legacyText,
      lastParen: legacyText,
      "$+": legacyText,
      leftContext: legacyText,
      "$`": legacyText,
      rightContext: legacyText,
      "$'": legacyText,
      $1: legacyText,
      $2: legacyText,
      $3: legacyText,
      $4: legacyText,
      $5: legacyText,
      $6: legacyText,
      $7: legacyText,
      $8: legacyText,
      $9: legacyText,
    },
    instance: {
      lastIndex: {
        getter: b.union(b.length, b.slot("this", "lastIndex")),
        capability: "none",
        uses: { this: [b.readsContents("lastIndex")] },
        implementation: b.inline("RegExpGetLastIndex"),
        setter: {
          value: b.any,
          capability: "none",
          throws: true,
          uses: { this: [b.writesContents("lastIndex")], value: [b.storedIn(b.slot("this", "lastIndex"))] },
          implementation: b.inline("RegExpSetLastIndex"),
        },
      },
    },
    proto: {
      dotAll: flagGetter("DotAll"),
      global: flagGetter("Global"),
      hasIndices: flagGetter("HasIndices"),
      ignoreCase: flagGetter("IgnoreCase"),
      multiline: flagGetter("Multiline"),
      sticky: flagGetter("Sticky"),
      unicode: flagGetter("Unicode"),
      unicodeSets: flagGetter("UnicodeSets"),
      flags: [
        {
          this: pristineRegExp,
          getter: b.string,
          capability: "heap",
          throws: true,
          uses: { this: b.inspected },
          reads: [state],
          usesFeature: "RegExp",
          nativeSymbols: ["regExpProtoGetterFlags"],
          implementation: b.inline("RegExpFlags"),
        },
        {
          this: b.object,
          getter: b.string,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: [b.runs(b.named(...flagNames))] },
          reads: [state],
          implementation: b.hostCall,
        },
      ],
      source: {
        getter: b.string,
        capability: "heap",
        throws: true,
        uses: { this: b.inspected },
        reads: [state],
        usesFeature: "RegExp",
        nativeSymbols: ["regExpProtoGetterSource"],
        implementation: b.inline("RegExpSource"),
      },
      compile: {
        args: { pattern: b.any.optional, flags: b.any.optional },
        returns: self,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          this: [b.writesContents(), b.replacesContents],
          pattern: [b.runs("ToPrimitive")],
          flags: [b.runs("ToPrimitive")],
        },
        reads: [state],
        usesFeature: ["RegExpFromString", "RegExp", "RegExpUnicodeProperties"],
        nativeSymbols: ["regExpProtoFuncCompile"],
        implementation: b.hostCall,
      },
      exec: {
        args: { string: b.string.loose },
        returns: matchResult,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { this: usesLastIndex },
        usesFeature: "RegExp",
        nativeSymbols: ["regExpProtoFuncExec"],
        substringOf: "string",
        prefixForm: "RegExpExecPrefix",
        prefixProperties: { index: nonNegativeInt32, input: b.string },
        implementation: b.inline("RegExpExec"),
      },
      test: [
        {
          this: pristineRegExp,
          args: { string: b.string.loose },
          returns: b.bool,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: usesLastIndex },
          usesFeature: "RegExp",
          nativeSymbols: ["regExpProtoFuncTest"],
          implementation: b.inline("RegExpTest"),
        },
        {
          this: b.object,
          args: { string: b.string.loose },
          returns: b.bool,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: [runsMembers(b.calls("exec"))] },
          reads: [state],
          writes: [state],
          implementation: b.hostCall,
        },
      ],
      toString: {
        this: b.object,
        returns: b.string,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { this: [runsMembers(b.named("source", "flags"))] },
        reads: [state],
        usesFeature: "RegExp",
        nativeSymbols: ["regExpProtoFuncToString"],
        implementation: b.hostCall,
      },
      "@@match": symbolMethod("regExpProtoFuncMatch", {
        args: { string: b.string.loose },
        returns: b.union(b.object, b.strictNull),
        runsOnlyOperandCode: true,
        uses: { this: [runsMembers(b.named("flags", "lastIndex"), b.calls("exec"), "OwnValues"), storesLastIndex] },
        usesFeature: "RegExp",
      }),
      "@@matchAll": symbolMethod("regExpProtoFuncMatchAll", {
        args: { string: b.string.loose },
        returns: newMatchIterator,
        uses: { this: [b.retained, runsMembers(b.named(...namesReadByConstruct, "lastIndex"), "Species")] },
        usesFeature: ["RegExp", "RegExpFromString", "RegExpUnicodeProperties"],
      }),
      "@@replace": symbolMethod("regExpProtoFuncReplace", {
        args: { string: b.string.loose, replaceValue: b.any.optional },
        returns: b.string,
        runsOnlyOperandCode: true,
        uses: {
          this: [
            runsMembers(b.named("flags", "lastIndex", "length", "index", "groups"), b.calls("exec"), "OwnValues"),
            storesLastIndex,
          ],
          replaceValue: [
            b.runs("ToPrimitive"),
            b.called({ times: "Repeatedly", args: [b.string], spreadArgs: b.any, result: "Escapes" }),
          ],
        },
        usesFeature: "RegExp",
      }),
      "@@search": symbolMethod("regExpProtoFuncSearch", {
        args: { string: b.string.loose },
        returns: b.any,
        runsOnlyOperandCode: true,
        uses: { this: [runsMembers(b.named("lastIndex", "index"), b.calls("exec")), restoresLastIndex] },
        usesFeature: "RegExp",
      }),
      "@@split": symbolMethod("regExpProtoFuncSplit", {
        args: { string: b.string.loose, limit: b.any.optional },
        returns: newArrayOf(b.any),
        uses: {
          this: [
            b.retained,
            runsMembers(
              b.named(...namesReadByConstruct, "lastIndex", "length"),
              b.calls("exec"),
              "OwnValues",
              "Species",
            ),
          ],
          limit: [b.runs("ToPrimitive")],
        },
        usesFeature: ["RegExp", "RegExpFromString", "RegExpUnicodeProperties"],
      }),
    },
  }),
  b.extend("RegExp.prototype", { usesFeature: "RegExp" }),

  b.method("String.prototype.match", [
    {
      this: stringReceiver,
      args: { regexp: pristineRegExp },
      returns: matchResult,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      readsStateBeforeConversions: true,
      uses: { regexp: usesLastIndex },
      overloadUsesFeature: "RegExp",
      prefixForm: "StringMatchPrefix",
      implementation: b.inline("StringMatch", { otherwise: b.hostCall }),
    },
    invokeSymbolMethod(b.calls("@@match")),
  ]),
  b.method("String.prototype.matchAll", [
    {
      this: stringReceiver,
      args: { regexp: pristineRegExp },
      returns: newMatchIterator,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      readsStateBeforeConversions: true,
      uses: { regexp: [b.readsContents("lastIndex"), b.runsOnContents("ToPrimitive")] },
      overloadUsesFeature: "RegExp",
      implementation: b.hostCall,
    },
    invokeSymbolMethod(b.named("@@match", "flags"), b.calls("@@matchAll")),
  ]),
  b.method("String.prototype.replace", replace("StringReplace", "Once", b.calls("@@replace"))),
  b.method(
    "String.prototype.replaceAll",
    replace("StringReplaceAll", "Repeatedly", b.named("@@match", "flags"), b.calls("@@replace")),
  ),
  b.method("String.prototype.search", [
    {
      this: stringReceiver,
      args: { regexp: pristineRegExp },
      returns: b.int(-1, maximumInt32),
      capability: "heap",
      throws: true,
      readsStateBeforeConversions: true,
      uses: { regexp: b.inspected },
      reads: [state],
      writes: [state],
      overloadUsesFeature: "RegExp",
      implementation: b.inline("StringSearch", { otherwise: b.hostCall }),
    },
    invokeSymbolMethod(b.calls("@@search")),
  ]),
  b.method("String.prototype.split", [
    { ...splitAtString(b.notPassed), substringOf: "this", prefixForm: "StringSplitPrefix" },
    splitAtString(nonNegativeInt32),
    splitAtRegExp(b.notPassed),
    splitAtRegExp(nonNegativeInt32),
    {
      this: stringReceiver,
      args: { separator: b.primitiveValue.optional, limit: b.u32.wrap.optional },
      returns: newArrayOf(b.string).dense,
      capability: "heap",
      throws: true,
      uses: { separator: characters },
      implementation: b.hostCall,
    },
    {
      this: b.any,
      args: { separator: b.any.optional, limit: b.any.optional },
      returns: b.any,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: { this: escapesToOperandCode, separator: [runsMembers(b.calls("@@split"))], limit: escapesToOperandCode },
      reads: [state],
      writes: [state],
      overloadUsesFeature: ["RegExp", "RegExpFromString", "RegExpUnicodeProperties"],
      implementation: b.hostCall,
    },
  ]),

  // `import "core-js"` replaces it without a test, from 3.33 to 3.37.
  b.replacedByPackages("RegExp.escape"),
);
