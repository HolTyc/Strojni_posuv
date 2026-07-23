# CLAUDE.md

Guidance for working in this firmware. It controls a **universal electric machine feed** ("strojní posuv") — a powered axis-feed unit that drives a stepper motor for milling machines, lathes, and similar tools. The operator picks a feed mode and rate on a small GLCD menu, and the unit steps the motor accordingly.

**Status: work in progress.** The motion screens and HW setup are wired, including selectable output/time units, but the mechanical values still need verification on real machines. See [Known TODOs / gotchas](#known-todos--gotchas).

## Hardware

- **MCU:** STM32F103RBTx (Cortex-M3, 128 KB flash, 20 KB RAM), STM32CubeMX/HAL project.
- **Clock:** 8 MHz HSE × PLL4 = **32 MHz** SYSCLK. APB1 = 16 MHz, APB2 = 8 MHz. ADC clock = PCLK2/2.
- **Motor driver:** TB6600HG stepper driver. Controlled by bit-banged GPIO: `STEP`, `DIR`, `Enable`, `Reset`, plus microstep-select lines `M1`/`M2`/`M3` and `LTC`. Microstepping is meant to be **adjustable on the device** (not yet implemented — see TODOs).
- **Display:** 128×64 ST7920 graphic LCD over **software SPI** (driver in `Core/Src/ST7920_SERIAL.c`). Runs in graphic mode; the firmware draws into a RAM framebuffer and pushes it with `ST7920_Update()`.
- **Encoder:** rotary encoder on **TIM2 in encoder mode**. Raw `TIM2->CNT` is shifted `>> 2` (4 counts/detent) and inverted centrally by `Encoder_GetSteps()`.
- **Keypad:** 4×4 matrix numeric keypad, physically wired. `Keypad_Task()` provides debounced numeric entry, `#` confirmation, and a silent `*` shortcut that toggles `odpojeniMotoru`; enumerated unit choices are encoder-only.
- **RGB LED:** status indicator. **No signaling logic defined yet** — current `RGB_R/G/B` writes are placeholders/experiments, not a finalized scheme.
- **Inputs / switches** (see `Core/Inc/main.h` for pin mapping):
  - `Bconf` — **Back** button (EXTI). Returns to the main menu, resets `rychlost`/`pozice`.
  - `Benc` — encoder push-button = **Confirm / "A"** (EXTI). Enters menus, confirms edits, and starts motion; while any motor movement is active, its ISR instead raises `motor_stop_request` and immediately pulls `STEP` low. Auto, positional, and fast-jog loops all exit on that request.
  - `Left` / `Right` — two throws of an **ON-OFF-ON** switch selecting motor **direction** for Auto and finite `°Rel` moves.
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
| `-2` | **Running motor** | Generates STEP pulses. For Auto posuv this is continuous; for Absolutni/Inkrementalni it runs a finite loop then drops back to `0`. |

`selected` (0–3) = which main-menu mode is active. `inside_cursor` = highlighted field within a mode (count per mode comes from `Inside_CursorCount()`).

### Menu modes (`selected`)

0. **Auto posuv** — continuous feed at `rychlost`, direction from the `Left/Right` switch (`smer_pohybu`).
1. **Absolutni** — move to an absolute `pozice` in the selected output unit, tracked internally in STEP pulses. With `°Rel`, the direction switch chooses the increasing/right or decreasing/left path around 0–359.99°; its center position prevents START.
2. **Inkrementalni** — move by a relative increment `pozice` in the selected output unit. With `°Rel`, the direction switch applies the increment to the right or left; its center position prevents START.
3. **HW setup** — ten settings with seven visible rows; `inside_top` scrolls to keep `inside_cursor` visible. Settings are **persisted to flash** (`Settings_Load()`/`Settings_Save()`, last 1 KB page `0x0801FC00`, checksum only; saved on each confirmed change):
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

- `rychlost` — feed rate `R` stored in hundredths of the selected output/time unit (`100` = `1.00`), clamped to the per-second `maxSpeed` after conversion to the active time denominator and to the step-rate limit.
- `pozice` — target/increment stored in hundredths of the selected output unit; `poziceAktualniKroky` retains the signed physical position in STEP pulses across unit changes.
- `smer_pohybu` — direction: `1` = Prava (right), `0` = Leva (left), `-1` = Stop.
- `maxSpeed` — the `Max` cap stored in hundredths of the selected output unit per second; changing `Cas` does not change it.
- HW-setup values: `stoupaniSetiny`, `jednotkaDelky`, `jednotkaCasu`, `akcelerace`, `rychlostManualSetiny`, `rychlostManualAcc`, `rychlostManualDec`, `orientace`, and `odpojeniMotoru`. `edit_target` selects what `_inMenu==-1` edits.

### Motion / step timing

Steps are bit-banged: set `STEP` high, `delay_us_motor(speed)`, low, `delay_us_motor(speed)`. `delay_us_motor()` busy-waits on **TIM4**; despite the name its unit is a **2 µs tick** (TIM4 kernel clock 32 MHz — APB1 16 MHz ×2 — / prescaler 64 = 0.5 MHz, `MOTOR_TICKS_PER_S`).

The half-period is **calibrated** from the selected output/time units and `stoupaniSetiny`: `LengthUnitRatio()` gives motor revolutions per output unit, while `StepHalfTicks()` compensates for the hundredth-based speed storage and includes 1 or 60 seconds for the selected `R` denominator. The step rate is capped at `STEP_RATE_MAX` (8 kHz; tune on hardware). `MaxRychlostPerSecond()` exposes the physical cap per second, while `RychlostEditMax()` converts it for the active `R` denominator. `JogTargetStepRate()` converts hundredth-based `RyM` to the 1/4-step jog rate and applies the same physical step-rate cap.

**Acceleration ramp**: positional moves go through `MotorMove(kroky)` — a trapezoidal profile at `akcelerace` selected-output-unit/s². `RampA2()` converts it through the same Stoupani ratio; deceleration mirrors acceleration, and short moves use a triangular profile. Auto feed ramps up across superloop iterations but still stops abruptly. `JogMove()` tracks squared step rate: it adds `RyAcc` while the fast switch is held and subtracts `RyDec` after release until stopped.

### Display coordinates

`GLCD_Font_Print(x, y, str)` uses an 8×8 font: **`x` = character column 0–15**, **`y` = text row 0–7** (128/8 cols, 64/8 rows). `PrintLineSel(y, text, active)` prints a row with a leading `>` when `active`. Drawing pattern is always: `ST7920_Clear()` → draw into framebuffer → `ST7920_Update()`.

## Conventions

- UI strings and many identifiers are in **Czech** (e.g. `rychlost` = speed, `pozice` = position, `smer` = direction, `posuv` = feed, `stoupani` = pitch). Keep new UI text consistent with that.
- Peripheral init is CubeMX-generated; add app code only inside `USER CODE` guards.
- No RTOS, no DMA for the display — the superloop blocks during motion and during keypad debounce (`HAL_Delay`).

## Known TODOs / gotchas

- **HW setup is fully wired.** Stoupani, selectable length/time units, Max rychlost, Akcelerace, RyM, RyAcc, RyDec, Orientace, and Odpojeni motoru all take effect. `STEP_RATE_MAX` (8 kHz) is an estimate — verify the motor doesn't stall at top speed under load and tune it.
- **Auto feed has no deceleration** — the ramp only accelerates; Stop/Back and direction-switch-to-center cut steps instantly (as the firmware always did). Fine for a feed drive, but worth knowing.
- **Real speed/distance accuracy** relies on `stoupaniSetiny`, `MOTOR_STEPS_PER_REV` (200), `MICROSTEP` (8), and `JOG_MICROSTEP` (4) matching the mechanics and M1/M2/M3 levels. All ten settings are stored directly with a checksum and no migration/version layer. Because this change extends that raw record, the previous record will fail its checksum and defaults will be used once after flashing. The final 1 KB flash page must remain reserved as the firmware grows.
- **Microstepping (`M1`/`M2`/`M3`) is hardcoded**, should become adjustable in the HW-setup menu.
- **Limit stop blocks both directions** — a pressed limit switch stops and prevents *every* movement (per design), including moving *away* from the switch; back off the limit with the manual handwheel. Aborted positional moves stop without a deceleration ramp (intentional — it's an end stop).
- **Keypad** — digits enter raw hundredths for `S`, `R`, `Max`, `RyM`, `Poz`, and `Ink` (`128` → `1.28`), and integers for `RyAcc`/`RyDec`; `#` confirms, and `*` silently toggles Odpojeni motoru between ANO/NE. In `°Rel`, keypad distance wraps modulo 360.00 just like the encoder; length/time enumerations are encoder-only.
- **RGB LED has no defined meaning yet** — current color writes are experimental.
- **ADC1/ADC2 are dead code.** Initialized (`MX_ADC1_Init`/`MX_ADC2_Init`, `hadc1`/`hadc2`, channels 0/1) but never sampled, and **not even connected on the PCB** — safe to remove.
- Long positional motion loops still block the normal UI, but the `Benc` EXTI stop and limit-switch interrupts remain active and terminate the movement.
