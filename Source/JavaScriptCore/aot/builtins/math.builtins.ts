import * as b from "bun-builtins-table";

const number = b.f64.loose;
const int32 = b.i32.wrap;

const pure = (
  args: Readonly<Record<string, b.TableType>>,
  returns: b.TableType,
  implementation: b.ImplementationSpec,
) => ({ args, returns, capability: "none", deterministic: true, implementation }) satisfies b.FunctionField;

const unary = (inlineBuiltin: string) => pure({ x: number }, b.f64, b.inline(inlineBuiltin));

const libraryFunction = (kind: string) => pure({ x: number }, b.f64, b.inline("MathFunction", { parameters: [kind] }));

const unaryWithRange = (inlineBuiltin: string, rule: b.RangeRule) =>
  ({ ...unary(inlineBuiltin), range: { rule, operand: "x" } }) satisfies b.FunctionField;

const rounding = (inlineBuiltin: string) => unaryWithRange(inlineBuiltin, "IntegerPart");

const extremum = (inlineBuiltin: string, listForm: string, rule: b.RangeRule) =>
  ({
    ...pure({ values: number.rest }, b.f64, b.inline(inlineBuiltin)),
    range: { rule },
    listForm,
  }) satisfies b.FunctionField;

export default b.declarations(
  b.namespace(
    "Math",
    {
      E: b.constant(b.checked(Math.E, "std::numbers::e")),
      LN10: b.constant(b.checked(Math.LN10, "std::numbers::ln10")),
      LN2: b.constant(b.checked(Math.LN2, "std::numbers::ln2")),
      LOG10E: b.constant(b.checked(Math.LOG10E, "std::numbers::log10e")),
      LOG2E: b.constant(b.checked(Math.LOG2E, "std::numbers::log2e")),
      PI: b.constant(b.checked(Math.PI, "std::numbers::pi")),
      SQRT1_2: b.constant(b.checked(Math.SQRT1_2, "std::numbers::sqrt2 / 2")),
      SQRT2: b.constant(b.checked(Math.SQRT2, "std::numbers::sqrt2")),

      abs: unaryWithRange("MathAbs", "AbsoluteValue"),
      acos: libraryFunction("ACos"),
      acosh: libraryFunction("ACosh"),
      asin: libraryFunction("ASin"),
      asinh: libraryFunction("ASinh"),
      atan: libraryFunction("ATan"),
      atan2: pure({ y: number, x: number }, b.f64, b.inline("MathATan2")),
      atanh: libraryFunction("ATanh"),
      cbrt: libraryFunction("Cbrt"),
      ceil: rounding("MathCeil"),
      clz32: pure({ x: int32 }, b.int(0, 32), b.inline("MathClz32")),
      cos: libraryFunction("Cos"),
      cosh: libraryFunction("Cosh"),
      exp: libraryFunction("Exp"),
      expm1: libraryFunction("Expm1"),
      f16round: unary("MathF16Round"),
      floor: rounding("MathFloor"),
      fround: unary("MathFround"),
      hypot: pure({ values: number.rest }, b.f64, b.inline("MathHypot", { max: 3, otherwise: b.hostCall })),
      imul: pure({ x: int32, y: int32 }, b.i32, b.inline("MathImul")),
      log: libraryFunction("Log"),
      log10: libraryFunction("Log10"),
      log1p: libraryFunction("Log1p"),
      log2: libraryFunction("Log2"),
      max: extremum("MathMax", "MathMaxOfArray", "MaximumOfArguments"),
      min: extremum("MathMin", "MathMinOfArray", "MinimumOfArguments"),
      pow: pure({ base: number, exponent: number }, b.f64, b.inline("MathPow")),
      random: {
        returns: b.f64,
        capability: "none",
        reads: ["RuntimeState"],
        writes: ["RuntimeState"],
        implementation: b.inline("MathRandom"),
      },
      round: rounding("MathRound"),
      sign: unaryWithRange("MathSign", "Sign"),
      sin: libraryFunction("Sin"),
      sinh: libraryFunction("Sinh"),
      sqrt: unary("MathSqrt"),
      sumPrecise: {
        args: { items: b.any },
        returns: b.f64,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { items: [b.runs("Iterate"), b.readsContents()] },
        usesFeature: "MathSumPrecise",
        nativeSymbols: ["mathProtoFuncSumPrecise"],
        implementation: b.hostCall,
      },
      tan: libraryFunction("Tan"),
      tanh: libraryFunction("Tanh"),
      trunc: rounding("MathTrunc"),
    },
    { locksMembers: true, mutability: "AlwaysLocked" },
  ),

  // Libraries for seeded random numbers and tests replace Math.random. sylvester, below natural, needs a Math.sign that answers 1
  // for 0. core-js 2 replaces Math.sinh after a test that never passes, and `import "core-js"` Math.sumPrecise from 3.37 to 3.39.
  b.replacedByPackages("Math.random", "Math.sign", "Math.sinh", "Math.sumPrecise"),
);
