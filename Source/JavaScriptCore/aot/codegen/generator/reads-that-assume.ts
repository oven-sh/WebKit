import { mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { parseArgs } from "node:util";
import { clashesOfStubRegisters } from "./stub-registers.ts";

const listFile = "aot/codegen/AOTReadsThatAssume.txt";
const stampFile = "AOTReadsThatAssumeChecked.h";

const watched: { name: string; pattern: RegExp; unless?: RegExp; notOnLineWith?: RegExp; instead: string }[] = [
  {
    name: "JSString_value",
    pattern: /\bJSString_value\b/g,
    instead: "StringEmitter::viewOf(), lengthOf(), viewOfString(), isRopeString(), or implOfFlatString() with a FlatStringCell",
  },
  {
    name: "viewOfFlatString",
    pattern: /\bviewOfFlatString\s*\(/g,
    instead: "StringEmitter::viewOf(): this one trusts its caller that isRopeString() of the same cell has just answered no",
  },
  {
    name: "offsetOfValue untested",
    pattern: /\bJSString\s*::\s*offsetOfValue\s*\(\s*\)/g,
    unless:
      /\bbranchIf(?:Not)?RopeStringImpl\b|\bbranchTest(?:Ptr|8|32|64)\s*\([^;]*\bisRopeInPointer\b|\bloadStringLength\s*\(/,
    notOnLineWith: /\bstatic_assert\s*\(/,
    instead: "a branch on the rope bit within three lines behind the load (branchIfRopeStringImpl()), or loadStringLength()",
  },
];

type Counts = Map<string, number>;

function keyOf(name: string, file: string): string {
  return `${name} | ${file}`;
}

function sourcesBelow(directory: string, prefix: string): string[] {
  return readdirSync(directory, { withFileTypes: true })
    .sort((a, b) => (a.name < b.name ? -1 : 1))
    .flatMap(entry => {
      if (entry.isDirectory()) return sourcesBelow(join(directory, entry.name), `${prefix}${entry.name}/`);
      return entry.isFile() && /\.(?:cpp|h)$/.test(entry.name) ? [`${prefix}${entry.name}`] : [];
    });
}

function countIn(text: string, { pattern, unless, notOnLineWith }: (typeof watched)[number]): number {
  if (!unless && !notOnLineWith) return text.match(pattern)?.length ?? 0;
  const lines = text.split("\n");
  let found = 0;
  for (const match of text.matchAll(pattern)) {
    const line = text.slice(0, match.index).split("\n").length - 1;
    if (notOnLineWith?.test(lines[line]!)) continue;
    if (!unless?.test(lines.slice(line, line + 4).join("\n"))) ++found;
  }
  return found;
}

function countPlaces(javascriptcore: string): Counts {
  const counts: Counts = new Map();
  for (const file of sourcesBelow(join(javascriptcore, "aot"), "")) {
    const text = readFileSync(join(javascriptcore, "aot", file), "utf8");
    for (const entry of watched) {
      const found = countIn(text, entry);
      if (found) counts.set(keyOf(entry.name, file), found);
    }
  }
  return counts;
}

function readList(text: string): { listed: Counts; problems: string[] } {
  const listed: Counts = new Map();
  const problems: string[] = [];
  for (const line of text.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const [name, file, count, reason] = line.split(" | ");
    if (!name || !file || !/^\d+$/.test(count ?? "") || !reason?.trim()) problems.push(`${listFile}: not "<name> | <file> | <count> | <reason>": ${line}`);
    else listed.set(keyOf(name, file), Number(count));
  }
  return { listed, problems };
}

function compare(found: Counts, listed: Counts): string[] {
  const problems: string[] = [];
  for (const [key, count] of found) {
    const allowed = listed.get(key) ?? 0;
    if (count === allowed) continue;
    const [name, file] = key.split(" | ");
    if (count > allowed)
      problems.push(
        `aot/${file} has ${count} of ${name}, the list has ${allowed}. It takes for granted what no type says. Use ${watched.find(entry => entry.name === name)!.instead}; or say in ${listFile} why the new place is right.`,
      );
    else problems.push(`aot/${file} has ${count} of ${name}, the list has ${allowed}: lower the count in ${listFile}, or the next place passes unseen.`);
  }
  for (const [key, allowed] of listed) {
    if (!found.has(key)) problems.push(`aot/${key.split(" | ")[1]} has none of ${key.split(" | ")[0]}, the list has ${allowed}: take the line out of ${listFile}.`);
  }
  return problems;
}

function hasTeeth(found: Counts): boolean {
  if (!watched.every(({ name }) => [...found.keys()].some(key => key.startsWith(`${name} | `)))) return false;
  const spaced = (name: string) => (name.startsWith("offsetOfValue") ? "JSString ::\n    offsetOfValue\n    ( )" : `heaps\n    -> ${name}\n    (cell)`);
  if (!watched.every(entry => countIn(spaced(entry.name), entry) === 1)) return false;
  const load = "jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);";
  const untested = watched.find(({ unless }) => unless)!;
  if (countIn(`${load}\n\n\nslow.append(jit.branchIfRopeStringImpl(impl));`, untested) || !countIn(`${load}\n\n\n\nslow.append(jit.branchIfRopeStringImpl(impl));`, untested)) return false;
  const [key, count] = [...found][0] ?? [];
  if (key === undefined || count === undefined) return false;
  const more = new Map(found).set(key, count + 1);
  const fewer = new Map(found).set(key, count - 1);
  const elsewhere = new Map(found).set(keyOf(watched[0]!.name, "codegen/AOTNoSuchFile.cpp"), 1);
  return !compare(found, found).length && [more, fewer, elsewhere].every(changed => compare(changed, found).length === 1 && compare(found, changed).length === 1);
}

const { values } = parseArgs({
  strict: true,
  options: { "javascriptcore": { type: "string" }, "out": { type: "string" }, "print-counts": { type: "boolean" } },
});
if (values.javascriptcore === undefined || (values.out === undefined && !values["print-counts"])) {
  console.error("usage: <bun or node> aot/codegen/generator/reads-that-assume.ts --javascriptcore <Source/JavaScriptCore> (--out <dir> | --print-counts)");
  process.exit(1);
}
const javascriptcore = resolve(values.javascriptcore);
const found = countPlaces(javascriptcore);
if (values["print-counts"]) {
  for (const [key, count] of found) console.log(`${key} | ${count}`);
  process.exit(0);
}
const { listed, problems } = readList(readFileSync(join(javascriptcore, listFile), "utf8"));
if (!hasTeeth(found)) problems.push("aot/codegen/generator/reads-that-assume.ts does not notice a place too many or too few: it checks nothing.");
problems.push(...compare(found, listed));
problems.push(...clashesOfStubRegisters(join(javascriptcore, "aot", "stubs")));
if (problems.length) {
  console.error(problems.join("\n"));
  process.exit(1);
}
mkdirSync(values.out!, { recursive: true });
writeFileSync(join(values.out!, stampFile), "// Written by aot/codegen/generator/reads-that-assume.ts when the sources of the compiler have passed it. Do not edit.\n");
