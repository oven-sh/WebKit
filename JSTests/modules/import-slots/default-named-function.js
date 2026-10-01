export default function named() { return "named"; }
export { named };
export function rename() { named = function replaced() { return "replaced"; }; }
