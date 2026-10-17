// The same function expression evaluated before and after this module's linked code was dropped and linked again.
export const makers = [];
for (let round = 0; round < 2; ++round) {
    makers.push((x) => x * 2 + round);
    globalThis.moduleSharedFunctionExpressionsRound = round;
    await globalThis.moduleSharedFunctionExpressionsGates[round];
}
