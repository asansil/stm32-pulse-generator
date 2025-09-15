/**
 ******************************************************************************
 * @file           	: pulse_gen.c
 * @author			: asans
 * @brief          	: 
 ******************************************************************************
 */


/* Private includes ----------------------------------------------------------*/
#include "pulse_gen.h"

/* Private typedef -----------------------------------------------------------*/

/* Private define ------------------------------------------------------------*/

/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
static uint32_t PulseGen_GetTimerMainClk(TIM_HandleTypeDef *htim);
static PulseGen_StatusTypeDef PulseGen_GetTimerPsc(PulseGen_HandleTypeDef *hPulseGen, uint32_t TimerClk, uint16_t *psc);
static inline uint32_t PulseGen_DeltaCCRToFreq(PulseGen_HandleTypeDef *hPulseGen, uint32_t DeltaCCR);
static inline uint32_t PulseGen_FreqToDeltaCCR(PulseGen_HandleTypeDef *hPulseGen, uint32_t PulseFreq);
static void PulseGen_UpdateTimCCR(PulseGen_HandleTypeDef *hPulseGen);

/* Code ----------------------------------------------------------------------*/

/**
 * @brief  Initializes the Pulse Generator handle and its associated timer. *
 * @param  hPulseGen Pointer to the PulseGen handle structure.
 * @retval PULSEGEN_OK if initialization was successful.
 * @retval PulseGen_StatusTypeDef indicating success or error.
 *
 * @note   The timer prescaler is configured automatically according to the Init requirements.
 */
PulseGen_StatusTypeDef PulseGen_Init(PulseGen_HandleTypeDef *hPulseGen) {
	/* Check the PulseGen handle allocation */
	if (hPulseGen == NULL) {
		return PULSEGEN_ERROR;
	}

	/* Get timer main clock */
	uint32_t tim_clk = PulseGen_GetTimerMainClk(hPulseGen->htim);

	/* Set the timer prescaler*/
	uint16_t tim_psc;
	if (PulseGen_GetTimerPsc(hPulseGen, tim_clk, &tim_psc) != PULSEGEN_OK) {
	    return PULSEGEN_ERROR;
	}

	// TODO: Comprobar si el prescaler ya es ese para no editarlo de mas
	/* Check if prescaler was already set by another PulseGen */
	if(hPulseGen->htim->Instance->PSC != tim_psc){
		__HAL_TIM_SET_PRESCALER(hPulseGen->htim, tim_psc);

		/* Generate an update event to reload the Prescaler value immediately */
		hPulseGen->htim->Instance->EGR = TIM_EGR_UG;
	}

	/* Initialize the timer count clock frequency */
	hPulseGen->TimerCntClk = tim_clk/(tim_psc+1);

	/* Initialize the maximum reachable Pulse frequency */
	hPulseGen->MaxPulseFreq = PulseGen_DeltaCCRToFreq(hPulseGen, hPulseGen->Init.MinDeltaCCR);

	/* Initialize the minimum reachable Pulse frequency */
	if(IS_TIM_32B_COUNTER_INSTANCE(hPulseGen->htim->Instance)){
		/* It's a 32 bit counter*/
		hPulseGen->MinPulseFreq = PulseGen_DeltaCCRToFreq(hPulseGen, 0xFFFFFFFF);
	} else{
		/* It's a 16 bit counter*/
		hPulseGen->MinPulseFreq = PulseGen_DeltaCCRToFreq(hPulseGen, 0xFFFF);
	}

	/* Initialize the Pulse Generator state */
	hPulseGen->State = PULSEGEN_INACTIVE;

	return PULSEGEN_OK;
}

// TODO
PulseGen_StatusTypeDef PulseGen_DeInit(PulseGen_HandleTypeDef *hPulseGen) {
	return PULSEGEN_OK;
}

/**
 * @brief  Sets the desired Pulse frequency for the Pulse Generator.
 * 		   It adjusts the internal CCR increment to achieve the requested frequency.
 * @param  hPulseGen Pointer to the PulseGen handle structure.
 * @param  PulseFreq Desired Pulse frequency in Hz.
 * @retval PulseGen_StatusTypeDef indicating success or error.
 */
PulseGen_StatusTypeDef PulseGen_SetPulseFreq(PulseGen_HandleTypeDef *hPulseGen, uint32_t PulseFreq) {
	if(hPulseGen == NULL || !PulseFreq){
		return PULSEGEN_ERROR;
	}

	/* Check if PulseFreq is reachable*/
	if(PulseFreq < hPulseGen->MinPulseFreq || PulseFreq > hPulseGen->MaxPulseFreq){
		return PULSEGEN_ERROR;
	}

	/* Set the CCR increment */
	hPulseGen->DeltaCCR = PulseGen_FreqToDeltaCCR(hPulseGen, PulseFreq);

	/* Set the real reached Pulse frequency  */
	hPulseGen->PulseFreq = PulseGen_DeltaCCRToFreq(hPulseGen, hPulseGen->DeltaCCR);

	return PULSEGEN_OK;
}

/**
 * @brief  Starts the Pulse Generator.
 * @param  hPulseGen Pointer to the PulseGen handle structure.
 * @retval PulseGen_StatusTypeDef indicating success or error.
 */
PulseGen_StatusTypeDef PulseGen_Start(PulseGen_HandleTypeDef *hPulseGen) {
	/* Init the Pulse logical level */
	hPulseGen->PulseLevel = PULSEGEN_SET;

	/* Reset Pulse counter */
	hPulseGen->PulseCount = 0U;

	/* Init timer CCR value*/
	uint32_t cnt_content = __HAL_TIM_GET_COUNTER(hPulseGen->htim);
	__HAL_TIM_SET_COMPARE(hPulseGen->htim, hPulseGen->TimChannel, cnt_content + hPulseGen->DeltaCCR);

	/* Start timer in OC */
	if (HAL_TIM_OC_Start_IT(hPulseGen->htim, hPulseGen->TimChannel) != HAL_OK) {
		return PULSEGEN_ERROR;
	}

	/* Update the Pulse generation state*/
	if(hPulseGen->State == PULSEGEN_INACTIVE){
		hPulseGen->State = PULSEGEN_ACTIVE;
	}

	return PULSEGEN_OK;
}

/**
 * @brief  Stops the Pulse Generator.
 * @param  hPulseGen Pointer to the PulseGen handle structure.
 * @retval PulseGen_StatusTypeDef indicating success or error.
 */
PulseGen_StatusTypeDef PulseGen_Stop(PulseGen_HandleTypeDef *hPulseGen) {
	/* Stop timer in OC */
	if (HAL_TIM_OC_Stop_IT(hPulseGen->htim, hPulseGen->TimChannel) != HAL_OK) {
		return PULSEGEN_ERROR;
	}

	/* Update the Pulse generation state*/
	hPulseGen->State = PULSEGEN_INACTIVE;

	return PULSEGEN_OK;
}

/**
 * @brief  Begins the generation of a specified number of Pulses.
 *         Executes in place of PulseGen_Start for controlled Pulse sequences.
 * @param  hPulseGen Pointer to the PulseGen handle structure.
 * @param  nPulses Number of Pulses to generate.
 * @retval PulseGen_StatusTypeDef indicating success or error.
 */
PulseGen_StatusTypeDef PulseGen_GeneratePulses(PulseGen_HandleTypeDef *hPulseGen, uint32_t nPulses) {
	/* Set target Pulses count */
	hPulseGen->TargetPulseCount = nPulses;

	/* Update the Pulse generation state*/
	hPulseGen->State = PULSEGEN_ACTIVE_COUNTING;

	/* Start Pulse generation */
	PulseGen_Start(hPulseGen);

	return PULSEGEN_OK;
}

/**
  * @brief  Get the timer main input clock frequency.
  * @param  htim TIM handle.
  * @retval Timer main clock frequency in Hz.
  */
static uint32_t PulseGen_GetTimerMainClk(TIM_HandleTypeDef *htim) {
	uint32_t tim_clk = 0;
	uint32_t pclk = 0;

	/* Check if timer is clocked from APB1 */
	if (IS_APB1_TIMER(htim->Instance)) {
		/* Get APB1 peripheral clock frequency */
		pclk = HAL_RCC_GetPCLK1Freq();

		/* If APB1 prescaler is different from 1, timer clock is doubled */
		if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_HCLK_DIV1) {
			tim_clk = pclk * 2U;
		} else {
			tim_clk = pclk;
		}
	} else {
		/* Timer is clocked from APB2 */

		/* Get APB2 peripheral clock frequency */
		pclk = HAL_RCC_GetPCLK2Freq();

		/* If APB2 prescaler is different from 1, timer clock is doubled */
		if ((RCC->CFGR & RCC_CFGR_PPRE2) != RCC_HCLK_DIV1) {
			tim_clk = pclk * 2U;
		} else {
			tim_clk = pclk;
		}
	}

	/* Return timer clock frequency */
	return tim_clk;
}


/**
 * @brief  Calculate the timer prescaler required for the PulseGen driver
 * @param  hPulseGen: Pointer to a PulseGen handle containing configuration parameters
 * @param  TimerClk: Timer input clock frequency in Hz
 * @param  psc: Pointer to store the computed prescaler value (PSC)
 * @retval PulseGen status:
 *         - PULSEGEN_OK if the prescaler was calculated successfully
 *         - PULSEGEN_ERROR if input parameters are invalid or the prescaler exceeds 16 bits
 *
 * @note   To avoid using <math.h>, the formula (a + b - 1) / b is used
 *         instead of ceil(a/b), saving both code size and CPU cycles.
 *         This ensures proper rounding up when calculating the prescaler.
 */
static PulseGen_StatusTypeDef PulseGen_GetTimerPsc(PulseGen_HandleTypeDef *hPulseGen, uint32_t TimerClk, uint16_t *psc) {
	if (!hPulseGen || !psc)
		return PULSEGEN_ERROR;

	uint32_t b = 2U * hPulseGen->Init.MaxPulseFreq * hPulseGen->Init.MinDeltaCCR;
	uint32_t psc32 = (TimerClk + b - 1U) / b - 1U;

	if (psc32 > 0xFFFFU) {
		*psc = 0xFFFFU;
		return PULSEGEN_ERROR;
	}

	*psc = (uint16_t) psc32;
	return PULSEGEN_OK;
}


/**
  * @brief  Convert a timer DeltaCCR value to Pulse frequency
  * @param  hPulseGen: Pointer to a PulseGen handle containing configuration parameters
  * @param  DeltaCCR: Timer compare delta value (ticks between toggle events)
  * @retval Pulse frequency in Hz
  *
  * @note   The timer interrupt is generated every half Pulse period, therefore the resulting frequency
  * 		is half the timer toggle rate
  */
static inline uint32_t PulseGen_DeltaCCRToFreq(PulseGen_HandleTypeDef *hPulseGen, uint32_t DeltaCCR){
	return (DeltaCCR == 0U) ? 0U : (hPulseGen->TimerCntClk/(2U*DeltaCCR));
}

/**
  * @brief  Convert a Pulse frequency to the corresponding timer DeltaCCR value
  * @param  hPulseGen: Pointer to a PulseGen handle containing configuration parameters
  * @param  PulseFreq: Desired Pulse frequency in Hz
  * @retval DeltaCCR value (timer ticks between toggle events)
  */
static inline uint32_t PulseGen_FreqToDeltaCCR(PulseGen_HandleTypeDef *hPulseGen, uint32_t PulseFreq){
	return (PulseFreq == 0U) ? 0U : (hPulseGen->TimerCntClk/(2U*PulseFreq));
}

/**
 * @brief  Updates the timer compare register (CCR) to schedule the next Pulse.
 * @param  hPulseGen Pointer to the PulseGen handle structure.
 */
static void PulseGen_UpdateTimCCR(PulseGen_HandleTypeDef *hPulseGen){
	/* Get the current timer CCR value */
	uint32_t ccr_content = HAL_TIM_ReadCapturedValue(hPulseGen->htim, hPulseGen->TimChannel);

	/* Update the new timer CCR value */
	__HAL_TIM_SET_COMPARE(hPulseGen->htim, hPulseGen->TimChannel, ccr_content + hPulseGen->DeltaCCR);
}

/**
 * @brief  Handles a half-Pulse update for the Pulse Generator in a timer callback.
 * @param  htim Pointer to the timer handle triggering the callback.
 * @param  hPulseGen Pointer to the PulseGen handle structure.
 * @retval PulseGen_StatusTypeDef indicating whether the timer matched and the update was applied.
 */
PulseGen_StatusTypeDef PulseGen_HalfPulseCallback(TIM_HandleTypeDef *htim, PulseGen_HandleTypeDef *hPulseGen){
	/* Check if the timer triggering the ISR corresponds to the one used by PulseGen handle */
	if (htim->Instance == hPulseGen->htim->Instance &&
			HAL_CHANNEL_TO_TIM_CHANNEL(htim->Channel) == hPulseGen->TimChannel) {
		/* Update CCR */
		PulseGen_UpdateTimCCR(hPulseGen);

		/* Toggle Pulse logical level */
		hPulseGen->PulseLevel ^= 1U;

		/* Increment Pulse counter only on rising edge */
		if (hPulseGen->PulseLevel == PULSEGEN_SET) {
		    hPulseGen->PulseCount++;

		    /* Stop counter if target Pulses reached (Only in generate Pulses mode) */
		    if(hPulseGen->State == PULSEGEN_ACTIVE_COUNTING && hPulseGen->PulseCount >= hPulseGen->TargetPulseCount){
		    	PulseGen_Stop(hPulseGen);
		    }
		}

		/* Timer match */
		return PULSEGEN_OK;
	}

	/* No timer match confirmed */
	return PULSEGEN_ERROR;
}
