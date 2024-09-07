/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
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
#define Button_conf_Pin GPIO_PIN_0
#define Button_conf_GPIO_Port GPIOC
#define Button_prec_Pin GPIO_PIN_1
#define Button_prec_GPIO_Port GPIOC
#define Button_gen_stop_Pin GPIO_PIN_2
#define Button_gen_stop_GPIO_Port GPIOC
#define Button_left_max_Pin GPIO_PIN_3
#define Button_left_max_GPIO_Port GPIOC
#define GLCD_CS_Pin GPIO_PIN_6
#define GLCD_CS_GPIO_Port GPIOA
#define GLCD_SID_Pin GPIO_PIN_7
#define GLCD_SID_GPIO_Port GPIOA
#define Button_right_max_Pin GPIO_PIN_4
#define Button_right_max_GPIO_Port GPIOC
#define GLCD_RST_Pin GPIO_PIN_0
#define GLCD_RST_GPIO_Port GPIOB
#define DIR_Pin GPIO_PIN_10
#define DIR_GPIO_Port GPIOB
#define RGB_B_Pin GPIO_PIN_7
#define RGB_B_GPIO_Port GPIOC
#define RGB_G_Pin GPIO_PIN_8
#define RGB_G_GPIO_Port GPIOC
#define RGB_R_Pin GPIO_PIN_9
#define RGB_R_GPIO_Port GPIOC
#define STEP_Pin GPIO_PIN_8
#define STEP_GPIO_Port GPIOA
#define GLCD_SCK_Pin GPIO_PIN_6
#define GLCD_SCK_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
