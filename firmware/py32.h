// Minimal PY32F002B register map: the peripherals main.c uses. Layouts and addresses derived from Puya's
// py32f002bx5.h (Copyright (c) Puya Semiconductor, Apache-2.0, via IOsetting/py32f0-template; see NOTICE).
// Modified: trimmed to what this firmware needs.
#pragma once
#include <stdint.h>
#define IO volatile uint32_t

typedef struct { IO CR, ICSCR, CFGR, R0, ECSCR, R1, CIER, CIFR, CICR, IOPRSTR, AHBRSTR, APBRSTR1, APBRSTR2,
                 IOPENR, AHBENR, APBENR1, APBENR2; } RCC_T;
typedef struct { IO MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2], BRR; } GPIO_T;
typedef struct { IO CR1, CR2, OAR1, R0, DR, SR1, SR2, CCR, TRISE; } I2C_T;
typedef struct { IO ISR, IER, CR, CFGR1, CFGR2, SMPR, R0[2], TR, R1, CHSELR, R2[5], DR, CCSR; } ADC_T;
typedef struct { IO CR1, CR2, SMCR, DIER, SR, EGR, CCMR1, CCMR2, CCER, CNT, PSC, ARR, RCR, CCR[4], BDTR; } TIM_T;
typedef struct { IO KR, PR, RLR, SR; } IWDG_T;
typedef struct { IO RTSR, FTSR, SWIER, PR, R0[20], EXTICR[2]; } EXTI_T;
typedef struct { IO CTRL, LOAD, VAL; } SYST_T;

#define RCC    ((RCC_T *)0x40021000)
#define GPIOA  ((GPIO_T *)0x50000000)
#define GPIOB  ((GPIO_T *)0x50000400)
#define I2C1   ((I2C_T *)0x40005400)
#define ADC1   ((ADC_T *)0x40012400)
#define ADC_CCR (*(IO *)0x40012708)
#define TIM1   ((TIM_T *)0x40012C00)
#define IWDG   ((IWDG_T *)0x40003000)
#define EXTI   ((EXTI_T *)0x40021800)
#define SYST   ((SYST_T *)0xE000E010)
#define FLASH_ACR (*(IO *)0x40022000)
#define NVIC_ISER (*(IO *)0xE000E100)
#define NVIC_ICER (*(IO *)0xE000E180)
#define NVIC_ICPR (*(IO *)0xE000E280)
#define SCB_VTOR  (*(IO *)0xE000ED08)
#define SCB_AIRCR (*(IO *)0xE000ED0C)
#define SCB_SCR   (*(IO *)0xE000ED10)
#define HSI_TRIM_24M (*(volatile uint16_t *)0x1FFF0100)
#define UID_BYTES ((const volatile uint8_t *)0x1FFF0000)

#define RCC_CR_HSION (1u << 8)
#define RCC_CR_HSIRDY (1u << 10)
#define RCC_I2C (1u << 21)      // APBENR1 / APBRSTR1
#define RCC_TIM1 (1u << 11)     // APBENR2 / APBRSTR2
#define RCC_ADC (1u << 20)      // APBENR2 / APBRSTR2
#define I2C_IRQ 23

#define I2C_CR1_PE 1u
#define I2C_CR1_ACK (1u << 10)
#define I2C_SR1_ADDR (1u << 1)
#define I2C_SR1_STOPF (1u << 4)
#define I2C_SR1_RXNE (1u << 6)
#define I2C_SR1_TXE (1u << 7)
#define I2C_SR1_BERR (1u << 8)
#define I2C_SR1_ARLO (1u << 9)
#define I2C_SR1_AF (1u << 10)
#define I2C_SR1_OVR (1u << 11)
#define I2C_SR2_TRA (1u << 2)

#define ADC_CR_ADEN 1u
#define ADC_CR_ADDIS 2u
#define ADC_CR_ADSTART 4u
#define ADC_CR_ADCAL (1u << 31)
#define ADC_ISR_EOC 4u
#define ADC_CH_VREFINT 9
#define ADC_CH_TEMP 8
#define TS_CAL30 (*(volatile uint16_t *)0x1FFF0114)
#define TS_CAL85 (*(volatile uint16_t *)0x1FFF0118)
