# rt (module id 1, AMT_PH_BARE)

Self-test of the bare-metal runtime (`disk/rt/`): the services every other bare module relies on. It runs first on the core disk, so a failure here explains failures elsewhere. Source: `disk/modules/rt.c`.

| case | intent | rule | expected | a failure usually means |
|---|---|---|---|---|
| `vbl_clock` | the runtime's VBL counter and `amt_ticks()` advance in the bare phase | HRM: VERTB (INTREQ bit 5) is raised once per frame and taken at level 3 | at least 3 frames counted while waiting for them; ticks increasing | VERTB not delivered or not at level 3; the beam counters (VPOSR/VHPOSR) not moving |
| `catchall_illegal` | ILLEGAL ($4AFC) inside a nested `amt_try` reaches the runtime's catch-all and unwinds to the inner try only | MC68000 PRM: ILLEGAL takes vector 4 | abort code "unexpected exception", vector 4 | exception dispatch through the VBR wrong (68010+: VBR not used) |
| `try_restores_sr` | after that abort, `amt_try` has restored the interrupt mask | the runtime's contract (CONTRACT §10): modules start at SR = $2000 | SR interrupt mask 0 | the RTE/stack unwinding of the emulated exception frame is wrong |
| `own_vector_trap0` | a handler a module installs with `amt_vec_set` runs | PRM: TRAP #0 takes vector 32 | the handler ran exactly twice for two TRAP #0 | VBR-relative vector fetch wrong |
| `hot_repeat` | a pure integer loop gives the same result on every iteration of `amt_hot()` | determinism: the same code on the same data gives the same result | every iteration equals the first | with the JIT on: the compiled block differs from the interpreted first run; with it off: state leaking between iterations |

`hot_repeat` has no expected value of its own; the first iteration is the reference. That is the point of the case: with the JIT on, the first run is interpreted and the later ones compiled, so a difference is a miscompile. JIT/interpreter parity (the twin entries) checks the value itself.
