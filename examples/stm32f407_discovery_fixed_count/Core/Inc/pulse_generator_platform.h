#ifndef PULSE_GENERATOR_PLATFORM_H
#define PULSE_GENERATOR_PLATFORM_H

#include "pulse_generator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* pulse_generator_platform_t implementation for TIM4 (Output Compare
   Toggle, channel 1, PD12) on the STM32F407 Discovery. ctx is &htim4. */
extern const pulse_generator_platform_t g_stm32f4_platform;

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_PLATFORM_H */
