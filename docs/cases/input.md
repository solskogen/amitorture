# input (module 15, AMT_PH_BARE, disk group "chip")

Keyboard, mouse/joystick counters, pot ports and the CD32 pad. References: Amiga Hardware Reference Manual
(HRM) 3rd ed., chapter 8 "Interface Hardware" (controller ports, keyboard) and appendix A (JOY0DAT/JOY1DAT,
JOYTEST, POTGO/POTINP, POT0DAT/POT1DAT); appendix F (8520). The CD32 pad protocol is not in the HRM: it is the
one lowlevel.library ReadJoyPort() uses (the pad's 74xx165 shift register: port pin 5 driven low selects shift
mode, pin 6 /FIR is the clock driven by CIA-A, pin 9 carries the data).

Events the CPU cannot cause go through the host request channel (CONTRACT §13): `key`, `mouse`, `joy`,
`cd32pad`. A `wait` probe goes first: when the host does not acknowledge it, no further request is sent and
every host case is `untestable` (each request would cost a 5 s timeout). After the first unacknowledged
request of a kind, later requests of that kind are not sent either (`ctx.sent` = 0, `ctx.note` says why).
After an acknowledged request the module waits for the event (1 s for key bytes and button levels, up to
3 s for mouse counters until they are stable), then reports exact values; latencies go to `timing`.

Safety and restore: CIA-A through `modules/cia_hw.h` (CRA/CRB, latches, ICR mask). CIA-A DDRA bit 7 (/FIR1 output as the CD32 clock) is only set inside a CD32 read the host
acknowledged; DDRA is restored; PRA writes keep the LED bit and write OVL = 0. POTGO is left at $FF00 (all pot
pins outputs, high), which is what Kickstart 2.04+ and AROS write at boot (POTGO is write-only). JOY0DAT is
restored through JOYTEST, so the OS pointer does not jump. Keys injected are pressed one at a time (plus one
A+S rollover); never Ctrl-Amiga-Amiga, never caps lock ($62: the LED key toggles and is not observable),
never $68-$7F (reset warning $78, CDTV keys). Keyboard lost-sync recovery is out of scope.

## Requests sent (the host must implement these, CONTRACT §13)
- `wait` `{}`
- `key` `{"code":n,"up":0|1}`: n = Amiga raw key code 0..$67
- `mouse` `{"dx":n,"dy":n,"buttons":m,"port":0}`: dx/dy in mouse counts (one count = one quadrature step;
  +dx right, +dy down/toward the user); buttons absolute: bit0 left, bit1 right, bit2 middle. The host must
  deliver exactly dx/dy counts (Amiberry: mouse speed 100, no tablet/mousehack, joyport0=mouse).
- `joy` `{"port":1,"dirs":"<subset of udlr>","fire":b}`: b bit0 = fire 1 (pin 6), bit1 = fire 2 (pin 9); state is
  absolute (an empty dirs string = centred).
- `cd32pad` `{"port":1,"buttons":mask}`: mask bit0 blue, 1 red, 2 yellow, 3 green, 4 forward, 5 reverse,
  6 play; absolute. A host that can attach a CD32 pad but not press its buttons may ack `buttons:0` only:
  that makes `cd32_id_released` testable.

## Cases

| case | host | intent | rule (reference) | a failure usually means |
|---|---|---|---|---|
| `joytest_readback` | no | JOYTEST with 12 fixed + 8 seeded patterns (2 in quick); JOY0DAT and JOY1DAT & $FCFC = pattern & $FCFC | HRM A JOYTEST: writes Y7-Y2 (data 15-10) and X7-X2 (data 7-2) of all four counters | JOYTEST not implemented, wrong bits, or only one port written |
| `joytest_low_bits_unchanged` | no | bits 1-0 of every counter are the same before and after each JOYTEST write | HRM A JOYTEST: bits 1-0 are not written (they follow the quadrature inputs) | JOYTEST writes all 8 bits |
| `joytest_hot` | no | the JOYTEST/JOYxDAT loop repeated (amt_hot): mismatch signature 0 on every iteration | as above | JIT and interpreter disagree on custom register access, or a counter drifts |
| `potinp_out_readback_single` | no | each pot pin alone as output (OUTxx=1) with DATxx 1 then 0, the others inputs: POTINP reads the driven level | HRM A POTGO/POTINP: OUTxx = output enable, DATxx = the pin's data, POTINP reads the pins | output enable ignored, or POTINP returns the written POTGO bits / the input state |
| `potinp_out_readback_multi` | no | all pins outputs with both pin 5s high and the pin 9s in all combinations; then pin 5s in all combinations with pin 9s as inputs | as above (a pin 5 driven low selects CD32 pad shift mode, so pin 9 is never an output then) | pins not independent |
| `pot_counters_start` | no | POTGO=$0001 (all inputs, START); POT0DAT/POT1DAT sampled 0, 2, 10, 313, 626 lines later. Always passes: the values are in `timing` only | HRM A POTGO START: dump the capacitors, start the counters; the counts with nothing connected depend on the hardware | (timing only: compare across emulators / CE vs fast) |
| `kbd_key_down_up` | key | A ($20) down then up: SDR = $BF, then $BE | HRM ch. 8: the code is sent rotated left (bits 6..0, then the up flag) and inverted: SDR = ~((code<<1)\|(code>>7)) | wrong bit order or polarity of the keyboard serial data |
| `kbd_keycode_set` | key | 62 keys (every row, keypad, cursor, F-keys, help, modifiers; every 4th in quick), each down and up, exact SDR | as above | a key code mapped wrong, or bytes lost; `got.bad` lists [code, SDR down, SDR up] |
| `kbd_no_spurious_bytes` | key | no further byte within 3 frames after each key's up code | HRM ch. 8: one code per transition; no typematic repeat in the keyboard | duplicated codes, auto-repeat, or lost-sync codes because a handshake was not seen |
| `kbd_rollover_order` | key | A down, S down, S up, A up give $20,$21,$A1,$A0 in order | HRM ch. 8 | events reordered or merged |
| `kbd_hold_until_handshake` | key | B ($35) down is received but not acknowledged; B up is injected; for 30 ms no new SP and SDR unchanged; after the handshake the up code ($B5) arrives | HRM ch. 8: the keyboard waits (up to 143 ms) for the handshake before it sends the next code | the next code overwrites SDR without a handshake |
| `mouse_move_right` / `_left` / `_down` / `_up` | mouse | dx=+10 / -10, dy=+10 / -10: JOY0DAT X/Y 8-bit deltas exactly that | HRM ch. 8 mouse: X counts up moving right, Y counts up moving down (toward the user) | wrong direction, scaling, or lost counts |
| `mouse_move_diagonal` | mouse | dx=+3, dy=-5 at once | as above, the counters are independent | axes mixed |
| `mouse_move_large` | mouse | dx=+100, dy=-100 (60 in quick) | as above; < 128 counts between reads keeps the delta unambiguous | counts clamped or lost |
| `mouse_wrap_x` | mouse | JOYTEST $80FC (X=$FC..$FF), dx=+8: X wraps mod 256, Y unchanged | HRM: independent 8-bit counters, no carry from X into Y | carry into Y, or saturation |
| `mouse_wrap_y` | mouse | JOYTEST $0080 (Y=$00..$03), dy=-8: Y wraps below 0, X unchanged | as above | borrow into X, or saturation |
| `mouse_left_button` | mouse | CIA-A PRA bit 6 (/FIR0) 1, 0 while pressed, 1; JOY0DAT unchanged | HRM ch. 8: left button = port 0 pin 6 = /FIR0, active low | wrong bit/polarity |
| `mouse_right_button` | mouse | POTGO=$FF00: POTINP bit 10 DATLY 1, 0 while pressed, 1 | HRM ch. 8 / A: right button = port 0 pin 9; an output driven high is pulled low by the button | output-high pin not overridden by the button |
| `mouse_right_button_input` | mouse | POTGO=$0000 (pin 9 input): DATLY = 0 while pressed (the released level floats: not checked) | HRM A POTGO | button not seen in input mode |
| `mouse_middle_button` | mouse | POTGO=$FF00: POTINP bit 8 DATLX 1, 0 while pressed, 1 | HRM ch. 8: middle button = port 0 pin 5 | middle button not wired to pin 5 |
| `joy_directions` | joy | port 1, centred and the 8 directions: decoded right=X1, left=Y1, down=X1^X0, up=Y1^Y0 | HRM ch. 8 joystick | wrong decode bits |
| `joy_fire` | joy | port 1 fire: CIA-A PRA bit 7 (/FIR1) 1, 0, 1 | HRM ch. 8 | wrong bit/polarity |
| `joy_fire2` | joy | port 1 second fire: POTINP bit 14 DATRY (POTGO=$FF00) 1, 0, 1 | HRM ch. 8 / A | second button not on pin 9 |
| `cd32_id_released` | cd32pad | buttons=0: the 9 reads are $0FF (7 released buttons = 1, then 1, then 0) | lowlevel.library CD32 pad protocol | no pad, or wrong shift sequence / id bits |
| `cd32_buttons_single` | cd32pad | each of the 7 buttons alone: only its read is 0 | as above, order blue, red, yellow, green, forward, reverse, play | button order wrong |
| `cd32_buttons_combo` | cd32pad | masks $7F, $55, $2A | as above | buttons interfere |
| `cd32_normal_mode` | cd32pad | red+blue pressed with pin 5 high: PRA bit 7 = 0 and DATRY = 0; released: both 1 | CD32 pad 2-button mode | normal mode not emulated |
