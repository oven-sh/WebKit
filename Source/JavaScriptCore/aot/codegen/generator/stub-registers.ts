import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const cpus = ["X86_64", "ARM64"];
const self = "aot/codegen/generator/stub-registers.ts";
const fewestListsJudged = 30;

type Tables = Map<string, Map<string, string>>;
type Names = Map<string, string | undefined>;
type Signature = { operands: string[]; result: string | undefined };
type RegistersByField = { struct: string; fields: Names };
type Body = { name: string; header: string; line: number; lines: string[]; names: Names; objects: Map<string, RegistersByField> };

function tablesIn(text: string): Tables {
  const listBehind = (beginning: string): string[] => {
    const found = new RegExp(`${beginning} \\{([^}]*)\\}`).exec(text);
    if (!found) throw new Error(`AOTStubRegisters.h: no "${beginning} { .. }": ${self} no longer reads it as it is written`);
    return found[1]!.split(",").map(entry => entry.trim());
  };
  const names = listBehind("enum class StubRegister : uint8_t");
  return new Map(
    cpus.map(cpu => {
      const numbers = listBehind(`stubRegistersOn${cpu}`);
      if (numbers.length !== names.length) throw new Error(`AOTStubRegisters.h: ${names.length} names and ${numbers.length} registers on ${cpu}`);
      return [cpu, new Map(names.map((name, place) => [name, numbers[place]!]))] as const;
    }),
  );
}

function linesFor(text: string, cpu: string): string[] {
  const open: { now: boolean | undefined; wasTaken: boolean }[] = [];
  return text.split("\n").map(line => {
    const directive = /^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$/.exec(line);
    if (!directive) return open.every(({ now }) => now !== false) ? line : "";
    const [, word, condition] = directive;
    const asked = /^\s*(!?)\s*CPU\((\w+)\)\s*$/.exec(condition!);
    const holds = asked && cpus.includes(asked[2]!) ? (asked[2] === cpu) !== (asked[1] === "!") : undefined;
    const last = open[open.length - 1];
    if (word === "endif") open.pop();
    else if (word === "elif" && last) {
      last.now = last.wasTaken ? false : holds;
      last.wasTaken ||= holds === true;
    } else if (word === "else" && last) last.now = last.wasTaken ? false : last.now === undefined ? undefined : true;
    else if (word !== "elif" && word !== "else") open.push({ now: holds, wasTaken: holds === true });
    return "";
  });
}

function closing(text: string, open: number): number {
  let depth = 0;
  for (let at = open; at < text.length; ++at) {
    if ("([{".includes(text[at]!)) ++depth;
    else if (")]}".includes(text[at]!) && !--depth) return at;
  }
  return text.length;
}

function splitArguments(text: string): string[] {
  const found: string[] = [];
  let start = 0;
  for (let at = 0; at < text.length; ++at) {
    if ("([{".includes(text[at]!)) at = closing(text, at);
    else if (text[at] === ",") {
      found.push(text.slice(start, at).trim());
      start = at + 1;
    }
  }
  const last = text.slice(start).trim();
  return last ? [...found, last] : found;
}

function bodiesIn(lines: string[]): Body[] {
  const bodies: Body[] = [];
  for (let start = 1; start < lines.length; ++start) {
    if (lines[start] !== "{") continue;
    let end = start + 1;
    while (end < lines.length && lines[end] !== "}") ++end;
    const header = lines[start - 1]!;
    const name = /(\w+)\(/.exec(header)?.[1];
    if (name) bodies.push({ name, header, line: start + 2, lines: lines.slice(start + 1, end), names: new Map(), objects: new Map() });
    start = end;
  }
  return bodies;
}

function signaturesIn(text: string): Map<string, Signature> {
  const found = new Map<string, Signature>();
  for (const [, name, expression] of text.matchAll(/^StubSignature signatureOf(\w+)\([^)]*\)\s*\{\s*return ([^;]*);\s*\}$/gm)) {
    const made = /^(StubSignature::withGPRResult|StubSignature::withoutResult|signatureWithResumeMode)\(\{ ((?:gprOf\(StubRegister::\w+\)(?:, )?)*) \}\)((?:\.\w+\(\d*\))*)$/.exec(expression!);
    if (!made || made[3]!.includes("withResultIn")) continue;
    found.set(name!, { operands: [...made[2]!.matchAll(/StubRegister::(\w+)/g)].map(match => match[1]!), result: made[1]!.endsWith("withoutResult") ? undefined : "R0" });
  }
  return found;
}

function structsIn(text: string): Map<string, string[]> {
  const found = new Map<string, string[]>();
  for (const [, name, fields] of text.matchAll(/^struct (\w+) \{ GPRReg ([\w, ]+); \};?$/gm)) found.set(name!, fields!.split(",").map(field => field.trim()));
  return found;
}

function aliasesIn(texts: Iterable<string>): Names {
  const found: Names = new Map();
  for (const text of texts) {
    for (const [, name, register] of text.matchAll(/\bconstexpr StubRegister (\w+) = StubRegister::(\w+);/g))
      found.set(name!, found.has(name!) && found.get(name!) !== register ? undefined : register);
  }
  return found;
}

function lookupIn(body: Body, signature: Signature | undefined, aliases: Names, known: Map<string, string>) {
  return (expression: string): string | undefined => {
    const named = /^(?:jit\.(?:allocateScratch|reuseScratch|savedScratch|clobberRegister)|gprOf)\((StubRegister::)?(\w+)\)$/.exec(expression);
    if (named) return named[1] ? (known.has(named[2]!) ? named[2] : undefined) : aliases.get(named[2]!);
    const operand = /^jit\.(?:operand|clobberOperand)\((\d+)\)$/.exec(expression);
    if (operand) return signature?.operands[Number(operand[1])];
    if (expression === "jit.result()") return signature?.result;
    const member = /^(\w+)\.(\w+)$/.exec(expression);
    if (member) return body.objects.get(member[1]!)?.fields.get(member[2]!);
    return /^\w+$/.test(expression) ? body.names.get(expression) : undefined;
  };
}

function readNames(body: Body, registerOf: (expression: string) => string | undefined, structs: Map<string, string[]>): void {
  const doubtful = new Set<string>();
  const set = (name: string, register: string | undefined) => {
    if (body.names.has(name) && body.names.get(name) !== register) doubtful.add(name);
    body.names.set(name, register);
  };
  for (let place = 0; place < body.lines.length; ++place) {
    const line = body.lines[place]!.trim();
    const declared = /^(?:const |constexpr |static )*(?:GPRReg|auto) (\w+) = (.+);$/.exec(line);
    if (declared) {
      set(declared[1]!, registerOf(declared[2]!));
      continue;
    }
    const taken = /^const auto \[([^\]]+)\] = (\w+);$/.exec(line);
    if (taken) {
      const object = body.objects.get(taken[2]!);
      const fields = object && structs.get(object.struct);
      taken[1]!.split(",").forEach((name, position) => set(name.trim(), fields ? object.fields.get(fields[position]!) : undefined));
      continue;
    }
    const made = /^const (\w+) (\w+) \{$/.exec(line);
    if (!made || !structs.has(made[1]!)) continue;
    const fields: Names = new Map();
    while (body.lines[place + 1]?.trim().startsWith(".")) {
      const field = /^\.(\w+) = (.+),$/.exec(body.lines[++place]!.trim());
      if (field) fields.set(field[1]!, registerOf(field[2]!));
    }
    body.objects.set(made[2]!, { struct: made[1]!, fields });
  }
  for (const name of doubtful) body.names.set(name, undefined);
}

function handObjectsDown(bodies: Body[], structs: Map<string, string[]>): void {
  const textOf = (fields: Names) => JSON.stringify([...fields].sort());
  for (const callee of bodies) {
    const open = callee.header.indexOf("(");
    splitArguments(callee.header.slice(open + 1, closing(callee.header, open))).forEach((parameter, position) => {
      const typed = /^const (\w+)& (\w+)$/.exec(parameter);
      if (!typed || !structs.has(typed[1]!)) return;
      const given: (RegistersByField | undefined)[] = [];
      for (const caller of bodies) {
        const text = caller.lines.join("\n");
        for (const call of text.matchAll(new RegExp(`\\b${callee.name}\\(`, "g"))) {
          const start = call.index! + call[0].length;
          const argument = splitArguments(text.slice(start, closing(text, start - 1)))[position];
          given.push(argument === undefined ? undefined : caller.objects.get(argument));
        }
      }
      const first = given[0];
      if (first && given.every(object => object && textOf(object.fields) === textOf(first.fields))) callee.objects.set(typed[2]!, first);
    });
  }
}

function judge(files: Map<string, string>, tables: Tables): { problems: string[]; listsJudged: number } {
  const problems = new Set<string>();
  let listsJudged = 0;
  for (const cpu of cpus) {
    const registers = tables.get(cpu)!;
    const others = tables.get(cpus.find(other => other !== cpu)!)!;
    const texts = new Map([...files].map(([file, text]) => [file, linesFor(text, cpu).join("\n")]));
    const aliases = aliasesIn(texts.values());
    for (const [file, text] of texts) {
      const bodies = bodiesIn(text.split("\n"));
      const signatures = signaturesIn(text);
      const structs = structsIn(text);
      const lookups = new Map(bodies.map(body => [body, lookupIn(body, signatures.get(body.name.replace(/^generate/, "")), aliases, registers)]));
      for (const body of bodies) readNames(body, lookups.get(body)!, structs);
      handObjectsDown(bodies, structs);
      for (const body of bodies) {
        body.names.clear();
        readNames(body, lookups.get(body)!, structs);
        const lines = body.lines.join("\n");
        for (const assertion of lines.matchAll(/\b(?:RELEASE_ASSERT|ASSERT|static_assert)\(/g)) {
          const open = assertion.index! + assertion[0].length - 1;
          const condition = lines.slice(open + 1, closing(lines, open));
          if (/\|\||\?|!noOverlap/.test(condition)) continue;
          for (const list of condition.matchAll(/\bnoOverlap\(/g)) {
            const start = list.index! + list[0].length;
            const named = splitArguments(condition.slice(start, closing(condition, start - 1)))
              .map(name => ({ name, register: lookups.get(body)!(name) }))
              .filter(({ register }) => register !== undefined && registers.get(register) !== "noMachineRegister");
            listsJudged += named.length > 1 ? 1 : 0;
            named.forEach((one, position) => {
              for (const other of named.slice(position + 1)) {
                if (one.register === other.register || registers.get(one.register!) !== registers.get(other.register!) || others.get(one.register!) === others.get(other.register!)) continue;
                const line = body.line + lines.slice(0, assertion.index).split("\n").length - 1;
                problems.add(`aot/stubs/${file}:${line}: ${one.name} (${one.register}) and ${other.name} (${other.register}) are one register on ${cpu} alone: the assertion fails there when the stub is generated. Take another register, and say areDistinctOnEveryISA().`);
              }
            });
          }
        }
      }
    }
  }
  return { problems: [...problems], listsJudged };
}

function noticesClashes(tables: Tables): boolean {
  const sample = (declaration: string) =>
    new Map([
      [
        "Sample.cpp",
        [
          "struct RestoredSample { GPRReg saved, mode, value, count, scratch; };",
          "static void emitSample(StubAssembler& jit, const RestoredSample& frame)",
          "{",
          "    const auto [saved, mode, value, flags, resumeAddress] = frame;",
          declaration,
          "    RELEASE_ASSERT(noOverlap(saved, mode, value, flags, resumeAddress, other));",
          "}",
          "StubSignature signatureOfSample(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }",
          "void generateSample(StubAssembler& jit)",
          "{",
          "    const RestoredSample frame {",
          "        .saved = jit.clobberOperand(0),",
          "        .mode = jit.clobberOperand(1),",
          "        .value = jit.clobberOperand(2),",
          "        .count = jit.allocateScratch(StubRegister::T9),",
          "        .scratch = jit.allocateScratch(StubRegister::T11),",
          "    };",
          "    emitSample(jit, frame);",
          "}",
        ].join("\n"),
      ],
    ]);
  const taken = (register: string) => `    const GPRReg other = jit.reuseScratch(StubRegister::${register});`;
  const found = (declaration: string) => judge(sample(declaration), tables).problems;
  const says = (problems: string[], cpu: string) => problems.length === 1 && problems[0]!.includes(`on ${cpu} alone`);
  return (
    says(found(taken("R0")), "ARM64") &&
    says(found(taken("T12")), "X86_64") &&
    !found(taken("T10")).length &&
    !found(`#if CPU(X86_64)\n${taken("R0")}\n#else\n${taken("T10")}\n#endif`).length
  );
}

export function clashesOfStubRegisters(stubs: string): string[] {
  const names = readdirSync(stubs).filter(name => /\.(?:cpp|h)$/.test(name));
  const files = new Map(names.sort().map(name => [name, readFileSync(join(stubs, name), "utf8")]));
  const tables = tablesIn(files.get("AOTStubRegisters.h")!);
  const { problems, listsJudged } = judge(files, tables);
  if (!noticesClashes(tables)) problems.push(`${self} does not notice two names of one register: it checks nothing.`);
  if (listsJudged < fewestListsJudged) problems.push(`${self} judges ${listsJudged} lists of registers, fewer than ${fewestListsJudged}: it no longer reads the stubs as they are written.`);
  return problems;
}

if (process.argv[1]?.endsWith("stub-registers.ts")) {
  const problems = clashesOfStubRegisters(process.argv[2]!);
  for (const problem of problems) console.error(problem);
  process.exit(problems.length ? 1 : 0);
}
