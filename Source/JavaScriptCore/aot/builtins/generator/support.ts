import { mkdirSync, readFileSync, realpathSync, renameSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";
import { fileURLToPath } from "node:url";

export function requireRuntime(): void {
  if ("Bun" in globalThis) return;
  const [major, minor] = process.versions.node.split(".").map(Number);
  if (major > 24 || (major === 24 && minor >= 2) || (major === 22 && minor >= 18)) return;
  console.error(
    `error: the generators of aot/ need node 24.2 or later, or 22.18 or later of 22: this is ${process.versions.node}`,
  );
  process.exit(1);
}

export function isProgram(url: string): boolean {
  const started = process.argv[1];
  return started !== undefined && realpathSync(started) === realpathSync(fileURLToPath(url));
}

export function cap(text: string): string {
  return text[0].toUpperCase() + text.slice(1);
}

export function readFlags(names: readonly string[]): Record<string, string | boolean | undefined> {
  const flags: Record<string, string | boolean | undefined> = {};
  for (const argument of process.argv.slice(2)) {
    const separator = argument.indexOf("=");
    const name = argument.slice(2, separator === -1 ? undefined : separator);
    if (!argument.startsWith("--") || !names.includes(name)) {
      console.error(`error: unknown argument: ${argument}`);
      process.exit(1);
    }
    flags[name] = separator === -1 ? true : argument.slice(separator + 1);
  }
  return flags;
}

export function writeIfChanged(file: string, written: string): void {
  const text = `${written.replaceAll("\r\n", "\n").trim()}\n`;
  try {
    if (readFileSync(file, "utf8") === text) return;
  } catch {}
  mkdirSync(dirname(file), { recursive: true });
  const temporary = `${file}.${process.pid}.tmp`;
  writeFileSync(temporary, text);
  renameSync(temporary, file);
}

const setPattern = (name: string): RegExp => new RegExp(String.raw`^set\(${name}\s([^)]*)\)`, "m");

export function readCMakeList(file: string, name: string, variables: Readonly<Record<string, string>> = {}): string[] {
  const words = setPattern(name).exec(readFileSync(file, "utf8"))?.[1];
  if (words === undefined) return [];
  return words
    .split(/\s+/)
    .filter(word => word !== "")
    .map(word => word.replace(/\$\{(\w+)\}/g, (reference, variable: string) => variables[variable] ?? reference));
}
