/*
 * callbacks.c
 *
 *  Created on: Sep 1, 2025
 *      Author: asans
 */

#include "pulse_gen.h"

extern PulseGen_HandleTypeDef *PulseGens[4];

/**
 * @brief  Timer output compare callback for Pulse Generator.
 * 		   Iterates through all PulseGen instances and calls the half-pulse update
 *         for the timer that triggered the interrupt.
 * @param  htim Pointer to the timer handle that triggered the callback.
 */
void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim) {

	for (size_t i = 0; i < sizeof(PulseGens)/sizeof(PulseGens[0]); i++) {
		if(PulseGen_HalfPulseCallback(htim, PulseGens[i]) == PULSEGEN_OK){
			/* Pulse generator matched */
			break;
		}
	}
}
