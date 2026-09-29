import four from "./chain_four.py";
export const made = new Map();
export const all = () => [made, ...four.all()];
