# CLAUDE.md

Guidance for working in this firmware. It controls a **universal electric machine feed** ("strojní posuv") — a powered axis-feed unit that drives a stepper motor for milling machines, lathes, and similar tools. The operator picks a feed mode and rate on a small GLCD menu, and the unit steps the motor accordingly.

**Status: work in progress.** Several screens are drawn but not wired to behavior, and the mm/speed units are not yet calibrated to real machines. See [Known TODOs / gotchas](#known-todos--gotchas).

## Hardware

- **MCU:** STM32F103RBTx (Cortex-M3, 128 KB flash, 20 KB RAM), STM32CubeMX/HAL project.
- **Clock:** 8 MHz HSE × PLL4 = **32 MHz** SYSCLK. APB1 = 16 MHz, APB2 = 8 MHz. ADC clock = PCLK2/2.
- **Motor driver:** TB6600HG stepper driver. Controlled by bit-banged GPIO: `STEP`, `DIR`, `Enable`, `Reset`, plus microstep-select lines `M1`/`M2`/`M3` and `LTC`. Microstepping is meant to be **adjustable on the device** (not yet implemented — see TODOs).
- **Display:** 128×64 ST7920 graphic LCD over **software SPI** (driver in `Core/Src/ST7920_SERIAL.c`). Runs in graphic mode; the firmware draws into a RAM framebuffer and pushes it with `ST7920_Update()`.
- **Encoder:** rotary encoder on **TIM2 in encoder mode**. Raw `TIM2->CNT` is shifted `>> 2` (4 counts/detent) via `Encoder_GetSteps()`.
- **Keypad:** 4×4 matrix numeric keypad, physically wired. Scanned by `Keypad_GetKey()`. **Not yet used** (entry of values by keypad is planned).
- **RGB LED:** status indicator. **No signaling logic defined yet** — current `RGB_R/G/B` writes are placeholders/experiments, not a finalized scheme.
- **Inputs / switches** (see `Core/Inc/main.h` for pin mapping):
  - `Bconf` — **Back** button (EXTI). Returns to the main menu, resets `rychlost`/`pozice`.
  - `Benc` — encoder push-button = **Confirm / "A"** (EXTI). Enters menus, confirms edits, starts motion.
  - `Left` / `Right` — two throws of an **ON-OFF-ON** switch selecting motor **direction**.
  - `Bleft_fast` / `Bright_fast` — two throws of an **(ON)-OFF-(ON)** momentary switch for **fast jog** (EXTI-configured, also polled in the main loop).
  - `Bleft_max` / `Bright_max` — **limit switches** (configured; not yet acted upon in code).

## Build

STM32CubeIDE project. The `Debug/` directory holds generated build artifacts (`.o`, `.elf`, `.list`, `.map`) and is checked in — don't hand-edit it. Regenerate peripheral init code via `Posuv.ioc` in CubeMX/CubeIDE; HAL-managed code lives between `/* USER CODE BEGIN */ ... /* USER CODE END */` guards, so keep custom logic inside those guards or CubeMX will overwrite it.

## Code layout

- `Core/Src/main.c` — **all application logic**: state machine, menu/UI, motion, encoder, keypad, peripheral init.
- `Core/Inc/main.h` — pin name `#define`s (generated from the .ioc).
- `Core/Src/stm32f1xx_it.c` — interrupt handlers. `EXTI1` → `Benc`; `EXTI9_5` → `Bleft_fast`/`Bright_fast`. All dispatch into `HAL_GPIO_EXTI_Callback()` in `main.c`.
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
1. **Absolutni** — move to an absolute `pozice` (mm), tracked against `poziceAktualni`.
2. **Inkrementalni** — move by a relative increment `pozice` (mm).
3. **HW setup** — five settings, **persisted to flash** (`Settings_Load()`/`Settings_Save()`, last 1 KB page `0x0801FC00`, magic + checksum; saved on each confirmed change):
   - **Stoupani** — leadscrew pitch, stored as `stoupaniUm` (thousandths of mm, 0.001 mm step). Editable; **not yet wired into steps/mm** (calibration TODO).
   - **Max rychlost** — user cap on `rychlost`, stored in `maxSpeed`, clamped to `MAXSPEED_HARD_CAP` (70). Wired: `rychlost` can't exceed it.
   - **Akcelerace** — `akcelerace` (mm/s²). Editable/stored; **ramp not yet applied** to motion (TODO, needs the calibrated speed model).
   - **Orientace** — `orientace` toggle (`1` = "+ doprava", `0` = "+ doleva"); inverts every physical DIR write via `DirApply()`.
   - **Odpojeni motoru** — `odpojeniMotoru` toggle (ANO/NE); ANO de-energises the motor when idle, NE keeps holding torque.

### Key globals (`main.c`)

- `rychlost` — feed rate (labeled mm/s; clamped `1..maxSpeed`).
- `pozice` / `poziceAktualni` — target / current position (labeled mm).
- `smer_pohybu` — direction: `1` = Prava (right), `0` = Leva (left), `-1` = Stop.
- `maxSpeed` (=70), `speed` — see motion timing below. `maxSpeed` doubles as the "Max rychlost" cap.
- HW-setup values: `stoupaniUm` (pitch, µm), `akcelerace` (mm/s²), `orientace` (0/1), `odpojeniMotoru` (0/1). `edit_target` selects what `_inMenu==-1` edits.

### Motion / step timing

Steps are bit-banged: set `STEP` high, `delay_us_motor(speed)`, low, `delay_us_motor(speed)`. `delay_us_motor()` busy-waits on **TIM4** (1 µs tick, prescaler 64-1 at 32 MHz → ~0.5 MHz; treat the unit as "timer ticks", empirically tuned).

The per-pulse delay is `speed = SPEED_BASE - rychlost*SPEED_SLOPE` (`SPEED_BASE`=4200, `SPEED_SLOPE`=56), i.e. **higher `rychlost` → shorter delay → faster**. The jog uses a fixed `JOG_SPEED` (490). These are **empirically tuned magic numbers** (formerly `maxSpeed*60`/`*56`/`maxSpeed*7` when `maxSpeed` was hardcoded 70), now decoupled from the user-adjustable `maxSpeed` so changing "Max rychlost" doesn't invert the real speed. (Intent: replace this inverse mapping with a normal, calibrated speed model.)

### Display coordinates

`GLCD_Font_Print(x, y, str)` uses an 8×8 font: **`x` = character column 0–15**, **`y` = text row 0–7** (128/8 cols, 64/8 rows). `PrintLineSel(y, text, active)` prints a row with a leading `>` when `active`. Drawing pattern is always: `ST7920_Clear()` → draw into framebuffer → `ST7920_Update()`.

## Conventions

- UI strings and many identifiers are in **Czech** (e.g. `rychlost` = speed, `pozice` = position, `smer` = direction, `posuv` = feed, `stoupani` = pitch). Keep new UI text consistent with that.
- Peripheral init is CubeMX-generated; add app code only inside `USER CODE` guards.
- No RTOS, no DMA for the display — the superloop blocks during motion and during keypad debounce (`HAL_Delay`).

## Known TODOs / gotchas

- **mm units are not calibrated.** `rychlost`/`pozice` are labeled mm·s⁻¹/mm but motion uses raw step counts and tuned constants. Per-machine calibration (steps/mm, leadscrew pitch) is a planned device-side setup feature.
- **HW setup is partially wired.** Max rychlost, Orientace, Odpojeni motoru take effect immediately. Stoupani and Akcelerace are editable and stored but **not yet applied** to motion (need the calibrated speed/steps-per-mm model). All five are **persisted to flash** (last 1 KB page, `0x0801FC00`); this page is assumed free (program is ~22 KB) — if the firmware ever grows past ~127 KB, reserve it in the linker script.
- **Microstepping (`M1`/`M2`/`M3`) is hardcoded**, should become adjustable in the HW-setup menu.
- **Limit switches `Bleft_max`/`Bright_max` are not handled** — wired and GPIO-configured but no end-stop logic.
- **Keypad** — `Keypad_Task()` feeds digits/`*`/`#` into value editing (`_inMenu==-1`) for whatever `edit_target` points at, incl. HW-setup values. Note: for Stoupani the keypad enters raw thousandths of a mm (type `2000` → 2.000 mm), since there is no decimal-point key.
- **RGB LED has no defined meaning yet** — current color writes are experimental.
- **ADC1/ADC2 are dead code.** Initialized (`MX_ADC1_Init`/`MX_ADC2_Init`, `hadc1`/`hadc2`, channels 0/1) but never sampled, and **not even connected on the PCB** — safe to remove.
- Long blocking motion loops (e.g. `abs(pozice)*rychlost` iterations) make the UI unresponsive while running; only the EXTI buttons interrupt.
