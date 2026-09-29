import two from "./chain_two.py";
export const made = { by: "chain_one.mjs" };
export const all = () => [made, ...two.all()];
