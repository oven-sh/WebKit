import second from "./cycle_second.py";
export const early = "set before";
export function hoisted() { return "hoisted"; }
export const secondWhenThisWasRun = Object.keys(second).filter(name => !name.startsWith("_"));
