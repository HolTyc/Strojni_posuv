# CLAUDE.md

Guidance for working in this firmware. It controls a **universal electric machine feed** ("strojní posuv") — a powered axis-feed unit that drives a stepper motor for milling machines, lathes, and similar tools. The operator picks a feed mode and rate on a small GLCD menu, and the unit steps the motor accordingly.

**Status: work in progress.** The motion screens and HW setup are wired, including selectable output/time units, but the mechanical values still need verification on real machines. See [Known TODOs / gotchas](#known-todos--gotchas).

## Hardware

- **MCU:** STM32F103RBTx (Cortex-M3, 128 KB flash, 20 KB RAM), STM32CubeMX/HAL project.
- **Clock:** **16 MHz HSE** × PLL4 = **64 MHz** SYSCLK (`HSE_VALUE` in `stm32f1xx_hal_conf.h`, `RCC.SYSCLKFreq_VALUE` in `Posuv.ioc`). HCLK = 64 MHz, APB1 = 32 MHz, APB2 = 16 MHz, ADC clock = PCLK2/2. **APB1 timer clock is 64 MHz**, not 32 — an APB prescaler ≠ 1 doubles the timer clock back up, which is what `MOTOR_TICKS_PER_S` depends on. `FLASH_LATENCY_2` corroborates the 64 MHz figure; CubeMX only emits it above 48 MHz.
- **Motor driver:** TB6600HG stepper driver. Controlled by bit-banged GPIO: `STEP`, `DIR`, `Enable`, `Reset`, plus microstep-select lines `M1`/`M2`/`M3` and `LTC`. Microstepping is meant to be **adjustable on the device** (not yet implemented — see TODOs).
- **Display:** 128×64 ST7920 graphic LCD over **software SPI** (driver in `Core/Src/ST7920_SERIAL.c`). Runs in graphic mode; the firmware draws into a RAM framebuffer and pushes it with `ST7920_Update()`.
- **Encoder:** rotary encoder on **TIM2 in encoder mode**. Raw `TIM2->CNT` is shifted `>> 2` (4 counts/detent) and inverted centrally by `Encoder_GetSteps()`.
- **Keypad:** 4×4 matrix numeric keypad, physically wired. `Keypad_Task()` provides debounced numeric entry, `#` confirmation, and a silent `*` shortcut that toggles `odpojeniMotoru`; enumerated unit choices are encoder-only. Digits typed while merely *hovering* `R`, `Poz`, or `Ink` write straight into that value — no confirm press needed to start editing (see [direct keypad entry](#direct-keypad-entry)).
- **RGB LED:** status indicator. **No signaling logic defined yet** — current `RGB_R/G/B` writes are placeholders/experiments, not a finalized scheme.
- **Inputs / switches** (see `Core/Inc/main.h` for pin mapping):
  - `Bconf` — **Back** button (EXTI). Returns to the main menu and resets `pozice`/`poziceAktualniKroky`. `rychlost` (`R`) deliberately survives — it is persisted, see [key globals](#key-globals-mainc).
  - `Benc` — encoder push-button = **Confirm / "A"** (EXTI). Enters menus, confirms edits, and starts motion; while any motor movement is active, its ISR instead raises `motor_stop_request` and immediately pulls `STEP` low. Auto, positional, and fast-jog loops all exit on that request.
  - `Left` / `Right` — two throws of an **ON-OFF-ON** switch selecting motor **direction** for Auto posuv, **all Inkrementalni moves**, and Absolutni `°Rel` moves. Absolutni in `mm`/`°Abs`/`ot` ignores it by necessity — an absolute move has to run whichever way the target lies. Note `Left` is **PB2 = BOOT1**, and `ReadDirectionSwitch()` tests it first, so a board-level BOOT1 pull-down would make "Prava" unreachable; both inputs are `GPIO_NOPULL` and rely on external pulls.
  - `Bleft_fast` / `Bright_fast` — two throws of an **(ON)-OFF-(ON)** momentary switch for **fast jog** (EXTI-configured, also polled in the main loop).
  - `Bleft_max` / `Bright_max` — **limit switches**, interrupt-driven (EXTI3/4, both edges; CubeMX has them rising-only without NVIC, so USER CODE 2 re-inits the pins and enables the IRQs, and the handlers live in `stm32f1xx_it.c` USER CODE 1). The ISR maintains `volatile limit_stop` from the pin levels and on press immediately drops `Enable`/`Reset` to de-energise the driver. Either switch stops **all** movement in **both** directions: Auto run ends, positional moves retain the actual completed STEP count in `poziceAktualniKroky`, and jog loops exit — all via `LimitHit()`.

## Build

STM32CubeIDE project. The `Debug/` directory holds generated build artifacts (`.o`, `.elf`, `.list`, `.map`) and is checked in — don't hand-edit it. Regenerate peripheral init code via `Posuv.ioc` in CubeMX/CubeIDE; HAL-managed code lives between `/* USER CODE BEGIN */ ... /* USER CODE END */` guards, so keep custom logic inside those guards or CubeMX will overwrite it.

## Code layout

- `Core/Src/main.c` — **all application logic**: state machine, menu/UI, motion, encoder, keypad, peripheral init.
- `Core/Inc/main.h` — pin name `#define`s (generated from the .ioc).
- `Core/Src/stm32f1xx_it.c` — interrupt handlers. `EXTI1` → `Benc`; `EXTI3`/`EXTI4` → `Bleft_max`/`Bright_max` (hand-written in USER CODE 1 — CubeMX doesn't know these IRQs are enabled); `EXTI9_5` → `Bleft_fast`/`Bright_fast`. All dispatch into `HAL_GPIO_EXTI_Callback()` in `main.c`.
- `Core/Src/ST7920_SERIAL.c` / `.h` — GLCD driver + graphics primitives.
- `Core/Inc/font.h` — 8×8 bitmap font.

## Application architecture

Everything runs from a single `while(1)` superloop in `main()`, branching on the global **`_inMenu`** state variable. Button presses are handled in `HAL_GPIO_EXTI_Callback()` (interrupt context); the encoder, jog, direction switch, limit handling, and motion stepping are polled in the loop.

### `_inMenu` state machine

| Value | State | Behavior |
|------:|-------|----------|
| `1` / `2` | **Main menu** | `Menu_Task()` — encoder scrolls the 4 items, `Benc` enters. `2` forces a redraw on entry. |
| `0` | **Inside a mode** | `Menu_Inside()` — encoder moves `inside_cursor` over the fields/buttons of the selected mode. |
| `-1` | **Editing a value** | Encoder/keypad edit the value chosen by `edit_target` (rychlost, pozice, or an HW-setup value); `Edit_Binding()` supplies the pointer + min/max/step. `Benc` clamps and returns to `0`. |
| `-2` | **Running motor** | Generates STEP pulses. For Auto posuv this is continuous; for Absolutni/Inkrementalni it runs a finite loop then drops back to `0`. Entered by `MotorStart()` — from the START row, or immediately on opening Auto posuv. |

`selected` (0–3) = which main-menu mode is active. `inside_cursor` = highlighted field within a mode (count per mode comes from `Inside_CursorCount()`).

`Menu_Inside_Init()` sets the **entry cursor per mode**, so the row you most likely want is already selected: Auto posuv → row 1 (START/STOP, matching its start-on-entry), Absolutni/Inkrementalni → row 1 (`Poz:`/`Ink:`, so the numpad can set the value immediately with no scrolling and no confirm press), HW setup → row 0.

### Menu modes (`selected`)

0. **Auto posuv** — continuous feed at `rychlost`, direction from the `Left/Right` switch (`smer_pohybu`). **Starts on entry:** `Menu_Inside_Init()` calls `MotorStart()` right away, so opening the item goes straight to `_inMenu==-2` with the cursor parked on the STOP row — no START press. Nothing actually steps until the direction switch leaves its center position, and a pressed limit switch drops it back to `_inMenu==0`. `Benc` (or Back) stops it; to change `R`, stop first, since the keypad isn't scanned while running. Opening the item also **zeroes `poziceAktualniKroky`**, so `Dr:` counts the distance fed during this run — note this redefines the shared zero point that Absolutni/Inkrementalni use.
1. **Absolutni** — move to an absolute `pozice` in the selected output unit, tracked internally in STEP pulses. With `°Rel`, the direction switch chooses the increasing/right or decreasing/left path around 0–359.99°; its center position prevents START.
2. **Inkrementalni** — move by a relative increment `pozice` in the selected output unit. **The direction switch chooses which way the increment runs, in every unit** (not just `°Rel`), so `Ink:` is a magnitude only: `Edit_Binding()` clamps it to `0..POZICE_MAX` for this mode and `Smer:` always shows the lever. A centered lever prevents START. This is deliberate — the numpad cannot enter negative values, so a sign-driven direction made "left" unreachable from the keypad.

   Both positional modes carry five cursor rows — `R`, `Poz`/`Ink`, **VYNULOVAT**, **NULOVY BOD**, START:
   - **VYNULOVAT** — sets `poziceAktualniKroky = 0`, i.e. "the zero point is here". This is the old NULOVY BOD behavior, moved to its own row.
   - **NULOVY BOD** — *drives* the axis back to the zero point by exactly `-poziceAktualniKroky` steps, through the normal `_inMenu==-2` branch (`navrat_do_nuly` flag) so it gets the usual accel ramp, `Benc` stop, and limit-switch abort. It **unwinds exactly**: in `°Rel` a position wound up over several turns is retraced in full rather than reduced modulo 360°. A no-op when already at zero.
3. **HW setup** — ten settings with seven visible rows; `inside_top` scrolls to keep `inside_cursor` visible. Settings are **persisted to flash** (`Settings_Load()`/`Settings_Save()`, wear-levelled slot array in the last 8 KB `0x0801E000`, checksum only; saved on each confirmed change — see [flash storage](#flash-storage)). The same record also carries `rychlost` (`R`), which is not an HW-setup row — it is written whenever a confirmed or direct-entry `R` edit changes it:
   - **Stoupani** — general mechanical ratio, stored as `stoupaniSetiny` in hundredths of the selected output unit per motor revolution. Labels are `mm/otM`, `°R/otM`, `°A/otM`, or `otV/otM`. Position inputs are also hundredths, so `StepsForDistance(valueHundredths)` effectively computes `valueHundredths × MOTOR_STEPS_PER_REV × MICROSTEP / stoupaniSetiny` (rounded, 64-bit).
   - **Vzdalenost[...]** — `jednotkaDelky`: `mm`, `°Rel`, `°Abs`, or output revolutions `ot`. Width-constrained `R`, `Max`, `S`, and selector rows use compact `°R`/`°A`; `Poz` and `Ink` show full `°Rel`/`°Abs`. `°Rel` wraps in the range 0.00–359.99°; `°Abs` retains signed/cumulative behavior.
   - **Cas[...]** — `jednotkaCasu`: `s` or `min`. It changes and converts only the live `R` feed-rate denominator. Stoupani remains per motor revolution, `Max` remains per second, and `Acc` remains per second squared.
   - **Max rychlost** — user cap stored in hundredths of the selected output unit **per second** in `maxSpeed`, independent of `Cas`. `MaxRychlostPerSecond()` enforces the physical `STEP_RATE_MAX` limit, and `RychlostEditMax()` converts that cap to the active `s`/`min` denominator for `R`.
   - **Akcelerace** — `akcelerace` in selected-output-unit/s². **Applied**: accel/decel ramp for positional moves and ramp-up for Auto feed.
   - **RyM** — `rychlostManualSetiny`, fast-jog target speed in hundredths of the selected output unit per second (`128` = `1.28`), matching `Max`. It is physically capped by `STEP_RATE_MAX` using the jog's fixed 1/4-step mode.
   - **RyAcc** — `rychlostManualAcc`, fast-jog acceleration in selected-output-unit/s².
   - **RyDec** — `rychlostManualDec`, fast-jog deceleration after normal switch release in selected-output-unit/s². Encoder STOP and limit switches remain immediate.
   - **Orientace** — `orientace` toggle (`1` = "+ doprava", `0` = "+ doleva"); inverts every physical DIR write via `DirApply()`.
   - **Odpojeni motoru** — `odpojeniMotoru` toggle (ANO/NE); ANO de-energises the motor when idle, NE keeps holding torque.

### Key globals (`main.c`)

- `rychlost` — feed rate `R` stored in hundredths of the selected output/time unit (`100` = `1.00`), clamped to the per-second `maxSpeed` after conversion to the active time denominator and to the step-rate limit. **Persisted** in the same flash record as the HW setup (`SettingsFlash.rychlostSetiny`), so it survives both Back-to-menu and power-down; the units it is expressed in live in that record too, so they always reload consistently. Saved from `Confirm_Action()` (`EDIT_RYCHLOST`) and from `DirectEntry_Finish()`; `Settings_Save()` no-ops when the record is unchanged, so a write happens only on an *actual* value change. `Settings_Load()` re-clamps it via `ClampSpeedValues()`, so a corrupt or out-of-range stored value cannot produce an unsafe rate.
- `pozice` — target/increment stored in hundredths of the selected output unit; `poziceAktualniKroky` retains the signed physical position in STEP pulses across unit changes. It is now fed by **every** kind of motion — positional moves, Auto feed (one count per pulse), and fast jog — and is the source of the `Dr:` readout. Zeroed in exactly two places: **VYNULOVAT**, and **opening Auto posuv** (`Menu_Inside_Init()`, so `Dr:` measures that feed run). Back-to-menu deliberately leaves it alone, or stepping out to the menu would silently discard the zero point. Not persisted to flash — a physical position is meaningless after power-down, since the axis can be moved by hand.
- `smer_pohybu` — direction: `1` = Prava (right), `0` = Leva (left), `-1` = Stop.
- `maxSpeed` — the `Max` cap stored in hundredths of the selected output unit per second; changing `Cas` does not change it.
- HW-setup values: `stoupaniSetiny`, `jednotkaDelky`, `jednotkaCasu`, `akcelerace`, `rychlostManualSetiny`, `rychlostManualAcc`, `rychlostManualDec`, `orientace`, and `odpojeniMotoru`. `edit_target` selects what `_inMenu==-1` edits.

### `Dr:` counter

<a id="dr-counter"></a>The bottom line of Auto posuv, Absolutni and Inkrementalni shows **`Dr:`** — the signed distance from the zero point, i.e. `poziceAktualniKroky` rendered in the selected length unit by `DistanceFromSteps()` (the exact inverse of `StepsForDistance()`; round-trip error is bounded by half a microstep). Negative to the left. HW setup has no counter — `S:` there stays Stoupani.

The zero point it counts from is **per-mode by intent**: opening Auto posuv resets it (free feed — the useful reading is "how far have I fed this run"), while Absolutni and Inkrementalni keep theirs across menu visits so VYNULOVAT/NULOVY BOD stay meaningful. Since the underlying `poziceAktualniKroky` is one shared global, a trip through Auto posuv does move the zero that the positional modes return to.

Two things it depends on:

- **All motion counts.** Auto feed adds ±1 per pulse; positional moves add the value `MotorMove()` actually completed; jog adds its pulses scaled by `JOG_STEP_WEIGHT` (`MICROSTEP / JOG_MICROSTEP` = 2) because jog runs at a **coarser microstep** than everything else, so one jog pulse is two position pulses. A `_Static_assert` pins that the division is exact. Miss the scaling and jog travel is counted at half its true distance.
- **It repaints only when motion stops** (`Inside_Draw()` after a move finishes, after Auto stop, after limit abort, after jog release) — never on a timer during motion, because one display push costs tens of ms and would tear holes in the step train. The number holds still during a long move, then jumps to the true value. Note a redraw *does* already happen mid-Auto if the direction switch is flipped; that is pre-existing and only fires on an actual switch change.

### Flash storage

Because `R` is persisted on every change (an operator may retune it 150× a day), rewriting one fixed location would burn through the STM32F103's 10 k guaranteed page erases in about **9 weeks**. The store is therefore a **wear-levelled slot array**, not a single record:

- **Region:** last 8 KB, `SETTINGS_FLASH_BASE = 0x0801E000` … `0x0801FFFF` (`SETTINGS_PAGES = 8` × 1 KB). Three `_Static_assert`s pin the layout: word-sized record, at least one slot per page, and the region ending exactly at the top of flash.
- **Slots:** the 48-byte `SettingsFlash` record tiles each page 21× (`SETTINGS_SLOTS_PER_PAGE`, 16 B/page unused), 168 slots total. `Settings_Slot()` indexes per page so **a slot never straddles a page boundary**.
- **Write:** `Settings_Save()` writes the lowest **fully erased** slot; the region is erased (all 8 pages at once) only when every slot is used. That is one erase per 168 writes → **~1.68 M writes**, ≈ 30 years at 150/day. The all-at-once erase is what keeps the "highest slot = newest" invariant true.
- **Read:** `Settings_Latest()` scans downward for the highest slot whose checksum validates. Writes always move upward, so slot order *is* write order — no sequence counter needed.
- **Power-loss safety:** the checksum is the record's last word and is programmed last, so an interrupted write leaves a slot that fails validation. Load skips it and returns the previous record; save skips it too (it is not erased) and moves to the next slot. An **erased** slot can never validate — the sum of 11 `0xFFFFFFFF` words is `0xFFFFFFF5`, not `0xFFFFFFFF`.

Erasing 8 pages stalls the CPU for roughly 160–320 ms (flash reads block, so ISRs stall too). That is safe here only because **no save can occur during motion**: the keypad is not scanned in `_inMenu==-2`, `DirectEditTarget()` requires `_inMenu==0`, and `Confirm_Action()` finalizes direct entry *before* `MotorStart()`. Preserve that property if you add new `Settings_Save()` call sites.

### Motion / step timing

Steps are bit-banged: set `STEP` high, `delay_us_motor(speed)`, low, `delay_us_motor(speed)`. `delay_us_motor()` busy-waits on **TIM4**; its unit is a **1 µs tick** (TIM4 kernel clock 64 MHz — APB1 32 MHz ×2 — ÷ prescaler 64 = 1 MHz, `MOTOR_TICKS_PER_S`).

`MOTOR_TICKS_PER_S` is **derived from `HSE_VALUE`** (`MOTOR_SYSCLK_HZ` → `MOTOR_TIM4_HZ` → ÷ `MOTOR_TIM4_PSC`) rather than hard-coded, because it was previously the literal `500000` — a figure taken from an 8 MHz crystal the board does not have. With a 16 MHz crystal every `delay_us_motor()` wait was half as long as intended, so **every feed ran exactly 2× the displayed rate**, and `STEP_HALF_MIN_TICKS` capped the step rate at ~15.9 kHz instead of the intended 8 kHz. Distances stayed correct throughout, because they depend only on the step *count*. If the crystal or PLL/prescaler config ever changes, update these macros — a `_Static_assert` checks only that the resulting minimum half-period still fits `delay_us_motor()`'s `uint16_t`.

The half-period is **calibrated** from the selected output/time units and `stoupaniSetiny`: `LengthUnitRatio()` gives motor revolutions per output unit, while `StepHalfTicks()` compensates for the hundredth-based speed storage and includes 1 or 60 seconds for the selected `R` denominator. The step rate is capped at `STEP_RATE_MAX` (8 kHz; tune on hardware). `MaxRychlostPerSecond()` exposes the physical cap per second, while `RychlostEditMax()` converts it for the active `R` denominator. `JogTargetStepRate()` converts hundredth-based `RyM` to the 1/4-step jog rate and applies the same physical step-rate cap.

**Acceleration ramp**: positional moves go through `MotorMove(kroky)` — a trapezoidal profile at `akcelerace` selected-output-unit/s². `RampA2()` converts it through the same Stoupani ratio; deceleration mirrors acceleration, and short moves use a triangular profile. Auto feed ramps up across superloop iterations but still stops abruptly. `JogMove()` tracks squared step rate: it adds `RyAcc` while the fast switch is held and subtracts `RyDec` after release until stopped.

### Display coordinates

`GLCD_Font_Print(x, y, str)` uses an 8×8 font: **`x` = character column 0–15**, **`y` = text row 0–7** (128/8 cols, 64/8 rows). `PrintLineSel(y, text, active)` prints a row with a leading `>` when `active`. Drawing pattern is always: `ST7920_Clear()` → draw into framebuffer → `ST7920_Update()`.

`GLCD_Font_Print` **wraps to the next row past column 15 and never bounds-checks `y`**, so a 17-char string on row 7 writes past the end of the 1024-byte `GLCD_Buf`. `PrintLineSel` therefore truncates hard at 16 columns (`char buf[17]`) — this matters because the `Dr:` row lives on row 7 and its width grows with the value.

Row layout of the three motion screens (row 0 stays free for debug, `Dr:` is always the bottom line):

| Row | Auto posuv | Absolutni / Inkrementalni |
|---:|---|---|
| 1 | `R:` (cursor 0) | `R:` (cursor 0) |
| 2 | `Smer:` | `Smer:` |
| 3 | — | `Poz:`/`Ink:` (cursor 1) |
| 4 | — | `VYNULOVAT` (cursor 2) |
| 5 | — | `NULOVY BOD` (cursor 3) |
| 6 | START/STOP (cursor 1) | START/CEKEJ (cursor 4) |
| 7 | `Dr:` | `Dr:` |

A full `ST7920_Update()` bit-bangs all 512 blocks and costs **tens of ms**, which is why `Dr:` is never refreshed on a timer during motion — see [`Dr:` counter](#dr-counter).

## Conventions

- UI strings and many identifiers are in **Czech** (e.g. `rychlost` = speed, `pozice` = position, `smer` = direction, `posuv` = feed, `stoupani` = pitch). Keep new UI text consistent with that.
- Peripheral init is CubeMX-generated; add app code only inside `USER CODE` guards.
- No RTOS, no DMA for the display — the superloop blocks during motion and during keypad debounce (`HAL_Delay`).

## Known TODOs / gotchas

- **HW setup is fully wired.** Stoupani, selectable length/time units, Max rychlost, Akcelerace, RyM, RyAcc, RyDec, Orientace, and Odpojeni motoru all take effect. `STEP_RATE_MAX` (8 kHz) is an estimate — verify the motor doesn't stall at top speed under load and tune it. Note it was **not actually enforced** until the `MOTOR_TICKS_PER_S` fix: the wrong tick unit let the real ceiling reach ~15.9 kHz, so any earlier stall observations at "top speed" were taken at roughly double the intended rate and are worth re-checking.
- **Auto feed has no deceleration** — the ramp only accelerates; Stop/Back and direction-switch-to-center cut steps instantly (as the firmware always did). Fine for a feed drive, but worth knowing.
- **Real speed/distance accuracy** relies on `stoupaniSetiny`, `MOTOR_STEPS_PER_REV` (200), `MICROSTEP` (8), and `JOG_MICROSTEP` (4) matching the mechanics and M1/M2/M3 levels. All ten settings plus `rychlost` are stored directly with a checksum and no migration/version layer. **Any change to `SettingsFlash` invalidates every stored record** — checksums fail and defaults are used once after flashing. The final 8 KB of flash must remain reserved as the firmware grows (image is ~32 KB of 128 KB, so there is ~88 KB of headroom; the linker does not know about the region, so nothing but distance protects it).
- **Microstepping (`M1`/`M2`/`M3`) is hardcoded**, should become adjustable in the HW-setup menu.
- **Limit stop blocks both directions** — a pressed limit switch stops and prevents *every* movement (per design), including moving *away* from the switch; back off the limit with the manual handwheel. Aborted positional moves stop without a deceleration ramp (intentional — it's an end stop). This also applies to **NULOVY BOD**: a limit hit part-way back leaves the axis short of zero, with `Dr:` showing how far it actually got — press it again after backing off.
- **`Dr:` drifts if the axis is moved by hand.** The counter is derived from commanded STEP pulses, not from an encoder on the axis, so turning the handwheel (or a stall/lost steps under load) makes it read wrong. VYNULOVAT is the re-sync.
- **Keypad** — digits enter raw hundredths for `S`, `R`, `Max`, `RyM`, `Poz`, and `Ink` (`128` → `1.28`), and integers for `RyAcc`/`RyDec`; `#` confirms, and `*` silently toggles Odpojeni motoru between ANO/NE. In `°Rel`, keypad distance wraps modulo 360.00 just like the encoder; length/time enumerations are encoder-only. **The numpad cannot enter a negative value** — only the encoder can go below zero, and only where the range allows it (`Poz` in Absolutni). This is why Inkrementalni takes its direction from the lever instead of the sign of `Ink`.

<a id="direct-keypad-entry"></a>
- **Direct keypad entry (no confirm press)** — in modes 0/1/2, typing a digit while the cursor merely *hovers* `R` (cursor 0) or `Poz`/`Ink` (cursor 1) writes into that value without entering `_inMenu==-1`. `DirectEditTarget()` maps the hovered row to an `edit_target`; `direct_target` remembers which field is being typed, and the first digit on a newly hovered field replaces its value. The encoder therefore keeps moving the cursor. `DirectEntry_Finish()` applies the same clamping/`ClampSpeedValues()` as a confirmed edit and runs on cursor move, on `#`/`Benc`, and on Back. HW setup is deliberately excluded — those values persist to flash and still need a confirm press. `Edit_Binding(target, …)` takes the target explicitly so both paths share it.
- **RGB LED has no defined meaning yet** — current color writes are experimental.
- **ADC1/ADC2 are dead code.** Initialized (`MX_ADC1_Init`/`MX_ADC2_Init`, `hadc1`/`hadc2`, channels 0/1) but never sampled, and **not even connected on the PCB** — safe to remove.
- Long positional motion loops still block the normal UI, but the `Benc` EXTI stop and limit-switch interrupts remain active and terminate the movement.
