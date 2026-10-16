#pragma once

// Test-only child for the stack SIGKILL scenario (stage 9.2). It takes the
// deploy lock, writes one journal entry per group member and their artifacts,
// then pauses until the parent's timeout SIGKILLs it — the kernel drops the
// flock, which is what leaves "entry present AND lock free" as the truthful
// interrupted signal (§6.7). Defined in tests/deploy_stack_test.cpp; the test
// binary's main dispatches to it.
int stack_kill_child_main(int argc, char** argv);
