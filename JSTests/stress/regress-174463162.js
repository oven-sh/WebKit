// Bun: the helper below scribbles proto's header while main() still holds proto, so a collection before the script ends
// marks a cell with no Structure. Only the two modes that pass --collectContinuously=true collect that early.
//@ $skipModes << "ftl-eager-no-cjit".to_sym
//@ $skipModes << "no-cjit-collect-continuously".to_sym

// $vm.installPropertyInlineCacheClearingWatchpointWithDeadOwner creates the IC
// watchpoint and sets the owner as dead, simulating the state between GC
// marking-end and CodeBlock sweep.

function main() {
    let proto = $vm.createCustomTestGetterSetterWithSharedStructure();
    let sibling = $vm.createCustomTestGetterSetterWithSharedStructure();

    $vm.installPropertyInlineCacheClearingWatchpointWithDeadOwner(proto, "customAccessor");

    sibling.triggerTransition = true;
}
main();
