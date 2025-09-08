/*
 * callbacks.c
 *
 *  Created on: Sep 1, 2025
 *      Author: asans
 */

#include "pulse_gen.h"

extern PulseGen_HandleTypeDef hPulseGen1;

/**
 * @brief  Timer output compare callback for Pulse Generator.
 * @param  htim Pointer to the timer handle that triggered the callback.
 */
void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim) {
	PulseGen_HalfPulseCallback(htim, &hPulseGen1);
}
