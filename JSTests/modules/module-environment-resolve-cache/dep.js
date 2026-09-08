export let importedValue = 40;

export function bump()
{
    importedValue += 1;
}

import { cyclicFromLib } from "./lib.js"

export let starValue = 100;

export function readCyclic1() { return cyclicFromLib; }
export function readCyclic2() { return cyclicFromLib; }

// lib.js has not been evaluated yet: these link against its environment and must hit its TDZ.
export let tdzErrorsDuringCycle = 0;
for (const read of [readCyclic1, readCyclic2, readCyclic1]) {
    try {
        read();
    } catch (error) {
        if (error instanceof ReferenceError)
            tdzErrorsDuringCycle += 1;
    }
}
