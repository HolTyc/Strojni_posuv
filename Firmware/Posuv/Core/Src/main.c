/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ST7920_SERIAL.h"
#include <stdio.h>
#include <stdlib.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim4;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM4_Init(void);
/* USER CODE BEGIN PFP */
void delay_us_motor (uint16_t us);
static void Menu_KeepVisible(void);
static void Menu_Draw(void);
static int Inside_CursorCount(int sel);
static void Inside_Draw(void);
void Menu_Init(void);
void Menu_Inside_Init(void);
void Menu_Task(void);
void Menu_Inside(void);
static char Keypad_ScanRaw(void);   // neblokujici sken: vrati stisknutou klavesu nebo 0
static void Confirm_Action(void);   // sdilena akce potvrzeni: enkoder tlacitko (Benc) i '#'
static void Keypad_Task(void);      // cislice = editace, # = potvrdit, * = Odpojeni motoru
static void Settings_Save(void);    // HW setup + R do flash (viz SettingsFlash)
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static const char *menu[] = {
    "Auto posuv",
    "Absolutni",
    "Inkrementalni",
    "HW setup"
};
#define MENU_COUNT   ((int)(sizeof(menu)/sizeof(menu[0])))

// How many lines you want visible at once
#define MENU_VISIBLE_LINES 4

// Display positions (adjust to your font/layout)
#define MENU_X 0
#define MENU_Y0 0   // first line Y
#define MENU_LINE_SPACING 1  // if y is line-based, keep 1; if pixel-based, change

// After >>2, range is 0..16383 (16384 steps total)
#define ENC_STEPS_RANGE  (65536 / 4)     // 16384
#define ENC_HALF_RANGE   (ENC_STEPS_RANGE / 2) // 8192

// Jak dlouho musi byt stav klavesnice stabilni, nez se ohlasi (ms).
#define KEYPAD_DEBOUNCE_MS 20

// ------------ STATE ------------
static int16_t enc_last = 0;
static int selected = 0;   // highlighted item
static int top = 0;        // first visible item (for scrolling)

// ----- MODES -----
int8_t _inMenu = 1;

// ---- Inside screen state ----
static int inside_cursor = 0;       // which line/field is highlighted
static int inside_top = 0;          // first visible HW-setup item
static int16_t enc1_last = 0;

//POMOCNY RYCHLO FUNKCE
static inline int16_t Encoder_GetSteps(void)
{
    // Fyzicky smer enkoderu je opacny proti smeru menu. Inverze je tady,
    // aby platila jednotne pro hlavni menu, vnitrni menu i editaci hodnot.
    return (int16_t)(-(int32_t)(TIM2->CNT >> 2));
}
static void PrintLineSel(uint8_t y, const char *text, uint8_t active)
{
    // Displej ma 16 sloupcu a GLCD_Font_Print delsi retezec zalomi na dalsi
    // radek - na y=7 by zapisoval az za konec GLCD_Buf. Proto se tvrde orizne.
    char buf[17];
    if (active) snprintf(buf, sizeof(buf), ">%s", text);
    else        snprintf(buf, sizeof(buf), " %s", text);
    GLCD_Font_Print(0, y, buf);
}

//NUMPAD
static const char keymap[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

static GPIO_TypeDef* rowPorts[4] = {Num_R1_GPIO_Port, Num_R2_GPIO_Port, Num_R3_GPIO_Port, Num_R4_GPIO_Port};
static uint16_t      rowPins [4] = {Num_R1_Pin,  Num_R2_Pin,  Num_R3_Pin,  Num_R4_Pin};

static GPIO_TypeDef* colPorts[4] = {Num_C1_GPIO_Port, Num_C2_GPIO_Port, Num_C3_GPIO_Port, Num_C4_GPIO_Port};
static uint16_t      colPins [4] = {Num_C1_Pin,  Num_C2_Pin,  Num_C3_Pin,  Num_C4_Pin};

// Rychlost a pozice jsou ulozeny v setinach,
// tedy 100 = 1.00 zvolene jednotky.
int rychlost = 100;
int rychlost_last = 0;
int pozice = 0;
int pozice_last = 0;
int64_t poziceAktualniKroky = 0; // fyzicka poloha v STEP pulzech, nezavisla na zvolene jednotce
int smer_pohybu = 1;   // 1 = Pravo, 0 = Levo, -1 = stop

uint32_t adcValue1;
uint32_t adcValue2;
int maxSpeed = 7000;  // 70.00: uzivatelsky strop v setinach zvolene jednotky/s
// Kalibrace vystupu: zvolena jednotka na otacku motoru je ulozena
// v stoupaniSetiny (HW setup). Viz StepsForDistance() nize.
#define MOTOR_STEPS_PER_REV  200   // krokovy motor 1.8 stupne
#define MICROSTEP            8     // TB6600: M1=1,M2=0,M3=1 pri behu (Confirm_Action) = 1/8 krok
#define POZICE_MAX           99999 // 999.99 zvolene jednotky
#define UHEL_REL_CELY        36000 // 360.00 stupne v setinach
#define UHEL_REL_MAX         35999 // nejvyssi zobrazitelna hodnota °Rel = 359.99
static int keypad_fresh = 0;  // 1 = prvni cislice zacne nove cislo (prepise stavajici)
// Benc je EXTI tlacitko. Bez pohybu pouze frontuje UI udalost, kterou hlavni
// smycka zpracuje mimo ISR (ochrana SW-SPI displeje). Pri pohybu nastavi STOP,
// ktery prerusi cekani i vsechny krokovaci smycky; ISR navic ihned stahne STEP.
static volatile uint8_t benc_event = 0;
static volatile uint32_t benc_last_ms = 0;  // softwarovy debounce
static volatile uint8_t motor_stop_request = 0;
static volatile uint8_t motor_moving = 0;
// Stav koncovych spinacu, udrzovany prerusenim EXTI3/4 (obe hrany, viz
// HAL_GPIO_EXTI_Callback). 1 = nektery spinac sepnut -> zadny pohyb.
static volatile uint8_t limit_stop = 0;
char message[100] = "Hello World!";
uint32_t last_print = 0, now = 0;
int pos = 0;
const uint8_t ICON_Flag[8]			={0x00,0x80,0xff,0x8e,0x0e,0x1c,0x18,0x10};
// ---- HW setup: konstanty ----
// Kalibrovany model rychlosti. Casova zakladna delay_us_motor() je TIM4, proto
// se odvozuje primo z hodinoveho stromu (viz SystemClock_Config a Posuv.ioc):
// HSE 16 MHz x PLL4 = 64 MHz SYSCLK, APB1 = /2 = 32 MHz, ale hodiny casovacu
// na APB1 se pri delicce != 1 zdvojnasobuji zpet na 64 MHz. TIM4 PSC = 64,
// takze 1 tick = 1 us.
// POZOR: drive tu byla pevna 500000 (podle HSE 8 MHz / 32 MHz SYSCLK). Krystal
// je ale 16 MHz, takze kazde cekani bylo o polovinu kratsi a vsechny posuvy
// jely presne 2x rychleji, nez ukazoval displej. Odvozeni z HSE_VALUE brani
// tomu, aby konstanta znovu tise zestarla.
#define MOTOR_SYSCLK_HZ    (HSE_VALUE * 4u)              // PLLMUL = 4
#define MOTOR_TIM4_HZ      (MOTOR_SYSCLK_HZ / 2u * 2u)   // APB1 /2, hodiny casovacu x2
#define MOTOR_TIM4_PSC     64u                           // MX_TIM4_Init: Prescaler = 64-1
#define MOTOR_TICKS_PER_S  (MOTOR_TIM4_HZ / MOTOR_TIM4_PSC)
// Strop krokove frekvence: 8 kHz pri 1/8 kroku = 4 ot/s. Rozjezd/dojezd
// resi akceleracni rampa (MotorMove/RampHalfTicks), limitem je tocivy moment
// motoru a rezie bit-bang smycky (polperioda 8 kHz = 63 ticku = 63 us).
// Po overeni na stroji lze doladit.
#define STEP_RATE_MAX      8000
#define STEP_HALF_MIN_TICKS ((MOTOR_TICKS_PER_S + 2*STEP_RATE_MAX - 1) / (2*STEP_RATE_MAX))
_Static_assert(STEP_HALF_MIN_TICKS >= 1 && STEP_HALF_MIN_TICKS <= 65535,
               "polperioda stropu rychlosti se musi vejit do delay_us_motor()");
#define JOG_MICROSTEP      4      // M1=1, M2=0, M3=0 pri rucnim rychloposuvu
// Poloha (poziceAktualniKroky) se vede v pulzech MICROSTEP. Rychloposuv jede
// v hrubsim JOG_MICROSTEP, takze jeden jeho pulz je JOG_STEP_WEIGHT pulzu polohy.
#define JOG_STEP_WEIGHT    (MICROSTEP / JOG_MICROSTEP)
_Static_assert(MICROSTEP % JOG_MICROSTEP == 0,
               "jog pulz musi byt cely nasobek pulzu polohy");
#define JOG_SPEED_MIN      1     // 0.01 zvolene jednotky/s
#define JOG_SPEED_MAX      99999 // 999.99 zvolene jednotky/s
#define JOG_ACCEL_MIN      1      // zvolena jednotka/s^2
#define JOG_ACCEL_MAX      999    // zvolena jednotka/s^2
#define SPEED_VALUE_MAX    99999  // 999.99 v setinach; STEP_RATE_MAX je fyzicky strop
#define STOUPANI_MIN       1      // 0.01 zvolene jednotky / otacku motoru
#define STOUPANI_MAX       99999  // 999.99 zvolene jednotky / otacku motoru
#define AKCEL_MIN          1      // zvolena jednotka/s^2
#define AKCEL_MAX          999    // zvolena jednotka/s^2

// ---- HW setup: hodnoty (perzistentni, viz Settings_Load/Save) ----
int stoupaniSetiny = 200; // 2.00 zvolene jednotky vystupu na jednu otacku motoru
int akcelerace = 50;      // Akcelerace/decelerace rampy [zvolena delka/s^2] (viz MotorMove)
int rychlostManualSetiny = 128; // RyM 1.28: nejblizsi setinova hodnota puvodni rychlosti
int rychlostManualAcc = 50;        // RyAcc [zvolena delka/s^2]
int rychlostManualDec = 50;        // RyDec [zvolena delka/s^2]
int orientace = 1;        // 1 = + doprava (vychozi), 0 = + doleva (fyzicky DIR invertovan)
int odpojeniMotoru = 1;   // 1 = ANO (v klidu uvolnit), 0 = NE (drzet moment)

enum {
	JEDNOTKA_DELKY_MM = 0,
	JEDNOTKA_DELKY_STUPNE_REL,
	JEDNOTKA_DELKY_STUPNE_ABS,
	JEDNOTKA_DELKY_OT
};
enum { JEDNOTKA_CASU_S = 0, JEDNOTKA_CASU_MIN };
int jednotkaDelky = JEDNOTKA_DELKY_MM;
int jednotkaCasu = JEDNOTKA_CASU_S;

static int IsRelativeDegreeUnit(void)
{
	return jednotkaDelky == JEDNOTKA_DELKY_STUPNE_REL;
}

// Pocet otacek motoru na jednu zvolenou jednotku vystupu jako zlomek.
// Stoupani je ulozeno v setinach vystupni jednotky na otacku motoru,
// proto 1 jednotka vystupu = 100/stoupaniSetiny otacky motoru.
static void LengthUnitRatio(int unit, int64_t *num, int64_t *den)
{
	(void)unit; // jednotka meni vyznam a popisek, prevod urcuje stoupani
	*num = 100;
	*den = stoupaniSetiny;
}

static int TimeUnitSeconds(int unit)
{
	return (unit == JEDNOTKA_CASU_MIN) ? 60 : 1;
}

static const char *LengthUnitText(void)
{
	switch (jednotkaDelky) {
		case JEDNOTKA_DELKY_STUPNE_REL: return "\xF8" "Rel";
		case JEDNOTKA_DELKY_STUPNE_ABS: return "\xF8" "Abs";
		case JEDNOTKA_DELKY_OT:          return "ot";
		default:                         return "mm";
	}
}

// Zkraceny text pro Vzdalenost, R a Max, aby se hodnoty se setinami
// vesly na 16 znaku displeje. Poz a Ink pouzivaji plne °Rel/°Abs.
static const char *LengthUnitShortText(void)
{
	switch (jednotkaDelky) {
		case JEDNOTKA_DELKY_STUPNE_REL: return "\xF8" "R";
		case JEDNOTKA_DELKY_STUPNE_ABS: return "\xF8" "A";
		case JEDNOTKA_DELKY_OT:          return "ot";
		default:                         return "mm";
	}
}

static const char *PitchUnitText(void)
{
	switch (jednotkaDelky) {
		case JEDNOTKA_DELKY_STUPNE_REL: return "\xF8" "R/otM";
		case JEDNOTKA_DELKY_STUPNE_ABS: return "\xF8" "A/otM";
		case JEDNOTKA_DELKY_OT:          return "otV/otM";
		default:                         return "mm/otM";
	}
}

static const char *TimeUnitText(void)
{
	return (jednotkaCasu == JEDNOTKA_CASU_MIN) ? "min" : "s";
}

static void FormatHundredths(char *out, size_t outSize, int value)
{
	int magnitude = (value < 0) ? -value : value;
	snprintf(out, outSize, "%s%d.%02d", value < 0 ? "-" : "",
	         magnitude/100, magnitude%100);
}

static int64_t DivRoundSigned(int64_t num, int64_t den)
{
	if (num >= 0) return (num + den/2) / den;
	return -((-num + den/2) / den);
}

// Prevede hodnotu mezi jednotkami delky tak, aby zustal zachovan fyzicky pohyb.
static int ConvertLengthValue(int value, int oldUnit, int newUnit)
{
	int64_t oldNum, oldDen, newNum, newDen;
	LengthUnitRatio(oldUnit, &oldNum, &oldDen);
	LengthUnitRatio(newUnit, &newNum, &newDen);
	return (int)DivRoundSigned((int64_t)value * oldNum * newDen,
	                           oldDen * newNum);
}

// Stejny prevod pro rychlost; navic zohledni s/min ve jmenovateli.
static int ConvertSpeedValue(int value, int oldLength, int oldTime,
	                         int newLength, int newTime)
{
	int64_t oldNum, oldDen, newNum, newDen;
	LengthUnitRatio(oldLength, &oldNum, &oldDen);
	LengthUnitRatio(newLength, &newNum, &newDen);
	int64_t num = (int64_t)value * oldNum * newDen * TimeUnitSeconds(newTime);
	int64_t den = oldDen * newNum * TimeUnitSeconds(oldTime);
	return (int)DivRoundSigned(num, den);
}

// Pocet STEP pulzu pro vzdalenost ulozenou v setinach zvolene jednotky.
// Deleni stem je soucasti jmenovatele, aby zustal vypocet cely v int64_t.
static int64_t StepsForDistance(int distanceHundredths)
{
	int64_t unitNum, unitDen;
	LengthUnitRatio(jednotkaDelky, &unitNum, &unitDen);
	int64_t num = (int64_t)distanceHundredths * MOTOR_STEPS_PER_REV * MICROSTEP * unitNum;
	int64_t den = unitDen * 100;
	return (num + den/2) / den;
}

// Opak StepsForDistance(): fyzicka poloha v STEP pulzech -> setiny zvolene
// jednotky. Pouziva se pro zivy ukazatel drahy Dr: (poloha od nuloveho bodu).
static int DistanceFromSteps(int64_t steps)
{
	int64_t unitNum, unitDen;
	LengthUnitRatio(jednotkaDelky, &unitNum, &unitDen);
	int64_t num = steps * unitDen * 100;
	int64_t den = (int64_t)MOTOR_STEPS_PER_REV * MICROSTEP * unitNum;
	if (den < 1) return 0;
	return (int)DivRoundSigned(num, den);
}

// Polperioda STEP pulzu (ticky TIM4, 2 us) pro rychlost v setinach zvolene
// jednotky/casu. Nasobek 100 v citateli kompenzuje setinnou reprezentaci.
static uint16_t StepHalfTicks(int speedHundredths)
{
	if (speedHundredths < 1) speedHundredths = 1;
	int64_t unitNum, unitDen;
	LengthUnitRatio(jednotkaDelky, &unitNum, &unitDen);
	int64_t num = (int64_t)MOTOR_TICKS_PER_S * unitDen
	            * TimeUnitSeconds(jednotkaCasu) * 100;
	int64_t den = (int64_t)2 * speedHundredths * MOTOR_STEPS_PER_REV
	            * MICROSTEP * unitNum;
	int64_t half = (num + den/2) / den;
	if (half < STEP_HALF_MIN_TICKS) half = STEP_HALF_MIN_TICKS;
	if (half > 65535) half = 65535;
	return (uint16_t)half;
}

// Nejvyssi dosazitelna rychlost v setinach zvolene jednotky za zadany cas.
static int PhysicalSpeedCapHundredths(int timeUnit)
{
	int64_t unitNum, unitDen;
	LengthUnitRatio(jednotkaDelky, &unitNum, &unitDen);
	int64_t v = (int64_t)STEP_RATE_MAX * unitDen
	          * TimeUnitSeconds(timeUnit) * 100
	          / ((int64_t)MOTOR_STEPS_PER_REV * MICROSTEP * unitNum);
	if (v < 1) v = 1;
	if (v > SPEED_VALUE_MAX) v = SPEED_VALUE_MAX;
	return (int)v;
}

// RyM je v setinach zvolene jednotky/s a pouziva 1/4 krok rychloposuvu.
static int PhysicalJogSpeedCapHundredths(void)
{
	int64_t unitNum, unitDen;
	LengthUnitRatio(jednotkaDelky, &unitNum, &unitDen);
	int64_t v = (int64_t)STEP_RATE_MAX * unitDen * 100
	          / ((int64_t)MOTOR_STEPS_PER_REV * JOG_MICROSTEP * unitNum);
	if (v < JOG_SPEED_MIN) v = JOG_SPEED_MIN;
	if (v > JOG_SPEED_MAX) v = JOG_SPEED_MAX;
	return (int)v;
}

static uint32_t JogTargetStepRate(void)
{
	int64_t unitNum, unitDen;
	LengthUnitRatio(jednotkaDelky, &unitNum, &unitDen);
	int64_t num = (int64_t)rychlostManualSetiny * MOTOR_STEPS_PER_REV
	            * JOG_MICROSTEP * unitNum;
	int64_t den = unitDen * 100;
	int64_t rate = (num + den/2) / den;
	if (rate < 1) rate = 1;
	if (rate > STEP_RATE_MAX) rate = STEP_RATE_MAX;
	return (uint32_t)rate;
}

// Max je vzdy ulozen a zobrazen za sekundu, nezavisle na volbe Cas.
static int MaxRychlostPerSecond(void)
{
	return PhysicalSpeedCapHundredths(JEDNOTKA_CASU_S);
}

// Rychlost R pouziva zvoleny Cas. Uzivatelsky limit Max proto prevedeme ze
// zvolene jednotky/s na stejnou jednotku/s nebo /min jako editovana rychlost.
static int RychlostEditMax(void)
{
	int64_t userCap = (int64_t)maxSpeed * TimeUnitSeconds(jednotkaCasu);
	int cap = PhysicalSpeedCapHundredths(jednotkaCasu);
	if (userCap < cap) cap = (int)userCap;
	if (cap < 1) cap = 1;
	if (cap > SPEED_VALUE_MAX) cap = SPEED_VALUE_MAX;
	return cap;
}

static void ClampSpeedValues(void)
{
	int cap = MaxRychlostPerSecond();
	if (maxSpeed < 1) maxSpeed = 1;
	if (maxSpeed > cap) maxSpeed = cap;
	if (rychlost < 1) rychlost = 1;
	cap = RychlostEditMax();
	if (rychlost > cap) rychlost = cap;
}

static void ClampJogValues(void)
{
	int cap = PhysicalJogSpeedCapHundredths();
	if (rychlostManualSetiny < JOG_SPEED_MIN) rychlostManualSetiny = JOG_SPEED_MIN;
	if (rychlostManualSetiny > cap) rychlostManualSetiny = cap;
	if (rychlostManualAcc < JOG_ACCEL_MIN) rychlostManualAcc = JOG_ACCEL_MIN;
	if (rychlostManualAcc > JOG_ACCEL_MAX) rychlostManualAcc = JOG_ACCEL_MAX;
	if (rychlostManualDec < JOG_ACCEL_MIN) rychlostManualDec = JOG_ACCEL_MIN;
	if (rychlostManualDec > JOG_ACCEL_MAX) rychlostManualDec = JOG_ACCEL_MAX;
}

// Koncove spinace: 1 = nektery je sepnuty -> zastavit kazdy pohyb (Auto,
// pozicni, jog) v obou smerech. Stav plni preruseni (EXTI3/4), tady se jen
// cte priznak - pohybove smycky nesahaji na GPIO.
static inline uint8_t LimitHit(void)
{
	return limit_stop;
}

// ---- Akceleracni rampa ----
// Celociselna odmocnina (pro vypocet rychlosti rampy).
static uint32_t Isqrt64(uint64_t x)
{
	uint64_t r = 0, bit = 1ULL << 62;
	while (bit > x) bit >>= 2;
	while (bit) {
		if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return (uint32_t)r;
}

// Dvojnasobek akcelerace v krocich/s^2 ze zvolene jednotky delky/s^2.
static int64_t AccelerationA2(int acceleration, int microstep)
{
	int64_t unitNum, unitDen;
	LengthUnitRatio(jednotkaDelky, &unitNum, &unitDen);
	int64_t num = (int64_t)2 * acceleration * MOTOR_STEPS_PER_REV
	            * microstep * unitNum;
	int64_t a2 = (num + unitDen/2) / unitDen;
	return (a2 < 1) ? 1 : a2;
}

static int64_t RampA2(void)
{
	return AccelerationA2(akcelerace, MICROSTEP);
}

// Polperioda n-teho kroku rozjezdu z klidu (n = 1, 2, ...): po n krocich je
// rychlost v_n = sqrt(2*a*n) [kroku/s] (konstantni akcelerace po draze).
// Nikdy nevrati mene nez cilovou polperiodu 'half_cil' (strop rychlosti).
static uint16_t RampHalfTicks(int64_t a2, int64_t n, uint16_t half_cil)
{
	uint32_t f = Isqrt64((uint64_t)a2 * (uint64_t)n);
	if (f < 1) f = 1;
	uint32_t half = MOTOR_TICKS_PER_S / (2*f);
	if (half < half_cil) half = half_cil;
	if (half > 65535) half = 65535;
	return (uint16_t)half;
}

// Provede 'kroky' STEP pulzu s lichobeznikovou rampou: rozjezd z klidu na
// 'rychlost' (akcelerace [zvolena delka/s^2]), konstantni jizda, dojezd zpet do klidu.
// Kratke pohyby prejdou na trojuhelnikovy profil (dojezd zacne v polovine,
// jakmile zbyva prave tolik kroku, kolik jich rozjezd spotreboval).
// Blokujici (jako drive) - UI bezi az po dokonceni pohybu.
// Vraci pocet skutecne provedenych kroku: mene nez 'kroky', pokud pohyb
// zastavil koncovy spinac nebo pozadavek STOP z tlacitka enkoderu.
static int64_t MotorMove(int64_t kroky)
{
	uint16_t half_cil = StepHalfTicks(rychlost);
	int64_t a2 = RampA2();
	int64_t n_rozjezd = 0;   // kroku spotrebovanych rozjezdem (= delka dojezdu)
	uint8_t plna = 0;        // 1 = cilova rychlost dosazena, jede se konstantne

	for (int64_t i = 0; i < kroky; i++) {
		if (motor_stop_request || LimitHit()) return i;
		uint16_t half;
		int64_t zbyva = kroky - i;
		if (zbyva <= n_rozjezd) {
			half = RampHalfTicks(a2, zbyva, half_cil);   // dojezd (zrcadlo rozjezdu)
		} else if (!plna) {
			n_rozjezd++;
			half = RampHalfTicks(a2, n_rozjezd, half_cil);
			if (half <= half_cil) plna = 1;
		} else {
			half = half_cil;
		}
		HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 1);
		delay_us_motor(half);
		HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 0);
		delay_us_motor(half);
	}
	return kroky;
}

// Rozjezd Auto posuvu: rampa bezi pres iterace hlavni smycky.
// auto_ramp_n = poradi kroku rampy; -1 = cilova rychlost dosazena.
static int64_t auto_ramp_n = 0;
static int auto_ramp_smer = -1;   // smer, pro ktery rozjezd probehl

// NULOVY BOD: navrat do nuloveho bodu. Pohyb bezi stejnou vetvi jako pozicni
// posuv (_inMenu==-2), jen s vlastnim poctem kroku -poziceAktualniKroky, tedy
// presnym odvinutim ujete drahy (u °Rel vcetne celych otacek).
static uint8_t navrat_do_nuly = 0;

// Co se prave edituje v _inMenu==-1 (viz Edit_Binding()).
enum {
	EDIT_NONE = 0, EDIT_RYCHLOST, EDIT_POZICE, EDIT_STOUPANI,
	EDIT_JEDNOTKA_DELKY, EDIT_JEDNOTKA_CASU, EDIT_MAXRYCH, EDIT_AKCEL,
	EDIT_RYM, EDIT_RYACC, EDIT_RYDEC
};
int edit_target = EDIT_NONE;
// Primy zapis numpadem bez otevrene editace: staci najet kurzorem na R:, Poz:
// nebo Ink: a psat cislice. EDIT_NONE = zadny primy zapis neprobiha.
static int direct_target = EDIT_NONE;
static int edit_old_jednotkaDelky = JEDNOTKA_DELKY_MM;
static int edit_old_jednotkaCasu = JEDNOTKA_CASU_S;

// U °Rel udrzuje zadani v rozsahu 0.00..359.99; ostatni jednotky jsou
// podepsane a maji obecny rozsah -999.99..999.99.
static void NormalizePositionValue(void)
{
	if (IsRelativeDegreeUnit()) {
		pozice %= UHEL_REL_CELY;
		if (pozice < 0) pozice += UHEL_REL_CELY;
	} else {
		if (pozice < -POZICE_MAX) pozice = -POZICE_MAX;
		if (pozice > POZICE_MAX) pozice = POZICE_MAX;
	}
}

// V absolutnim rezimu stupnu zvoli prepinac smer cesty k relativnimu cili.
static int64_t AbsolutePositionDeltaSteps(void)
{
	int64_t target = StepsForDistance(abs(pozice));
	if (pozice < 0) target = -target;
	if (!IsRelativeDegreeUnit()) return target - poziceAktualniKroky;

	int64_t fullTurn = StepsForDistance(UHEL_REL_CELY);
	if (fullTurn < 1 || smer_pohybu < 0) return 0;

	target %= fullTurn;
	int64_t current = poziceAktualniKroky % fullTurn;
	if (current < 0) current += fullTurn;

	int64_t delta = target - current;
	if (smer_pohybu == 1) {
		if (delta < 0) delta += fullTurn;
	} else {
		if (delta > 0) delta -= fullTurn;
	}
	return delta;
}

// Po potvrzeni nove jednotky preved hodnoty tak, aby se fyzicka rychlost,
// cilova poloha a akcelerace zmenily co nejmene.
static void ConvertUnitDependentValues(int oldLength, int oldTime)
{
	rychlost = ConvertSpeedValue(rychlost, oldLength, oldTime,
	                              jednotkaDelky, jednotkaCasu);
	maxSpeed = ConvertSpeedValue(maxSpeed, oldLength, JEDNOTKA_CASU_S,
	                              jednotkaDelky, JEDNOTKA_CASU_S);

	if (oldLength != jednotkaDelky) {
		pozice = ConvertLengthValue(pozice, oldLength, jednotkaDelky);
		akcelerace = ConvertLengthValue(akcelerace, oldLength, jednotkaDelky);
		rychlostManualSetiny = ConvertSpeedValue(rychlostManualSetiny,
		                                               oldLength, JEDNOTKA_CASU_S,
		                                               jednotkaDelky, JEDNOTKA_CASU_S);
		rychlostManualAcc = ConvertLengthValue(rychlostManualAcc, oldLength, jednotkaDelky);
		rychlostManualDec = ConvertLengthValue(rychlostManualDec, oldLength, jednotkaDelky);
	}

	NormalizePositionValue();
	if (akcelerace < AKCEL_MIN) akcelerace = AKCEL_MIN;
	if (akcelerace > AKCEL_MAX) akcelerace = AKCEL_MAX;
	ClampSpeedValues();
	ClampJogValues();
}

// Ukazatel na hodnotu 'target' (nebo NULL) + meze a krok enkoderu.
static int *Edit_Binding(int target, int *lo, int *hi, int *step)
{
	switch (target) {
		case EDIT_RYCHLOST: { int cap = RychlostEditMax();
		                    *lo = 1; *hi = cap;
		                    *step = 1; return &rychlost; }
		case EDIT_POZICE:
			// V Inkrementalnim je Ink jen velikost - smer dava prepinac,
			// takze zaporna hodnota nema vyznam. Absolutni cil zustava znamenkovy.
			if (IsRelativeDegreeUnit()) { *lo = 0; *hi = UHEL_REL_MAX; }
			else if (selected == 2)     { *lo = 0; *hi = POZICE_MAX; }
			else                        { *lo = -POZICE_MAX; *hi = POZICE_MAX; }
			*step = 1; return &pozice;
		case EDIT_STOUPANI:       *lo = STOUPANI_MIN;            *hi = STOUPANI_MAX;           *step = 1; return &stoupaniSetiny;
		case EDIT_JEDNOTKA_DELKY: *lo = JEDNOTKA_DELKY_MM;       *hi = JEDNOTKA_DELKY_OT;      *step = 1; return &jednotkaDelky;
		case EDIT_JEDNOTKA_CASU:  *lo = JEDNOTKA_CASU_S;         *hi = JEDNOTKA_CASU_MIN;      *step = 1; return &jednotkaCasu;
		case EDIT_MAXRYCH:        *lo = 1;                        *hi = MaxRychlostPerSecond(); *step = 1; return &maxSpeed;
		case EDIT_AKCEL:          *lo = AKCEL_MIN;                *hi = AKCEL_MAX;              *step = 1; return &akcelerace;
		case EDIT_RYM:            *lo = JOG_SPEED_MIN;            *hi = PhysicalJogSpeedCapHundredths(); *step = 1; return &rychlostManualSetiny;
		case EDIT_RYACC:          *lo = JOG_ACCEL_MIN;            *hi = JOG_ACCEL_MAX;          *step = 1; return &rychlostManualAcc;
		case EDIT_RYDEC:          *lo = JOG_ACCEL_MIN;            *hi = JOG_ACCEL_MAX;          *step = 1; return &rychlostManualDec;
		default: return NULL;
	}
}

// Kterou hodnotu lze zapsat numpadem primo z najeteho radku (_inMenu==0).
// Editace se neotevira, takze enkoder dal posouva kurzor.
static int DirectEditTarget(void)
{
	if (_inMenu != 0) return EDIT_NONE;
	if (selected == 0) return (inside_cursor == 0) ? EDIT_RYCHLOST : EDIT_NONE;
	if (selected == 1 || selected == 2) {
		if (inside_cursor == 0) return EDIT_RYCHLOST;
		if (inside_cursor == 1) return EDIT_POZICE;
	}
	return EDIT_NONE;   // HW setup se dal potvrzuje (uklada se do flash)
}

// Ukonci primy zapis se stejnymi mezemi a prepocty jako potvrzena editace.
// Vola se pri odjezdu kurzoru, potvrzeni, startu pohybu i navratu do menu.
static void DirectEntry_Finish(void)
{
	if (direct_target == EDIT_NONE) return;
	int was = direct_target;
	int lo, hi, step;
	int *v = Edit_Binding(was, &lo, &hi, &step);
	if (v) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }
	ClampSpeedValues();
	rychlost_last = rychlost;
	pozice_last = pozice;
	direct_target = EDIT_NONE;
	keypad_fresh = 0;
	if (was == EDIT_RYCHLOST) Settings_Save();  // R prezije i vypnuti napajeni
}

// Zapise cislici do hodnoty 'target' (spolecne pro editaci i primy zapis).
static void ApplyDigit(int target, char key)
{
	int lo, hi, step;
	int *v = Edit_Binding(target, &lo, &hi, &step);
	if (!v) return;

	int cur = keypad_fresh ? 0 : *v;
	if (cur < 0) cur = 0;                 // numpad zadava jen nezaporne
	cur = cur*10 + (key - '0');
	if (target == EDIT_POZICE && IsRelativeDegreeUnit()) {
		cur %= UHEL_REL_CELY;              // 360.00 -> 0.00, 361.00 -> 1.00
	} else if (cur > hi) {
		cur = hi;
	}
	*v = cur;
	keypad_fresh = 0;
	Inside_Draw();
}

// Aplikuje Orientaci na fyzickou uroven DIR. 'level' je vychozi (orientace=1) uroven pinu.
static inline GPIO_PinState DirApply(int level)
{
	int d = level ? 1 : 0;
	if (!orientace) d = !d;
	return d ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

static int ReadDirectionSwitch(void)
{
	if (HAL_GPIO_ReadPin(Left_GPIO_Port, Left_Pin)) return 1;
	if (HAL_GPIO_ReadPin(Right_GPIO_Port, Right_Pin)) return 0;
	return -1;
}

// START: 1/8 kroku, povoleni driveru a prechod do behoveho stavu _inMenu==-2.
static void MotorStart(void)
{
	motor_stop_request = 0;
	motor_moving = 1;
	_inMenu = -2;
	HAL_GPIO_WritePin(M1_GPIO_Port, M1_Pin, 1);
	HAL_GPIO_WritePin(M2_GPIO_Port, M2_Pin, 0);
	HAL_GPIO_WritePin(M3_GPIO_Port, M3_Pin, 1);
	HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, 1);
	HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, 1);
}

// Rychloposuv: po dobu drzeni zrychluje podle RyAcc, po uvolneni
// dojede do nuly podle RyDec. STOP a koncovy spinac rampu obchazeji.
// Vraci pocet provedenych pulzu (v JOG_MICROSTEP), aby volajici mohl
// dopocitat ujetou drahu do poziceAktualniKroky.
static int64_t JogMove(int direction)
{
	int64_t pulzy = 0;
	uint64_t speed2 = 0;
	uint64_t targetRate = JogTargetStepRate();
	uint64_t target2 = targetRate * targetRate;
	uint64_t accelerate2 = (uint64_t)AccelerationA2(rychlostManualAcc, JOG_MICROSTEP);
	uint64_t decelerate2 = (uint64_t)AccelerationA2(rychlostManualDec, JOG_MICROSTEP);

	HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(direction));
	HAL_GPIO_WritePin(M1_GPIO_Port, M1_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(M2_GPIO_Port, M2_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(M3_GPIO_Port, M3_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, GPIO_PIN_SET);

	while (!LimitHit() && !motor_stop_request) {
		uint8_t held = direction
		             ? HAL_GPIO_ReadPin(Bleft_fast_GPIO_Port, Bleft_fast_Pin)
		             : HAL_GPIO_ReadPin(Bright_fast_GPIO_Port, Bright_fast_Pin);

		if (held) {
			if (accelerate2 >= target2 - speed2) speed2 = target2;
			else speed2 += accelerate2;
		} else {
			if (speed2 <= decelerate2) break;
			speed2 -= decelerate2;
		}

		uint32_t rate = Isqrt64(speed2);
		if (rate < 1) rate = 1;
		uint32_t half = MOTOR_TICKS_PER_S / (2 * rate);
		if (half < STEP_HALF_MIN_TICKS) half = STEP_HALF_MIN_TICKS;
		if (half > 65535) half = 65535;

		HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, GPIO_PIN_SET);
		delay_us_motor((uint16_t)half);
		HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, GPIO_PIN_RESET);
		delay_us_motor((uint16_t)half);
		pulzy++;
	}
	HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, GPIO_PIN_RESET);
	return pulzy;
}

// ---- Trvale ulozeni nastaveni do flash (s rozlozenim opotrebeni) ----
// Poslednich 8 KB 128KB flash; program konci ~32 KB, takze je volno.
//
// R se uklada pri kazde zmene (radove stovky za den), takze prepisovat porad
// jedno misto by stranku vycerpalo za tydny (garantovanych 10 000 mazani).
// Rezervovana oblast je proto pole slotu velikosti jednoho zaznamu: zapisuje
// se vzdy do prvniho volneho slotu a maze se teprve az je cele pole plne.
// Na jedno mazani stranky tak pripada SETTINGS_SLOTS zapisu.
#define SETTINGS_FLASH_BASE  0x0801E000u        // 0x0801E000..0x0801FFFF
#define SETTINGS_PAGES       8u

typedef struct {
	int32_t  stoupaniSetiny;
	int32_t  maxSpeedSetiny; // Max vzdy ve zvolene jednotce/s
	int32_t  akcelerace;
	int32_t  rychlostManualSetiny;
	int32_t  rychlostManualAcc;
	int32_t  rychlostManualDec;
	int32_t  orientace;
	int32_t  odpojeniMotoru;
	int32_t  jednotkaDelky;
	int32_t  jednotkaCasu;
	int32_t  rychlostSetiny;   // R v setinach jednotek ze stejneho zaznamu
	uint32_t checksum;         // soucet predchozich slov
} SettingsFlash;

#define SETTINGS_WORDS       (sizeof(SettingsFlash)/4)
#define SETTINGS_SLOTS_PER_PAGE  (FLASH_PAGE_SIZE / sizeof(SettingsFlash))
#define SETTINGS_SLOTS       (SETTINGS_SLOTS_PER_PAGE * SETTINGS_PAGES)

_Static_assert(sizeof(SettingsFlash) % 4 == 0, "zaznam se zapisuje po slovech");
_Static_assert(SETTINGS_SLOTS_PER_PAGE >= 1, "zaznam se musi vejit do stranky");
_Static_assert(SETTINGS_FLASH_BASE + SETTINGS_PAGES * FLASH_PAGE_SIZE
               == 0x08020000u, "oblast musi koncit na konci 128KB flash");

static uint32_t Settings_Checksum(const SettingsFlash *s)
{
	const uint32_t *w = (const uint32_t *)s;
	uint32_t sum = 0;
	for (unsigned i = 0; i < SETTINGS_WORDS - 1; i++) sum += w[i];
	return sum;
}

// Adresa slotu. Slot nikdy neprechazi pres hranici stranky, aby zustal
// v celku i pri mazani po strankach (zbytek stranky za poslednim slotem
// se necha nevyuzity).
static const SettingsFlash *Settings_Slot(unsigned n)
{
	return (const SettingsFlash *)(SETTINGS_FLASH_BASE
	     + (n / SETTINGS_SLOTS_PER_PAGE) * FLASH_PAGE_SIZE
	     + (n % SETTINGS_SLOTS_PER_PAGE) * sizeof(SettingsFlash));
}

static int Settings_SlotErased(unsigned n)
{
	const uint32_t *w = (const uint32_t *)Settings_Slot(n);
	for (unsigned i = 0; i < SETTINGS_WORDS; i++)
		if (w[i] != 0xFFFFFFFFu) return 0;
	return 1;
}

// Posledni ulozeny zaznam = platny slot s nejvyssim indexem: zapisuje se vzdy
// do nejnizsiho volneho slotu, takze poradi slotu = poradi zapisu. Nedopsany
// slot (vypadek napajeni behem zapisu) neprojde souctem a preskoci se, takze
// zustane platny predchozi zaznam. Vymazany slot se platnym stat nemuze -
// soucet SETTINGS_WORDS-1 slov 0xFFFFFFFF je 0xFFFFFFF5, ne 0xFFFFFFFF.
// Vraci NULL, dokud nebyl ulozen zadny platny zaznam.
static const SettingsFlash *Settings_Latest(void)
{
	for (unsigned n = SETTINGS_SLOTS; n-- > 0; ) {
		const SettingsFlash *s = Settings_Slot(n);
		if (s->checksum == Settings_Checksum(s)) return s;
	}
	return NULL;
}

// Nacte pouze aktualni rozlozeni. Pri neplatnem souctu zustanou vychozi hodnoty.
static void Settings_Load(void)
{
	const SettingsFlash *s = Settings_Latest();
	if (s) {
		stoupaniSetiny = s->stoupaniSetiny;
		maxSpeed         = s->maxSpeedSetiny;
		akcelerace       = s->akcelerace;
		rychlostManualSetiny = s->rychlostManualSetiny;
		rychlostManualAcc = s->rychlostManualAcc;
		rychlostManualDec = s->rychlostManualDec;
		orientace        = s->orientace ? 1 : 0;
		odpojeniMotoru   = s->odpojeniMotoru ? 1 : 0;
		jednotkaDelky    = s->jednotkaDelky;
		jednotkaCasu     = s->jednotkaCasu;
		rychlost         = s->rychlostSetiny;
	}

	// Orez do platnych mezi chrani i proti poskozenym datum.
	if (stoupaniSetiny < STOUPANI_MIN) stoupaniSetiny = STOUPANI_MIN;
	if (stoupaniSetiny > STOUPANI_MAX) stoupaniSetiny = STOUPANI_MAX;
	if (jednotkaDelky < JEDNOTKA_DELKY_MM || jednotkaDelky > JEDNOTKA_DELKY_OT)
		jednotkaDelky = JEDNOTKA_DELKY_MM;
	if (jednotkaCasu < JEDNOTKA_CASU_S || jednotkaCasu > JEDNOTKA_CASU_MIN)
		jednotkaCasu = JEDNOTKA_CASU_S;
	if (akcelerace < AKCEL_MIN) akcelerace = AKCEL_MIN;
	if (akcelerace > AKCEL_MAX) akcelerace = AKCEL_MAX;
	ClampSpeedValues();
	ClampJogValues();
}

static int Settings_Same(const SettingsFlash *a, const SettingsFlash *b)
{
	const uint32_t *wa = (const uint32_t *)a;
	const uint32_t *wb = (const uint32_t *)b;
	for (unsigned i = 0; i < SETTINGS_WORDS; i++)
		if (wa[i] != wb[i]) return 0;
	return 1;
}

// Ulozi nastaveni do flash. Zapis probehne jen pri skutecne zmene a jde vzdy
// do dalsiho volneho slotu; mazani se dela az po zaplneni cele oblasti.
static void Settings_Save(void)
{
	SettingsFlash ns;
	ns.stoupaniSetiny = stoupaniSetiny;
	ns.maxSpeedSetiny = maxSpeed;
	ns.akcelerace       = akcelerace;
	ns.rychlostManualSetiny = rychlostManualSetiny;
	ns.rychlostManualAcc = rychlostManualAcc;
	ns.rychlostManualDec = rychlostManualDec;
	ns.orientace        = orientace;
	ns.odpojeniMotoru   = odpojeniMotoru;
	ns.jednotkaDelky    = jednotkaDelky;
	ns.jednotkaCasu     = jednotkaCasu;
	ns.rychlostSetiny   = rychlost;
	ns.checksum         = Settings_Checksum(&ns);

	const SettingsFlash *cur = Settings_Latest();
	if (cur && Settings_Same(cur, &ns)) return;   // beze zmeny -> zadny zapis

	// Prvni zcela vymazany slot. Nedopsany slot se preskoci (neni vymazany),
	// takze se do nej uz nikdy nezapisuje.
	unsigned slot = SETTINGS_SLOTS;
	for (unsigned n = 0; n < SETTINGS_SLOTS; n++) {
		if (Settings_SlotErased(n)) { slot = n; break; }
	}

	HAL_FLASH_Unlock();
	if (slot >= SETTINGS_SLOTS) {                 // oblast plna -> smazat celou
		FLASH_EraseInitTypeDef er = {0};
		er.TypeErase   = FLASH_TYPEERASE_PAGES;
		er.PageAddress = SETTINGS_FLASH_BASE;
		er.NbPages     = SETTINGS_PAGES;
		uint32_t pageError = 0;
		if (HAL_FLASHEx_Erase(&er, &pageError) != HAL_OK) {
			HAL_FLASH_Lock();
			return;
		}
		slot = 0;
	}

	// Poradi je dulezite: soucet je posledni slovo zaznamu, takze pri preruseni
	// zapisu zustane slot bez platneho souctu a Settings_Latest() ho preskoci.
	uint32_t addr = (uint32_t)(uintptr_t)Settings_Slot(slot);
	const uint32_t *w = (const uint32_t *)&ns;
	for (unsigned i = 0; i < SETTINGS_WORDS; i++) {
		HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i*4, w[i]);
	}
	HAL_FLASH_Lock();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM4_Init();
  /* USER CODE BEGIN 2 */

  HAL_TIM_Base_Start(&htim1);
  __HAL_TIM_SET_COUNTER(&htim1,0);

  ST7920_Init(&htim1);
  ST7920_GraphicMode(1);
  ST7920_Clear();
  Settings_Load();   // nacti ulozena nastaveni z flash (pred kreslenim menu)
  Menu_Init();
  HAL_Delay(1000);

  HAL_TIM_Encoder_Start(&htim2, TIM_CHANNEL_ALL);
  HAL_TIM_Base_Start(&htim4);

  HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, 0);
  HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, 0);
  HAL_GPIO_WritePin(LTC_GPIO_Port, LTC_Pin, 1);//Clock wise rotation
  HAL_GPIO_WritePin(M1_GPIO_Port, M1_Pin, 1);//Clock wise rotation
  HAL_GPIO_WritePin(M2_GPIO_Port, M2_Pin, 0);//Clock wise rotation
  HAL_GPIO_WritePin(M3_GPIO_Port, M3_Pin, 0);//Clock wise rotation
  HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 0);
  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(0));

  // Numpad sloupce potrebuji pull-up (membranova klavesnice nema vlastni odpory).
  // CubeMX je generuje jako NOPULL, prekonfigurujeme je zde v USER CODE.
  {
    GPIO_InitTypeDef kp = {0};
    kp.Mode  = GPIO_MODE_INPUT;
    kp.Pull  = GPIO_PULLUP;
    kp.Speed = GPIO_SPEED_FREQ_LOW;
    for (int c = 0; c < 4; c++) {
      kp.Pin = colPins[c];
      HAL_GPIO_Init(colPorts[c], &kp);
    }
  }

  // Koncove spinace: CubeMX je ma jen na nabeznou hranu a bez NVIC.
  // Prepneme na obe hrany (sepnuti i uvolneni) a povolime EXTI3/4 - stav
  // 'limit_stop' pak udrzuje vyhradne HAL_GPIO_EXTI_Callback, bez pollingu.
  {
    GPIO_InitTypeDef lim = {0};
    lim.Mode  = GPIO_MODE_IT_RISING_FALLING;
    lim.Pull  = GPIO_NOPULL;
    lim.Pin = Bleft_max_Pin;  HAL_GPIO_Init(Bleft_max_GPIO_Port, &lim);
    lim.Pin = Bright_max_Pin; HAL_GPIO_Init(Bright_max_GPIO_Port, &lim);
  }
  limit_stop = HAL_GPIO_ReadPin(Bleft_max_GPIO_Port, Bleft_max_Pin)
            || HAL_GPIO_ReadPin(Bright_max_GPIO_Port, Bright_max_Pin);  // vychozi stav
  HAL_NVIC_SetPriority(EXTI3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI3_IRQn);
  HAL_NVIC_SetPriority(EXTI4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI4_IRQn);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  // Auto posuv neni blokujici, proto jeho STOP zpracujeme na zacatku iterace.
	  if (motor_stop_request && _inMenu == -2 && selected == 0) {
		  _inMenu = 0;
		  motor_moving = 0;
		  motor_stop_request = 0;
		  Inside_Draw();
	  }

	  // Potvrzeni z tlacitka enkoderu (Benc) - zpracovano zde v hlavnim kontextu,
	  // aby kresleni nikdy nepreruusilo SW-SPI prenos na displej.
	  if (benc_event) {
		  benc_event = 0;
		  Confirm_Action();
	  }

	  // Numpad se necte behem behu motoru (_inMenu==-2), aby scan nenarusil
	  // casovani kroku; motor prerusuji EXTI tlacitka (Back/enkoder).
	  // Keypad_Task() je nyni neblokujici (sken bez cekani + debounce pres
	  // casova razitka), takze uz nezamrzava hlavni smycku ani scrollovani.
	  if (_inMenu != -2) {
		  Keypad_Task();
	  }

	  if (_inMenu>=1) {
		  Menu_Task();
		  HAL_GPIO_WritePin(RGB_B_GPIO_Port, RGB_B_Pin, 0);
		  HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 0);
		  HAL_GPIO_WritePin(RGB_G_GPIO_Port, RGB_G_Pin, 0);
	  } else if (_inMenu==0) {
		  Menu_Inside();
		  HAL_GPIO_WritePin(RGB_B_GPIO_Port, RGB_B_Pin, 0);
		  HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 0);
	  } else if (_inMenu==-1) { //ZMENY DAT
		  HAL_GPIO_WritePin(RGB_B_GPIO_Port, RGB_G_Pin, 1);

		  //HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 1);
		  int16_t enc1_now = Encoder_GetSteps();
		  int16_t delta1 = enc1_now - enc1_last;
		  if (delta1 >  ENC_HALF_RANGE) delta1 -= ENC_STEPS_RANGE; // wrapped forward
		  if (delta1 < -ENC_HALF_RANGE) delta1 += ENC_STEPS_RANGE; // wrapped backward
		  if (delta1 != 0) {
			  enc1_last = enc1_now;
			  int lo, hi, step;
			  int *v = Edit_Binding(edit_target, &lo, &hi, &step);
			  if (v) {
				  int nv = *v + (delta1 > 0 ? step : -step);
				  if (edit_target == EDIT_JEDNOTKA_DELKY ||
				      edit_target == EDIT_JEDNOTKA_CASU ||
				      (edit_target == EDIT_POZICE && IsRelativeDegreeUnit())) {
					  if (nv < lo) nv = hi;
					  if (nv > hi) nv = lo;
				  } else {
					  if (nv < lo) nv = lo;
					  if (nv > hi) nv = hi;
				  }
				  if (nv != *v) { *v = nv; Inside_Draw(); }
			  }
		  }
	  } else if (_inMenu==-2) {
		  Menu_Inside();
		  HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 1);

		  // Prepnuti smeru do stredu (stop) -> pristi rozbeh zase s rampou.
		  if (selected==0 && smer_pohybu<0) {
			  auto_ramp_smer = -1;
			  auto_ramp_n = 0;
		  }

		  if (navrat_do_nuly) {
			  // NULOVY BOD: presne odvinuti ujete drahy zpet do nuly.
			  int64_t deltaKroky = -poziceAktualniKroky;
			  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin,
			                    DirApply(deltaKroky >= 0 ? 1 : 0));

			  int64_t kroky = (deltaKroky >= 0) ? deltaKroky : -deltaKroky;
			  int64_t hotovo = MotorMove(kroky);
			  poziceAktualniKroky += (deltaKroky >= 0) ? hotovo : -hotovo;
			  navrat_do_nuly=0;
			  _inMenu=0;
			  motor_moving=0;
			  motor_stop_request=0;
			  Inside_Draw();
		  } else if (smer_pohybu>=0 && selected==0) {
			  if (LimitHit()) {
				  _inMenu=0;                                // koncovy spinac: ukoncit beh
				  motor_moving=0;
				  Inside_Draw();
			  } else {
				  // Zmena smeru (nebo prvni krok po startu/stopu) -> rozjezd znovu.
				  if (smer_pohybu != auto_ramp_smer) {
					  auto_ramp_smer = smer_pohybu;
					  auto_ramp_n = 0;
				  }
				  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(smer_pohybu));
				  uint16_t half = StepHalfTicks(rychlost);
				  if (auto_ramp_n >= 0) {                   // jeste se rozjizdi
					  auto_ramp_n++;
					  uint16_t rh = RampHalfTicks(RampA2(), auto_ramp_n, half);
					  if (rh <= half) auto_ramp_n = -1;     // cil dosazen
					  else half = rh;
				  }
				  HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 1);
				  delay_us_motor(half);  // Very short pulse
				  HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 0);
				  delay_us_motor(half);
				  // Do drahy Dr: se pocita i Auto posuv, aby NULOVY BOD
				  // umel vratit i to, co ujel volny posuv.
				  poziceAktualniKroky += smer_pohybu ? 1 : -1;
			  }
		  } else if (selected==1) {

			  int64_t deltaKroky = AbsolutePositionDeltaSteps();
			  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin,
			                    DirApply(deltaKroky >= 0 ? 1 : 0));

			  int64_t kroky = (deltaKroky >= 0) ? deltaKroky : -deltaKroky;
			  int64_t hotovo = MotorMove(kroky);
			  poziceAktualniKroky += (deltaKroky >= 0) ? hotovo : -hotovo;
			  _inMenu=0;
			  motor_moving=0;
			  motor_stop_request=0;
			  Inside_Draw();
		  } else if (selected==2) {

			  // Smer inkrementu urcuje prepinac (ve vsech jednotkach, ne jen
			  // v °Rel) - Ink je proto jen velikost kroku. Stred = zadny pohyb.
			  int smerKroku = smer_pohybu;
			  if (smerKroku >= 0) {
				  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(smerKroku));
				  int64_t kroky = StepsForDistance(abs(pozice));
				  int64_t hotovo = MotorMove(kroky);
				  poziceAktualniKroky += smerKroku ? hotovo : -hotovo;
			  }
			  _inMenu=0;
			  motor_moving=0;
			  motor_stop_request=0;
			  Inside_Draw();
		  }
	  }


	  // Po nouzovem STOPu rychloposuv znovu povol az po uvolneni prepinace.
	  uint8_t fastRight = HAL_GPIO_ReadPin(Bright_fast_GPIO_Port, Bright_fast_Pin);
	  uint8_t fastLeft = HAL_GPIO_ReadPin(Bleft_fast_GPIO_Port, Bleft_fast_Pin);
	  if (_inMenu != -2 && !fastRight && !fastLeft) {
		  motor_stop_request = 0;
	  }
	  if (_inMenu != -2 && !motor_stop_request && !LimitHit() &&
	      (fastRight || fastLeft)) {
		  int jogSmer = fastRight ? 0 : 1;
		  motor_moving = 1;
		  int64_t jogPulzy = JogMove(jogSmer);
		  motor_moving = 0;
		  // Rychloposuv jede v hrubsim kroku, prepocet na pulzy polohy.
		  poziceAktualniKroky += (jogSmer ? jogPulzy : -jogPulzy) * JOG_STEP_WEIGHT;
		  if (_inMenu<=0) Inside_Draw();   // Dr: se obnovi az po dojezdu
	  }
	  if (_inMenu!=-2) {
		  motor_moving = 0;
		  auto_ramp_smer = -1;   // mimo beh: pristi Auto start zacne rampou
		  auto_ramp_n = 0;
		  HAL_GPIO_WritePin(M1_GPIO_Port, M1_Pin, 1);//Clock wise rotation
		  HAL_GPIO_WritePin(M2_GPIO_Port, M2_Pin, 0);//Clock wise rotation
		  HAL_GPIO_WritePin(M3_GPIO_Port, M3_Pin, 0);//Clock wise rotation
		  // Odpojeni motoru v klidu: ANO = uvolnit, NE = drzet moment
		  if (odpojeniMotoru) {
			  HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, 0);
			  HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, 0);
		  } else {
			  HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, 1);
			  HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, 1);
		  }
		  HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 0);
	  }

	  //BACK BUTTON
	  if (HAL_GPIO_ReadPin(Bconf_GPIO_Port, Bconf_Pin)) {
		  // Back behem volby jednotky znamena zrusit nepotvrzenou volbu.
		  if (_inMenu == -1 &&
		      (edit_target == EDIT_JEDNOTKA_DELKY || edit_target == EDIT_JEDNOTKA_CASU)) {
			  jednotkaDelky = edit_old_jednotkaDelky;
			  jednotkaCasu = edit_old_jednotkaCasu;
		  }
		  edit_target=EDIT_NONE;
		  direct_target=EDIT_NONE;   // rozepsany primy zapis zahodit
		  navrat_do_nuly=0;
		  top=0;
		  _inMenu=2;
		  motor_moving=0;
		  motor_stop_request=0;
		  pozice=0;                  // R se pri navratu do menu nemaze (viz Settings_Load)
		  // poziceAktualniKroky (a tim Dr:) se maze jen tlacitkem VYNULOVAT,
		  // jinak by navrat do menu zahodil nulovy bod i ujetou drahu.
		  //HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 1);
	  }

	  //SMER POHYBU
	  int novy_smer = ReadDirectionSwitch();
	  if (_inMenu<=0 && novy_smer != smer_pohybu) {
		  smer_pohybu = novy_smer;
		  Inside_Draw();
	  }

	  //RESETING LED
	  if (!HAL_GPIO_ReadPin(Benc_GPIO_Port, Benc_Pin) && !HAL_GPIO_ReadPin(Bconf_GPIO_Port, Bconf_Pin)) {
		  HAL_GPIO_WritePin(RGB_B_GPIO_Port, RGB_B_Pin, 0);
		  HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 0);
		  HAL_GPIO_WritePin(RGB_G_GPIO_Port, RGB_G_Pin, 0);
	  }

/*
	  now = HAL_GetTick();
	  if (now - last_print >= 1000) {
		  sprintf(message, "Hello World! %d", ((TIM2->CNT))>>2);
		  ST7920_Clear();
		  GLCD_Font_Print(0,3, message);
		  ST7920_Update();
		  last_print = now;
	  }
*/

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV4;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC2_Init(void)
{

  /* USER CODE BEGIN ADC2_Init 0 */

  /* USER CODE END ADC2_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC2_Init 1 */

  /* USER CODE END ADC2_Init 1 */

  /** Common config
  */
  hadc2.Instance = ADC2;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.ContinuousConvMode = DISABLE;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC2_Init 2 */

  /* USER CODE END ADC2_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 64-1;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 0xffff-1;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_Encoder_InitTypeDef sConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 65535;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  sConfig.EncoderMode = TIM_ENCODERMODE_TI12;
  sConfig.IC1Polarity = TIM_ICPOLARITY_RISING;
  sConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC1Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC1Filter = 0;
  sConfig.IC2Polarity = TIM_ICPOLARITY_RISING;
  sConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC2Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC2Filter = 0;
  if (HAL_TIM_Encoder_Init(&htim2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM4_Init(void)
{

  /* USER CODE BEGIN TIM4_Init 0 */

  /* USER CODE END TIM4_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM4_Init 1 */

  /* USER CODE END TIM4_Init 1 */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 64-1;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 65535;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim4, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM4_Init 2 */

  /* USER CODE END TIM4_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, LTC_Pin|RGB_G_Pin|RGB_B_Pin|RGB_R_Pin
                          |M1_Pin|M2_Pin|M3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GLCD_CS_Pin|GLCD_SID_Pin|STEP_Pin|Num_R4_Pin
                          |Num_R3_Pin|Num_R2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GLCD_RST_Pin|DIR_Pin|Enable_Pin|Reset_Pin
                          |GLCD_SCK_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(Num_R1_GPIO_Port, Num_R1_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : LTC_Pin RGB_G_Pin RGB_B_Pin RGB_R_Pin
                           M1_Pin M2_Pin M3_Pin */
  GPIO_InitStruct.Pin = LTC_Pin|RGB_G_Pin|RGB_B_Pin|RGB_R_Pin
                          |M1_Pin|M2_Pin|M3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : Bconf_Pin Benc_Pin Bleft_max_Pin Bright_max_Pin
                           Bleft_fast_Pin Bright_fast_Pin */
  GPIO_InitStruct.Pin = Bconf_Pin|Benc_Pin|Bleft_max_Pin|Bright_max_Pin
                          |Bleft_fast_Pin|Bright_fast_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : GLCD_CS_Pin GLCD_SID_Pin STEP_Pin Num_R4_Pin
                           Num_R3_Pin Num_R2_Pin */
  GPIO_InitStruct.Pin = GLCD_CS_Pin|GLCD_SID_Pin|STEP_Pin|Num_R4_Pin
                          |Num_R3_Pin|Num_R2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : GLCD_RST_Pin DIR_Pin Enable_Pin Reset_Pin
                           GLCD_SCK_Pin */
  GPIO_InitStruct.Pin = GLCD_RST_Pin|DIR_Pin|Enable_Pin|Reset_Pin
                          |GLCD_SCK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : Right_Pin Left_Pin Num_C3_Pin Num_C2_Pin
                           Num_C1_Pin */
  GPIO_InitStruct.Pin = Right_Pin|Left_Pin|Num_C3_Pin|Num_C2_Pin
                          |Num_C1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : Num_C4_Pin */
  GPIO_InitStruct.Pin = Num_C4_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(Num_C4_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : Num_R1_Pin */
  GPIO_InitStruct.Pin = Num_R1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(Num_R1_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);

  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
// Sdilena akce potvrzeni. Vola se z EXTI (tlacitko enkoderu Benc) i z klavesy '#'.
static void Confirm_Action(void) {
	DirectEntry_Finish();              // rozepsane cislo z numpadu nejdriv dokonci
	if (_inMenu>=1) {
		Menu_Inside_Init();
		return;
	}
	if (_inMenu==-1) {                 // potvrzeni editace hodnoty -> zpet do modu
		int lo, hi, step;
		int *v = Edit_Binding(edit_target, &lo, &hi, &step);
		if (v) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }

		int was = edit_target;
		if (was == EDIT_JEDNOTKA_DELKY || was == EDIT_JEDNOTKA_CASU) {
			ConvertUnitDependentValues(edit_old_jednotkaDelky, edit_old_jednotkaCasu);
		} else {
			ClampSpeedValues(); // plati i po zmene Stoupani nebo Max rychlosti
			ClampJogValues();
		}

		rychlost_last = rychlost;
		pozice_last = pozice;
		edit_target = EDIT_NONE;
		if (was == EDIT_STOUPANI || was == EDIT_JEDNOTKA_DELKY ||
		    was == EDIT_JEDNOTKA_CASU || was == EDIT_MAXRYCH ||
		    was == EDIT_AKCEL || was == EDIT_RYM ||
		    was == EDIT_RYACC || was == EDIT_RYDEC ||
		    was == EDIT_RYCHLOST) Settings_Save();   // R prezije vypnuti napajeni
		_inMenu=0;
		enc_last = Encoder_GetSteps();   // re-sync cursor encoder so leaving edit doesn't jump
		Inside_Draw();
		return;
	}
	if (_inMenu==-2) {                 // beh motoru: Benc = STOP (Auto)
		_inMenu=0;
		motor_moving=0;
		motor_stop_request=0;
		Inside_Draw();
		return;
	}
	// ---- _inMenu==0 ----
	if (selected==3) {                 // HW setup
		edit_old_jednotkaDelky = jednotkaDelky;
		edit_old_jednotkaCasu = jednotkaCasu;
		switch (inside_cursor) {
			case 0: edit_target = EDIT_STOUPANI;       break;
			case 1: edit_target = EDIT_JEDNOTKA_DELKY; break;
			case 2: edit_target = EDIT_JEDNOTKA_CASU;  break;
			case 3: edit_target = EDIT_MAXRYCH;        break;
			case 4: edit_target = EDIT_AKCEL;          break;
			case 5: edit_target = EDIT_RYM;            break;
			case 6: edit_target = EDIT_RYACC;          break;
			case 7: edit_target = EDIT_RYDEC;          break;
			case 8: orientace = !orientace;           Settings_Save(); Inside_Draw(); return; // +/-
			case 9: odpojeniMotoru = !odpojeniMotoru; Settings_Save(); Inside_Draw(); return; // ANO / NE
			default: return;
		}
		_inMenu=-1;
		keypad_fresh = 1;
		enc1_last = Encoder_GetSteps();
		Inside_Draw();
		return;
	}
	// ---- mody 0,1,2 ----
	// Kurzor: 0 = R, 1 = Poz/Ink (mimo Auto), 2 = VYNULOVAT, 3 = NULOVY BOD,
	// 4 = START. Auto posuv ma jen 0 = R a 1 = START.
	if (inside_cursor==0 || (inside_cursor==1 && selected!=0)) {
		edit_target = (inside_cursor==0) ? EDIT_RYCHLOST : EDIT_POZICE;
		_inMenu=-1;
		keypad_fresh = 1;                // prvni cislice prepise stavajici hodnotu
		enc1_last = Encoder_GetSteps();  // seed edit encoder so first detent = +/-1, no jump
		Inside_Draw();
		return;
	}
	if (selected!=0 && inside_cursor==2) {
		poziceAktualniKroky=0;           // VYNULOVAT: tady je novy nulovy bod
		Inside_Draw();
		return;
	}
	if (selected!=0 && inside_cursor==3) {
		if (poziceAktualniKroky==0) {    // NULOVY BOD: uz v nule, neni co vracet
			Inside_Draw();
			return;
		}
		navrat_do_nuly=1;                // navrat resi vetev _inMenu==-2
		MotorStart();
		Inside_Draw();
		return;
	}
	if ((selected==0 && inside_cursor==1) || (selected!=0 && inside_cursor==4)) {
		smer_pohybu = ReadDirectionSwitch();
		// Stred prepinace blokuje START tam, kde smer urcuje prepinac:
		// Inkrementalni vzdy, Absolutni jen v °Rel. Auto se smi spustit i ve
		// stredu - jen nekroku, dokud se packa neprehodi.
		if (smer_pohybu < 0 &&
		    (selected == 2 || (selected == 1 && IsRelativeDegreeUnit()))) {
			Inside_Draw();
			return;
		}
		MotorStart();                    // START
		Inside_Draw();
	}
}

// Cteni numpadu. Cislice edituji hodnotu, '#' potvrzuje a '*' bez hlaseni
// prepina Odpojeni motoru mezi ANO/NE.
static void Keypad_Task(void) {
	// Neblokujici debounce pres casova razitka. Klavesa se ohlasi jednou pri
	// stabilnim stisku (hrana nic->klavesa). Zadne blokujici cekani, takze
	// hlavni smycka bezi dal a enkoder se stale cte.
	static char last_stable = 0;    // posledni oddebouncovany stav (0 = nic)
	static char candidate   = 0;    // kandidat behem debouncingu
	static uint32_t cand_since = 0;

	char raw = Keypad_ScanRaw();
	uint32_t t = HAL_GetTick();

	if (raw != candidate) {         // zmena vstupu -> restart debounce okna
		candidate  = raw;
		cand_since = t;
		return;
	}
	if ((t - cand_since) < KEYPAD_DEBOUNCE_MS) return;  // jeste neni stabilni
	if (candidate == last_stable) return;               // uz ohlaseno

	last_stable = candidate;
	if (candidate == 0) return;     // uvolneni klavesy -> zadna akce

	char key = candidate;           // nova stisknuta klavesa (jednorazova udalost)

	if (key == '#') {                 // '#' = potvrdit (jako Benc)
		Confirm_Action();
		return;
	}
	if (key == '*') {                 // tichy prepinac Odpojeni motoru ANO/NE
		odpojeniMotoru = !odpojeniMotoru;
		Settings_Save();
		return;
	}

	if (key < '0' || key > '9') return;

	if (_inMenu == -1) {              // otevrena editace hodnoty
		if (edit_target == EDIT_JEDNOTKA_DELKY ||
		    edit_target == EDIT_JEDNOTKA_CASU) return; // jednotky jen enkoderem
		ApplyDigit(edit_target, key);
		return;
	}

	// Primy zapis: staci najet kurzorem na R:, Poz: nebo Ink: a psat.
	// Editace se neotevira, takze enkoder dal posouva kurzor a odjezd z radku
	// (nebo potvrzeni) hodnotu dorovna na meze - viz DirectEntry_Finish().
	int target = DirectEditTarget();
	if (target == EDIT_NONE) return;
	if (target != direct_target) {    // nove pole -> prvni cislice ho prepise
		DirectEntry_Finish();
		direct_target = target;
		keypad_fresh = 1;
	}
	ApplyDigit(target, key);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {

	//KONCOVE SPINACE (EXTI3/4, obe hrany): aktualizuj 'limit_stop' podle
	//urovne pinu a pri sepnuti okamzite odpoj driver - motor se zastavi
	//jeste driv, nez pohybova smycka dojde ke kontrole priznaku.
	if (GPIO_Pin == Bleft_max_Pin || GPIO_Pin == Bright_max_Pin) {
		limit_stop = HAL_GPIO_ReadPin(Bleft_max_GPIO_Port, Bleft_max_Pin)
		          || HAL_GPIO_ReadPin(Bright_max_GPIO_Port, Bright_max_Pin);
		if (limit_stop) {
			HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, 0);
			HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, 0);
		}
		return;
	}

	// CONFIRM / STOP Z TLACITKA ENKODERU
	if ( GPIO_Pin == Benc_Pin ) {
		// Debounce zustava v ISR; displej se odsud nikdy nekresli.
		uint32_t now_ms = HAL_GetTick();
		if (now_ms - benc_last_ms >= 150) {
			benc_last_ms = now_ms;
			if (motor_moving) {
				motor_stop_request = 1;
				HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, GPIO_PIN_RESET);
			} else {
				benc_event = 1;
			}
		}
	}
}
void delay_us_motor (uint16_t us) {
	__HAL_TIM_SET_COUNTER(&htim4,0);
	while (__HAL_TIM_GET_COUNTER(&htim4) < us && !motor_stop_request);
}

// Keep selected visible by adjusting 'top'
static void Menu_KeepVisible(void)
{
	if (selected < top) top = selected;
	if (selected >= top + MENU_VISIBLE_LINES) top = selected - (MENU_VISIBLE_LINES - 1);

	if (top < 0) top = 0;
	if (top > MENU_COUNT - MENU_VISIBLE_LINES) top = MENU_COUNT - MENU_VISIBLE_LINES;
	if (top < 0) top = 0;
}

static void Menu_Draw(void)
{
	GLCD_Buf_Clear();   // clear RAM buffer only; ST7920_Update() repaints every pixel


	// Menu lines start at y=1 so debug fits on y=0
	for (int line = 0; line < MENU_VISIBLE_LINES; line++) {
		int idx = top + line;
		if (idx >= MENU_COUNT) break;

		char buf[24];
		if (idx == selected) snprintf(buf, sizeof(buf), ">%s", menu[idx]);
		else                 snprintf(buf, sizeof(buf), " %s", menu[idx]);

		GLCD_Font_Print(0, (uint8_t)(line+idx), buf);
	}

	ST7920_Update();
}

#define HW_MENU_COUNT 10
#define HW_MENU_VISIBLE_LINES 7

static int Inside_CursorCount(int sel)
{
    switch (sel) {
        case 0: return 2; // Rychlost, START
        case 1: return 5; // Rychlost, Pozice, VYNULOVAT, NULOVY BOD, START
        case 2: return 5; // Rychlost, Inkrement, VYNULOVAT, NULOVY BOD, START
        case 3: return HW_MENU_COUNT;
        default: return 1;
    }
}

static void Inside_KeepVisible(void)
{
	if (selected != 3) {
		inside_top = 0;
		return;
	}
	if (inside_cursor < inside_top) inside_top = inside_cursor;
	if (inside_cursor >= inside_top + HW_MENU_VISIBLE_LINES)
		inside_top = inside_cursor - (HW_MENU_VISIBLE_LINES - 1);
	if (inside_top < 0) inside_top = 0;
	if (inside_top > HW_MENU_COUNT - HW_MENU_VISIBLE_LINES)
		inside_top = HW_MENU_COUNT - HW_MENU_VISIBLE_LINES;
}
// Posledni radek pohybovych obrazovek: ujeta draha od nuloveho bodu.
// Zamerne se neobnovuje periodicky behem pohybu - jedno prekresleni displeje
// trva desitky ms a delalo by diry v krokovaci smycce. Hodnota se tedy
// dopocita az kdyz se pohyb zastavi (dojezd, STOP, koncovy spinac).
static void PrintDrahaLine(void)
{
	char buf[24];
	char value[16];
	FormatHundredths(value, sizeof(value), DistanceFromSteps(poziceAktualniKroky));
	snprintf(buf, sizeof(buf), "Dr:%s%s", value, LengthUnitText());
	PrintLineSel(7, buf, 0);
}

static void Inside_Draw(void) {
	char buf[32];
	char value[16];
	const char* pohybChar;

	GLCD_Buf_Clear();   // clear RAM buffer only; ST7920_Update() repaints every pixel

	if (smer_pohybu==1) {
		pohybChar="Prava";
	} else if (smer_pohybu==0) {
		pohybChar="Leva";
	} else {
		pohybChar="Stop";
	}
	switch (selected)
	{
		/* ===================== 0: AUTO POSUV ===================== */
		case 0:
			FormatHundredths(value, sizeof(value), rychlost);
			snprintf(buf, sizeof(buf), "R:%s%s/%s", value, LengthUnitShortText(), TimeUnitText());
			PrintLineSel(1, buf, (inside_cursor == 0));

			snprintf(buf, sizeof(buf), "Smer:%s", pohybChar);
			PrintLineSel(2, buf, 0);

			if (_inMenu!=-2) {
				PrintLineSel(6, "         START", (inside_cursor == 1));
			} else {
				PrintLineSel(6, "          STOP", (inside_cursor == 1));
			}
			PrintDrahaLine();
			break;

		/* ===================== 1: ABSOLUTNI ===================== */
		case 1:
			FormatHundredths(value, sizeof(value), rychlost);
			snprintf(buf, sizeof(buf), "R:%s%s/%s", value, LengthUnitShortText(), TimeUnitText());
			PrintLineSel(1, buf, (inside_cursor == 0));

			if (IsRelativeDegreeUnit()) {
				snprintf(buf, sizeof(buf), "Smer:%s", pohybChar);
			} else {
				int64_t deltaKroky = AbsolutePositionDeltaSteps();
				if (deltaKroky>0) {
					snprintf(buf, sizeof(buf), "Smer:%s", "Prava");
				} else if (deltaKroky<0) {
					snprintf(buf, sizeof(buf), "Smer:%s", "Leva");
				} else {
					snprintf(buf, sizeof(buf), "Smer:%s", "Stop");
				}
			}
			PrintLineSel(2, buf, 0);

			FormatHundredths(value, sizeof(value), pozice);
			snprintf(buf, sizeof(buf), "Poz:%s%s", value, LengthUnitText());
			PrintLineSel(3, buf, (inside_cursor == 1));

			PrintLineSel(4, "    VYNULOVAT", (inside_cursor == 2));
			PrintLineSel(5, "    NULOVY BOD", (inside_cursor == 3));

			if (_inMenu!=-2) {
				PrintLineSel(6, "         START", (inside_cursor == 4));
			} else {
				PrintLineSel(6, "         CEKEJ", (inside_cursor == 4));
			}
			PrintDrahaLine();
			break;

		/* ===================== 2: INKREMENTALNI ===================== */
		case 2:
			FormatHundredths(value, sizeof(value), rychlost);
			snprintf(buf, sizeof(buf), "R:%s%s/%s", value, LengthUnitShortText(), TimeUnitText());
			PrintLineSel(1, buf, (inside_cursor == 0));

			// Smer inkrementu drzi prepinac ve vsech jednotkach.
			snprintf(buf, sizeof(buf), "Smer:%s", pohybChar);
			PrintLineSel(2, buf, 0);

			FormatHundredths(value, sizeof(value), pozice);
			snprintf(buf, sizeof(buf), "Ink:%s%s", value, LengthUnitText());
			PrintLineSel(3, buf, (inside_cursor == 1));

			PrintLineSel(4, "    VYNULOVAT", (inside_cursor == 2));
			PrintLineSel(5, "    NULOVY BOD", (inside_cursor == 3));

			if (_inMenu!=-2) {
				PrintLineSel(6, "         START", (inside_cursor == 4));
			} else {
				PrintLineSel(6, "         CEKEJ", (inside_cursor == 4));
			}
			PrintDrahaLine();
			break;

		/* ===================== 3: HW SETUP ===================== */
		case 3:
			for (int item = inside_top;
			     item < HW_MENU_COUNT && item < inside_top + HW_MENU_VISIBLE_LINES;
			     item++) {
				switch (item) {
					case 0:
						FormatHundredths(value, sizeof(value), stoupaniSetiny);
						snprintf(buf, sizeof(buf), "S:%s%s", value, PitchUnitText());
						break;
					case 1:
						snprintf(buf, sizeof(buf), "Vzdalenost[%s]", LengthUnitShortText());
						break;
					case 2:
						snprintf(buf, sizeof(buf), "Cas[%s]", TimeUnitText());
						break;
					case 3:
						FormatHundredths(value, sizeof(value), maxSpeed);
						snprintf(buf, sizeof(buf), "Max:%s%s/s", value, LengthUnitShortText());
						break;
					case 4:
						snprintf(buf, sizeof(buf), "Acc:%d%s/s2", akcelerace, LengthUnitText());
						break;
					case 5:
						FormatHundredths(value, sizeof(value), rychlostManualSetiny);
						snprintf(buf, sizeof(buf), "RyM:%s%s/s", value, LengthUnitShortText());
						break;
					case 6:
						snprintf(buf, sizeof(buf), "RyAcc:%d%s/s2", rychlostManualAcc, LengthUnitShortText());
						break;
					case 7:
						snprintf(buf, sizeof(buf), "RyDec:%d%s/s2", rychlostManualDec, LengthUnitShortText());
						break;
					case 8:
						snprintf(buf, sizeof(buf), "Orient:%c", orientace ? '+' : '-');
						break;
					case 9:
						snprintf(buf, sizeof(buf), "Odpoj mot:%s", odpojeniMotoru ? "ANO" : "NE");
						break;
					default:
						buf[0] = '\0';
						break;
				}
				PrintLineSel((uint8_t)(item - inside_top + 1), buf,
				             inside_cursor == item);
			}
			break;

		default:
			GLCD_Font_Print(0, 0, "Neznamy stav");
			break;
	}

	ST7920_Update();
}

// Call once at startup (after TIM2 is configured/running)
void Menu_Init(void)
{
    enc_last = Encoder_GetSteps();
    selected = 0;
    top = 0;
    _inMenu = 1;
    Menu_Draw();
}
// Call once at startup (after TIM2 is configured/running)
void Menu_Inside_Init(void)
{
    enc_last = Encoder_GetSteps();
    inside_cursor=0;
    inside_top=0;
    direct_target=EDIT_NONE;
    navrat_do_nuly=0;
    _inMenu=0;
    // Auto posuv se rozjede uz otevrenim polozky - START se nemacka a kurzor
    // proto rovnou stoji na radku STOP. Prepinac smeru ve stredu pohyb drzi,
    // stejne jako sepnuty koncovy spinac (viz behova vetev _inMenu==-2).
    if (selected==0) {
        // Auto posuv je volny posuv, takze Dr: ma ukazovat drahu ujetou od
        // otevreni polozky - nulovy bod se nastavi sem. (Absolutni a
        // Inkrementalni si naopak nulovy bod drzi, meni ho jen VYNULOVAT.)
        poziceAktualniKroky = 0;
        smer_pohybu = ReadDirectionSwitch();
        inside_cursor = 1;
        MotorStart();
    } else if (selected==1 || selected==2) {
        // Kurzor rovnou na Poz:/Ink: - hodnotu, ktera se meni nejcasteji, tak
        // jde zadat numpadem hned po otevreni, bez scrollovani a bez potvrzeni
        // (viz DirectEditTarget()).
        inside_cursor = 1;
    }
    Inside_Draw();
}

// Call this repeatedly in main loop (or from a 10–50 ms timer tick)
void Menu_Task(void)
{
    int16_t enc_now = Encoder_GetSteps();
    int16_t delta = enc_now - enc_last;
    if (delta >  ENC_HALF_RANGE) delta -= ENC_STEPS_RANGE; // wrapped forward
	if (delta < -ENC_HALF_RANGE) delta += ENC_STEPS_RANGE; // wrapped backward

    if (delta != 0 || _inMenu==2) {
        _inMenu = 1;

        if (delta != 0) {
            // Update last immediately so we don't “repeat” the same movement
            enc_last = enc_now;

            // Move selection. If your encoder direction feels reversed, swap +/-
            if (delta > 0) selected++;
            else           selected--;

            // Wrap around
            if (selected < 0) selected = MENU_COUNT - 1;
            if (selected >= MENU_COUNT) selected = 0;

            Menu_KeepVisible();
        }

        Menu_Draw();
    }
}
void Menu_Inside(void) {
	int16_t enc_now = Encoder_GetSteps();
	int16_t delta = enc_now - enc_last;
	if (delta >  ENC_HALF_RANGE) delta -= ENC_STEPS_RANGE; // wrapped forward
	if (delta < -ENC_HALF_RANGE) delta += ENC_STEPS_RANGE; // wrapped backward

	if (delta != 0) {
		enc_last = enc_now;
		DirectEntry_Finish();   // odjezd z radku ukonci primy zapis numpadem

		// move cursor by 1 per movement (encoder direction is inverted centrally)
		if (delta > 0) inside_cursor++;
		else       inside_cursor--;

		int cnt = Inside_CursorCount(selected);
		if (inside_cursor < 0) inside_cursor = cnt - 1;
		if (inside_cursor >= cnt) inside_cursor = 0;
		Inside_KeepVisible();

		Inside_Draw(); // redraw only when encoder moved
	}
}
// Neblokujici sken klavesnice: vrati prvni prave stisknutou klavesu, nebo 0.
// ZADNE HAL_Delay a ZADNE cekani na uvolneni - to drive zamrzalo hlavni smycku
// (a scrollovani enkoderem). Debounce a detekci hrany resi Keypad_Task().
static char Keypad_ScanRaw(void)
{
    for (int r = 0; r < 4; r++)
    {
        /* Set all rows HIGH */
        for (int i = 0; i < 4; i++)
            HAL_GPIO_WritePin(rowPorts[i], rowPins[i], GPIO_PIN_SET);

        /* Pull current row LOW */
        HAL_GPIO_WritePin(rowPorts[r], rowPins[r], GPIO_PIN_RESET);

        /* Kratke ustaleni radku (radove us) - kratke busy-wait, ne HAL_Delay. */
        for (volatile int s = 0; s < 200; s++) { __NOP(); }

        /* Read columns (active LOW because pull-ups) */
        for (int c = 0; c < 4; c++)
        {
            if (HAL_GPIO_ReadPin(colPorts[c], colPins[c]) == GPIO_PIN_RESET)
                return keymap[r][c];
        }
    }

    return 0;
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
