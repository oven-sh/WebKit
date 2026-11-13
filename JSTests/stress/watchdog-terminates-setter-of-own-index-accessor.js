//@ runDefault("--watchdog=300", "--watchdog-exception-ok")

// The watchdog ends the setter that a store to an own index accessor calls. This test should not time out.
({ set 0(value) { for (;;) { } } })[0] = 1;
