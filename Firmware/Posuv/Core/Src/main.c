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
static void Keypad_Task(void);      // cteni numpadu: cislice/# do editace, # = potvrdit
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
static int16_t enc1_last = 0;

//POMOCNY RYCHLO FUNKCE
static inline int16_t Encoder_GetSteps(void)
{
    return (int16_t)(TIM2->CNT >> 2);
}
static void PrintLineSel(uint8_t y, const char *text, uint8_t active)
{
    char buf[24];
    if (active) snprintf(buf, sizeof(buf), ">%s", text);
    else        snprintf(buf, sizeof(buf), " %s ", text);
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

// Promene na nastaveni
int rychlost = 1;     // mm/s
int rychlost_last = 0;
int pozice = 0;       // mm
int pozice_last = 0;
int poziceAktualni = 0;
int smer_pohybu = 1;   // 1 = Pravo, 0 = Levo, -1 = stop

uint32_t adcValue1;
uint32_t adcValue2;
int maxSpeed = 70;   // "Max rychlost": uzivatelsky strop 'rychlost' (<= MAXSPEED_HARD_CAP)
// Kalibrace vzdalenosti: kroky/mm = kroky motoru na otacku * mikrokrokovani
// / stoupani sroubu (stoupaniUm, HW setup). Viz StepsForMm() nize.
#define MOTOR_STEPS_PER_REV  200   // krokovy motor 1.8 stupne
#define MICROSTEP            8     // TB6600: M1=1,M2=0,M3=1 pri behu (Confirm_Action) = 1/8 krok
const int POZICE_MAX = 999;   // horni mez pro zadani pozice z klavesnice (mm)
static int keypad_fresh = 0;  // 1 = prvni cislice zacne nove cislo (prepise stavajici)
// Tlacitko enkoderu (Benc) jen nastavi tento priznak v ISR; kresleni displeje
// se deje az v hlavni smycce, aby ISR nepreteklo probihajici SW-SPI prenos na
// ST7920 (jinak se displej rozsype - nahodne pixely/znaky).
static volatile uint8_t benc_event = 0;
static volatile uint32_t benc_last_ms = 0;  // softwarovy debounce
// Stav koncovych spinacu, udrzovany prerusenim EXTI3/4 (obe hrany, viz
// HAL_GPIO_EXTI_Callback). 1 = nektery spinac sepnut -> zadny pohyb.
static volatile uint8_t limit_stop = 0;
char message[100] = "Hello World!";
uint32_t last_print = 0, now = 0;
int pos = 0;
const uint8_t ICON_Flag[8]			={0x00,0x80,0xff,0x8e,0x0e,0x1c,0x18,0x10};
// ---- HW setup: konstanty ----
// Kalibrovany model rychlosti: TIM4 bezi na 32 MHz (APB1 16 MHz x2) / 64
// = 0.5 MHz, takze 1 tick delay_us_motor() = 2 us.
#define MOTOR_TICKS_PER_S  500000
// Strop krokove frekvence: 8 kHz pri 1/8 kroku = 4 ot/s. Rozjezd/dojezd
// resi akceleracni rampa (MotorMove/RampHalfTicks), limitem je tocivy moment
// motoru a rezie bit-bang smycky (polperioda 8 kHz = 32 ticku = 64 us).
// Po overeni na stroji lze doladit.
#define STEP_RATE_MAX      8000
#define STEP_HALF_MIN_TICKS ((MOTOR_TICKS_PER_S + 2*STEP_RATE_MAX - 1) / (2*STEP_RATE_MAX))
#define JOG_SPEED          490    // rychly jog: pevna polperioda (ticky), bezpecny krok
#define MAXSPEED_HARD_CAP  70     // tovarni strop Max rychlost [mm/s]
#define STOUPANI_MIN       1      // 0.001 mm
#define STOUPANI_MAX       99999  // 99.999 mm
#define AKCEL_MIN          1      // mm/s^2
#define AKCEL_MAX          999    // mm/s^2

// ---- HW setup: hodnoty (perzistentni, viz Settings_Load/Save) ----
int stoupaniUm = 2000;    // Stoupani zavitu v tisicinach mm (krok 0.001 mm) - kalibrace
int akcelerace = 50;      // Akcelerace/decelerace rampy [mm/s^2] (viz MotorMove)
int orientace = 1;        // 1 = + doprava (vychozi), 0 = + doleva (fyzicky DIR invertovan)
int odpojeniMotoru = 1;   // 1 = ANO (v klidu uvolnit), 0 = NE (drzet moment)

// Pocet STEP pulzu pro posuv o 'mm' milimetru, ze stoupani sroubu a
// mikrokrokovani (zaokrouhleno na nejblizsi krok). 64bit kvuli krajnim
// hodnotam (stoupani az 0.001 mm, pozice az 999 mm by preteklo int).
static int64_t StepsForMm(int mm)
{
	int64_t num = (int64_t)mm * MOTOR_STEPS_PER_REV * MICROSTEP * 1000;
	return (num + stoupaniUm/2) / stoupaniUm;
}

// Zpetny prevod: kolik mm (zaokrouhlene) odpovida 'kroky' STEP pulzum.
// Pro dopocet skutecne pozice po pohybu prerusenem koncovym spinacem.
static int MmForSteps(int64_t kroky)
{
	int64_t den = (int64_t)MOTOR_STEPS_PER_REV * MICROSTEP * 1000;
	return (int)((kroky * stoupaniUm + den/2) / den);
}

// Polperioda STEP pulzu (ticky TIM4, 2 us) pro rychlost v mm/s pri aktualnim
// stoupani: f [kroku/s] = v * kroky/mm; polperioda = (ticky/s) / (2*f).
// Spodni mez STEP_HALF_MIN_TICKS drzi krokovou frekvenci pod STEP_RATE_MAX.
static uint16_t StepHalfTicks(int v_mms)
{
	if (v_mms < 1) v_mms = 1;
	int64_t half = (int64_t)MOTOR_TICKS_PER_S * stoupaniUm
	             / ((int64_t)2 * v_mms * MOTOR_STEPS_PER_REV * MICROSTEP * 1000);
	if (half < STEP_HALF_MIN_TICKS) half = STEP_HALF_MIN_TICKS;
	if (half > 65535) half = 65535;
	return (uint16_t)half;
}

// Nejvyssi dosazitelna rychlost [mm/s] pri aktualnim stoupani (dana stropem
// krokove frekvence STEP_RATE_MAX). Minimalne 1, aby slo vzdy neco zvolit.
static int MaxRychlostMms(void)
{
	int64_t v = (int64_t)STEP_RATE_MAX * stoupaniUm
	          / ((int64_t)MOTOR_STEPS_PER_REV * MICROSTEP * 1000);
	if (v < 1) v = 1;
	if (v > MAXSPEED_HARD_CAP) v = MAXSPEED_HARD_CAP;
	return (int)v;
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

// Dvojnasobek akcelerace v krocich/s^2 (z 'akcelerace' [mm/s^2] a stoupani).
static int64_t RampA2(void)
{
	int64_t a2 = ((int64_t)2 * akcelerace * MOTOR_STEPS_PER_REV * MICROSTEP * 1000
	             + stoupaniUm/2) / stoupaniUm;
	return (a2 < 1) ? 1 : a2;
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
// 'rychlost' (akcelerace [mm/s^2]), konstantni jizda, dojezd zpet do klidu.
// Kratke pohyby prejdou na trojuhelnikovy profil (dojezd zacne v polovine,
// jakmile zbyva prave tolik kroku, kolik jich rozjezd spotreboval).
// Blokujici (jako drive) - UI bezi az po dokonceni pohybu.
// Vraci pocet skutecne provedenych kroku: mene nez 'kroky', pokud pohyb
// zastavil koncovy spinac.
static int64_t MotorMove(int64_t kroky)
{
	uint16_t half_cil = StepHalfTicks(rychlost);
	int64_t a2 = RampA2();
	int64_t n_rozjezd = 0;   // kroku spotrebovanych rozjezdem (= delka dojezdu)
	uint8_t plna = 0;        // 1 = cilova rychlost dosazena, jede se konstantne

	for (int64_t i = 0; i < kroky; i++) {
		if (LimitHit()) return i;    // koncovy spinac: okamzite zastavit
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

// Co se prave edituje v _inMenu==-1 (viz Edit_Binding()).
enum { EDIT_NONE = 0, EDIT_RYCHLOST, EDIT_POZICE, EDIT_STOUPANI, EDIT_MAXRYCH, EDIT_AKCEL };
int edit_target = EDIT_NONE;

// Ukazatel na prave editovanou hodnotu (nebo NULL) + meze a krok enkoderu.
static int *Edit_Binding(int *lo, int *hi, int *step)
{
	switch (edit_target) {
		case EDIT_RYCHLOST: { int cap = MaxRychlostMms();
		                    *lo = 1; *hi = (maxSpeed < cap) ? maxSpeed : cap;
		                    *step = 1; return &rychlost; }
		case EDIT_POZICE:   *lo = -POZICE_MAX;  *hi = POZICE_MAX;        *step = 1; return &pozice;
		case EDIT_STOUPANI: *lo = STOUPANI_MIN; *hi = STOUPANI_MAX;      *step = 1; return &stoupaniUm;
		case EDIT_MAXRYCH:  *lo = 1;            *hi = MAXSPEED_HARD_CAP; *step = 1; return &maxSpeed;
		case EDIT_AKCEL:    *lo = AKCEL_MIN;    *hi = AKCEL_MAX;         *step = 1; return &akcelerace;
		default: return NULL;
	}
}

// Aplikuje Orientaci na fyzickou uroven DIR. 'level' je vychozi (orientace=1) uroven pinu.
static inline GPIO_PinState DirApply(int level)
{
	int d = level ? 1 : 0;
	if (!orientace) d = !d;
	return d ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

// ---- Trvale ulozeni HW-setup nastaveni do flash ----
// Posledni 1KB stranka 128KB flash; program konci ~22KB, takze je volna.
#define SETTINGS_FLASH_PAGE  0x0801FC00u
#define SETTINGS_MAGIC       0x53503631u   // "SP61" - platny zaznam

typedef struct {
	uint32_t magic;
	int32_t  stoupaniUm;
	int32_t  maxSpeed;
	int32_t  akcelerace;
	int32_t  orientace;
	int32_t  odpojeniMotoru;
	uint32_t checksum;   // soucet predchozich slov
} SettingsFlash;

static uint32_t Settings_Checksum(const SettingsFlash *s)
{
	const uint32_t *w = (const uint32_t *)s;
	uint32_t sum = 0;
	for (unsigned i = 0; i < sizeof(SettingsFlash)/4 - 1; i++) sum += w[i];
	return sum;
}

// Nacte nastaveni z flash (pokud je platne), jinak necha zkompilovane defaulty.
static void Settings_Load(void)
{
	const SettingsFlash *s = (const SettingsFlash *)SETTINGS_FLASH_PAGE;
	if (s->magic != SETTINGS_MAGIC) return;
	if (s->checksum != Settings_Checksum(s)) return;
	stoupaniUm     = s->stoupaniUm;
	maxSpeed       = s->maxSpeed;
	akcelerace     = s->akcelerace;
	orientace      = s->orientace ? 1 : 0;
	odpojeniMotoru = s->odpojeniMotoru ? 1 : 0;
	// orez do platnych mezi (ochrana proti poskozenym datum)
	if (stoupaniUm < STOUPANI_MIN) stoupaniUm = STOUPANI_MIN;
	if (stoupaniUm > STOUPANI_MAX) stoupaniUm = STOUPANI_MAX;
	if (maxSpeed < 1) maxSpeed = 1;
	if (maxSpeed > MAXSPEED_HARD_CAP) maxSpeed = MAXSPEED_HARD_CAP;
	if (akcelerace < AKCEL_MIN) akcelerace = AKCEL_MIN;
	if (akcelerace > AKCEL_MAX) akcelerace = AKCEL_MAX;
	if (rychlost > maxSpeed) rychlost = maxSpeed;
}

// Ulozi nastaveni do flash. Eraze+zapis jen kdyz se neco opravdu zmenilo (setri flash).
static void Settings_Save(void)
{
	SettingsFlash ns;
	ns.magic          = SETTINGS_MAGIC;
	ns.stoupaniUm     = stoupaniUm;
	ns.maxSpeed       = maxSpeed;
	ns.akcelerace     = akcelerace;
	ns.orientace      = orientace;
	ns.odpojeniMotoru = odpojeniMotoru;
	ns.checksum       = Settings_Checksum(&ns);

	const SettingsFlash *cur = (const SettingsFlash *)SETTINGS_FLASH_PAGE;
	if (cur->magic == ns.magic && cur->checksum == ns.checksum &&
	    cur->stoupaniUm == ns.stoupaniUm && cur->maxSpeed == ns.maxSpeed &&
	    cur->akcelerace == ns.akcelerace && cur->orientace == ns.orientace &&
	    cur->odpojeniMotoru == ns.odpojeniMotoru) {
		return;   // uz je ulozeno totez
	}

	HAL_FLASH_Unlock();
	FLASH_EraseInitTypeDef er = {0};
	er.TypeErase   = FLASH_TYPEERASE_PAGES;
	er.PageAddress = SETTINGS_FLASH_PAGE;
	er.NbPages     = 1;
	uint32_t pageError = 0;
	if (HAL_FLASHEx_Erase(&er, &pageError) == HAL_OK) {
		const uint32_t *w = (const uint32_t *)&ns;
		for (unsigned i = 0; i < sizeof(SettingsFlash)/4; i++) {
			HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, SETTINGS_FLASH_PAGE + i*4, w[i]);
		}
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
			  int *v = Edit_Binding(&lo, &hi, &step);
			  if (v) {
				  int nv = *v + (delta1 > 0 ? step : -step);
				  if (nv < lo) nv = lo;
				  if (nv > hi) nv = hi;
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

		  if (smer_pohybu>=0 && selected==0) {
			  if (LimitHit()) {
				  _inMenu=0;                                // koncovy spinac: ukoncit beh
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
			  }
		  } else if (selected==1) {

			  int delta = pozice-poziceAktualni;   // mm se znamenkem

			  if (delta>=0) {
				  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(1));
			  } else {
				  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(0));
			  }

			  int64_t kroky = StepsForMm(abs(delta));
			  int64_t hotovo = MotorMove(kroky);
			  if (hotovo == kroky) {
				  poziceAktualni = pozice;
			  } else {
				  // Koncovy spinac: dopocitej, kam se skutecne dojelo.
				  int mm = MmForSteps(hotovo);
				  poziceAktualni += (delta >= 0) ? mm : -mm;
			  }
			  _inMenu=0;
			  Inside_Draw();
		  } else if (selected==2) {

			  if (pozice>=0) {
				  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(1));
			  } else if (pozice<=0) {
				  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(0));
			  }

			  MotorMove(StepsForMm(abs(pozice)));
			  _inMenu=0;
			  Inside_Draw();
		  }
	  }


	  while (HAL_GPIO_ReadPin(Bright_fast_GPIO_Port, Bright_fast_Pin) && !LimitHit()) {
		  HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 1);
		  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(0));//Anti clock wise rotation
		  HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, 1);
		  HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, 1);

		  HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 1);
		  delay_us_motor(JOG_SPEED);  // Very short pulse
		  HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 0);
		  delay_us_motor(JOG_SPEED);
	  }
	  while (HAL_GPIO_ReadPin(Bleft_fast_GPIO_Port, Bleft_fast_Pin) && !LimitHit()) {
		  HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 1);
		  HAL_GPIO_WritePin(DIR_GPIO_Port, DIR_Pin, DirApply(1));//Anti clock wise rotation
		  HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, 1);
		  HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, 1);

		  HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 1);
		  delay_us_motor(JOG_SPEED);  // Very short pulse
		  HAL_GPIO_WritePin(STEP_GPIO_Port, STEP_Pin, 0);
		  delay_us_motor(JOG_SPEED);
	  }
	  if (_inMenu!=-2) {
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
		  top=0;
		  _inMenu=2;
		  rychlost=1;
		  pozice=0;
		  poziceAktualni=0;
		  //HAL_GPIO_WritePin(RGB_R_GPIO_Port, RGB_R_Pin, 1);
	  }

	  //SMER POHYBU
	  if (HAL_GPIO_ReadPin(Left_GPIO_Port, Left_Pin)) {
		  if (_inMenu<=0 && smer_pohybu!=1) {
			  smer_pohybu = 1;
			  Inside_Draw();
		  }
	  } else if (HAL_GPIO_ReadPin(Right_GPIO_Port, Right_Pin)) {
		  if (_inMenu<=0 && smer_pohybu!=0) {
			  smer_pohybu = 0;
			  Inside_Draw();
		  }
	  } else {
		  if (_inMenu<=0 && smer_pohybu!=-1) {
			  smer_pohybu = -1;
			  Inside_Draw();
		  }
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
	if (_inMenu>=1) {
		Menu_Inside_Init();
		return;
	}
	if (_inMenu==-1) {                 // potvrzeni editace hodnoty -> zpet do modu
		int lo, hi, step;
		int *v = Edit_Binding(&lo, &hi, &step);
		if (v) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }
		if (rychlost < 1) rychlost = 1;               // platna rychlost i po numpadu
		if (rychlost > maxSpeed) rychlost = maxSpeed; // po zmene Max rychlost srovnej strop
		{ int cap = MaxRychlostMms();                 // po zmene Stoupani srovnej dosazitelnou mez
		  if (rychlost > cap) rychlost = cap; }
		rychlost_last = rychlost;
		pozice_last = pozice;
		int was = edit_target;
		edit_target = EDIT_NONE;
		if (was == EDIT_STOUPANI || was == EDIT_MAXRYCH || was == EDIT_AKCEL) Settings_Save();
		_inMenu=0;
		enc_last = Encoder_GetSteps();   // re-sync cursor encoder so leaving edit doesn't jump
		Inside_Draw();
		return;
	}
	if (_inMenu==-2) {                 // beh motoru: Benc = STOP (Auto)
		if (inside_cursor==1) { _inMenu=0; Inside_Draw(); }
		return;
	}
	// ---- _inMenu==0 ----
	if (selected==3) {                 // HW setup
		switch (inside_cursor) {
			case 0: edit_target = EDIT_STOUPANI; break;
			case 1: edit_target = EDIT_MAXRYCH;  break;
			case 2: edit_target = EDIT_AKCEL;    break;
			case 3: orientace = !orientace;           Settings_Save(); Inside_Draw(); return; // +/-
			case 4: odpojeniMotoru = !odpojeniMotoru; Settings_Save(); Inside_Draw(); return; // ANO / NE
			default: return;
		}
		_inMenu=-1;                     // ciselne polozky se editaji jako rychlost/pozice
		keypad_fresh = 1;
		enc1_last = Encoder_GetSteps();
		Inside_Draw();
		return;
	}
	// ---- mody 0,1,2 ----
	if (inside_cursor==0 || (inside_cursor==1 && selected!=0)) {
		edit_target = (inside_cursor==0) ? EDIT_RYCHLOST : EDIT_POZICE;
		_inMenu=-1;
		keypad_fresh = 1;                // prvni cislice prepise stavajici hodnotu
		enc1_last = Encoder_GetSteps();  // seed edit encoder so first detent = +/-1, no jump
		Inside_Draw();
	} else if ((inside_cursor==1 && selected==0) || (inside_cursor==3)) {
		_inMenu=-2;                      // START
		HAL_GPIO_WritePin(M1_GPIO_Port, M1_Pin, 1);//Clock wise rotation
		HAL_GPIO_WritePin(M2_GPIO_Port, M2_Pin, 0);//Clock wise rotation
		HAL_GPIO_WritePin(M3_GPIO_Port, M3_Pin, 1);//Clock wise rotation
		HAL_GPIO_WritePin(Reset_GPIO_Port, Reset_Pin, 1);
		HAL_GPIO_WritePin(Enable_GPIO_Port, Enable_Pin, 1);
		Inside_Draw();
	} else if (inside_cursor==2) {
		poziceAktualni=0;               // NULOVY BOD
		Inside_Draw();
	}
}

// Cteni numpadu. Cislice a '*' upravuji editovanou hodnotu (jen v _inMenu==-1),
// '#' se chova jako tlacitko enkoderu (potvrdit).
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

	if (_inMenu != -1) return;        // cislice davaji smysl jen pri editaci hodnoty

	int lo, hi, step;
	int *v = Edit_Binding(&lo, &hi, &step);
	if (!v) return;

	if (key >= '0' && key <= '9') {
		int d = key - '0';
		int cur = keypad_fresh ? 0 : *v;
		if (cur < 0) cur = 0;             // numpad zadava jen nezaporne
		cur = cur*10 + d;
		if (cur > hi) cur = hi;
		*v = cur;
		keypad_fresh = 0;
		Inside_Draw();
	} else if (key == '*') {          // '*' = smazat posledni cislici (backspace)
		*v /= 10;
		keypad_fresh = 0;
		Inside_Draw();
	}
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

	//CONFIRM BUTTON
	if ( GPIO_Pin == Benc_Pin ) {
		// Jen zaznamenat udalost + debounce; kresleni resi hlavni smycka (viz benc_event).
		uint32_t now_ms = HAL_GetTick();
		if (now_ms - benc_last_ms >= 150) {
			benc_last_ms = now_ms;
			benc_event = 1;
		}
	}
}
void delay_us_motor (uint16_t us) {
	__HAL_TIM_SET_COUNTER(&htim4,0);
	while (__HAL_TIM_GET_COUNTER(&htim4) < us);
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

static int Inside_CursorCount(int sel)
{
    switch (sel) {
        case 0: return 2; // Rychlost, START
        case 1: return 4; // Rychlost, NULOVY BOD (or Pozice if you prefer)
        case 2: return 4; // Rychlost, NULOVY BOD (or Inkrement if you prefer)
        case 3: return 5; // 4 visible setup lines (you listed 5; screen has 4 rows)
        default: return 1;
    }
}
static void Inside_Draw(void) {
	char buf[32];
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
			snprintf(buf, sizeof(buf), "Tempo: %02d mm/s", rychlost);
			PrintLineSel(1, buf, (inside_cursor == 0));

			snprintf(buf, sizeof(buf), "Smer:%s", pohybChar);
			PrintLineSel(2, buf, 0);

			// Big visual separation
			if (_inMenu!=-2) {
				PrintLineSel(7, "         START", (inside_cursor == 1));
			} else {
				PrintLineSel(7, "          STOP", (inside_cursor == 1));
			}
			break;

		/* ===================== 1: ABSOLUTNI ===================== */
		case 1:
			snprintf(buf, sizeof(buf), "Tempo: %02d mm/s", rychlost);
			PrintLineSel(1, buf, (inside_cursor == 0));

			if ((pozice-poziceAktualni)>0) {
				snprintf(buf, sizeof(buf), "Smer:%s", "Prava");
			} else if ((pozice-poziceAktualni)<0) {
				snprintf(buf, sizeof(buf), "Smer:%s", "Leva");
			} else {
				snprintf(buf, sizeof(buf), "Smer:%s", "Stop");
			}
			PrintLineSel(2, buf, 0);


			snprintf(buf, sizeof(buf), "Pozice: %02d mm", pozice);
			PrintLineSel(4, buf, (inside_cursor == 1));

			PrintLineSel(6, "    NULOVY BOD", (inside_cursor == 2));

			if (_inMenu!=-2) {
				PrintLineSel(7, "         START", (inside_cursor == 3));
			} else {
				PrintLineSel(7, "         CEKEJ", (inside_cursor == 3));
			}
			break;

		/* ===================== 2: INKREMENTALNI ===================== */
		case 2:
			snprintf(buf, sizeof(buf), "Tempo: %02d mm/s", rychlost);
			PrintLineSel(1, buf, (inside_cursor == 0));

			if (pozice>0) {
				snprintf(buf, sizeof(buf), "Smer:%s", "Prava");
			} else if (pozice<0) {
				snprintf(buf, sizeof(buf), "Smer:%s", "Leva");
			} else {
				snprintf(buf, sizeof(buf), "Smer:%s", "Stop");
			}
			PrintLineSel(2, buf, 0);


			snprintf(buf, sizeof(buf), "Inkre: %02d mm", pozice);
			PrintLineSel(4, buf, (inside_cursor == 1));

			PrintLineSel(6, "    NULOVY BOD", (inside_cursor == 2));

			if (_inMenu!=-2) {
				PrintLineSel(7, "         START", (inside_cursor == 3));
			} else {
				PrintLineSel(7, "         CEKEJ", (inside_cursor == 3));
			}

			break;

		/* ===================== 3: HW SETUP ===================== */
		case 3:
			snprintf(buf, sizeof(buf), "Stoup:%d.%03dmm", stoupaniUm/1000, stoupaniUm%1000);
			PrintLineSel(1, buf, (inside_cursor == 0));
			snprintf(buf, sizeof(buf), "MaxR: %d mm/s", maxSpeed);
			PrintLineSel(2, buf, (inside_cursor == 1));
			snprintf(buf, sizeof(buf), "Akcel:%dmm/s2", akcelerace);
			PrintLineSel(3, buf, (inside_cursor == 2));
			snprintf(buf, sizeof(buf), "Orient: %c", orientace ? '+' : '-');
			PrintLineSel(4, buf, (inside_cursor == 3));
			snprintf(buf, sizeof(buf), "Odpoj mot: %s", odpojeniMotoru ? "ANO" : "NE");
			PrintLineSel(5, buf, (inside_cursor == 4));
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
    _inMenu=0;
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
	uint16_t enc_now = Encoder_GetSteps();
	int16_t delta = enc_now - enc_last;
	if (delta >  ENC_HALF_RANGE) delta -= ENC_STEPS_RANGE; // wrapped forward
	if (delta < -ENC_HALF_RANGE) delta += ENC_STEPS_RANGE; // wrapped backward

	if (delta != 0) {
		enc_last = enc_now;

		// move cursor by 1 per movement (stable, no crazy jumps)
		if (delta > 0) inside_cursor++;
		else       inside_cursor--;

		int cnt = Inside_CursorCount(selected);
		if (inside_cursor < 0) inside_cursor = cnt - 1;
		if (inside_cursor >= cnt) inside_cursor = 0;

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
