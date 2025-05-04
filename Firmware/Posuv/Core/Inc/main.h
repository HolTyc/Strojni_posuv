/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LTC_Pin GPIO_PIN_13
#define LTC_GPIO_Port GPIOC
#define Bconf_Pin GPIO_PIN_0
#define Bconf_GPIO_Port GPIOC
#define Benc_Pin GPIO_PIN_1
#define Benc_GPIO_Port GPIOC
#define Bgen_stop_Pin GPIO_PIN_2
#define Bgen_stop_GPIO_Port GPIOC
#define Bleft_max_Pin GPIO_PIN_3
#define Bleft_max_GPIO_Port GPIOC
#define GLCD_CS_Pin GPIO_PIN_6
#define GLCD_CS_GPIO_Port GPIOA
#define GLCD_SID_Pin GPIO_PIN_7
#define GLCD_SID_GPIO_Port GPIOA
#define Bright_max_Pin GPIO_PIN_4
#define Bright_max_GPIO_Port GPIOC
#define Bleft_fast_Pin GPIO_PIN_5
#define Bleft_fast_GPIO_Port GPIOC
#define GLCD_RST_Pin GPIO_PIN_0
#define GLCD_RST_GPIO_Port GPIOB
#define Right_Pin GPIO_PIN_1
#define Right_GPIO_Port GPIOB
#define Left_Pin GPIO_PIN_2
#define Left_GPIO_Port GPIOB
#define DIR_Pin GPIO_PIN_10
#define DIR_GPIO_Port GPIOB
#define Bright_fast_Pin GPIO_PIN_6
#define Bright_fast_GPIO_Port GPIOC
#define RGB_G_Pin GPIO_PIN_7
#define RGB_G_GPIO_Port GPIOC
#define RGB_B_Pin GPIO_PIN_8
#define RGB_B_GPIO_Port GPIOC
#define RGB_R_Pin GPIO_PIN_9
#define RGB_R_GPIO_Port GPIOC
#define STEP_Pin GPIO_PIN_8
#define STEP_GPIO_Port GPIOA
#define Enc_CLK_Pin GPIO_PIN_10
#define Enc_CLK_GPIO_Port GPIOA
#define Enc_DT_Pin GPIO_PIN_11
#define Enc_DT_GPIO_Port GPIOA
#define M1_Pin GPIO_PIN_10
#define M1_GPIO_Port GPIOC
#define M2_Pin GPIO_PIN_11
#define M2_GPIO_Port GPIOC
#define M3_Pin GPIO_PIN_12
#define M3_GPIO_Port GPIOC
#define GLCD_SCK_Pin GPIO_PIN_6
#define GLCD_SCK_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
