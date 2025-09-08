/**
 ******************************************************************************
 * @file           	: pulse_gen.h
 * @author			: asans
 * @brief          	: 
 ******************************************************************************
 */
#ifndef INC_PULSE_GEN_H_
#define INC_PULSE_GEN_H_


#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"



/* Exported types ------------------------------------------------------------*/

/**
  * @brief  PulseGen Status structures definition
  */
typedef enum {
	PULSEGEN_OK,
	PULSEGEN_ERROR
} PulseGen_StatusTypeDef;

/**
  * @brief  PulseGen State structures definition
  */
typedef enum {
    PULSEGEN_INACTIVE = 0U,
    PULSEGEN_ACTIVE,
	PULSEGEN_ACTIVE_COUNTING
} PulseGen_StateTypeDef;

/**
 * @brief  Pulse SET and RESET enumeration
 */
typedef enum {
	PULSEGEN_RESET = 0U,
	PULSEGEN_SET
} PulseGen_PulseState;

/**
 * @brief  PulseGen init Structure definition
 */
typedef struct {
	uint32_t 			MinDeltaCCR;		/*!< Specifies the minimum CCR increment to use.
												@note	Higher values allow finer frequency adjustments but
														limit the generation of very low frequencies.
														Lower values allow reaching lower frequencies,
														but with lower resolution. 								*/
	uint32_t			MaxPulseFreq;		/*!< Maximum frequency to be reached.
	 	 	 	 	 	 	 	 	 	 	 	 @note	Should be equal to or less than half of the timer's
	 	 	 	 	 	 	 	 	 	 	 	 	 	main clock frequency.									*/

} PulseGen_InitTypeDef;


/**
 * @brief  PulseGen handle Structure definition
 */
typedef struct __PulseGen_HandleTypeDef{
	TIM_HandleTypeDef			*htim;				/*!< Pointer to the timer handle to be used 				*/
	uint32_t 					TimChannel; 		/*!< Timer channel to be used (TIM_CHANNEL_1...) 			*/
	PulseGen_InitTypeDef		Init;       		/*!< Pulse generation required parameters 					*/
	uint32_t 					TimerCntClk;		/*!< Specifies the timer count clock frequency (Hz).		*/
	uint32_t 					MinPulseFreq;   	/*!< Minimum reachable Pulse frequency (Hz) 					*/
	uint32_t 					MaxPulseFreq;   	/*!< Maximum reachable Pulse frequency (Hz) 					*/
	uint32_t 					PulseFreq;			/*!< Specifies the current Pulse frequency (Hz).				*/
	uint32_t 					DeltaCCR;			/*!< Specifies the current CCR.								*/
	uint32_t					TargetPulseCount;	/*!< Specifies the target number of Pulses to generate		*/
	__IO uint32_t				PulseCount;			/*!< Pulse Counter 											*/
	__IO PulseGen_PulseState	PulseLevel;			/*!< Current logical state of the Pulse output pin			*/
	__IO PulseGen_StateTypeDef	State;				/*!< Specifies the current state of the Pulse generation		*/
} PulseGen_HandleTypeDef;


/* Exported constants --------------------------------------------------------*/

/* Exported macro ------------------------------------------------------------*/
#define IS_APB2_TIMER(INSTANCE)  ( ((INSTANCE) == TIM1)  || \
                                   ((INSTANCE) == TIM8)  || \
                                   ((INSTANCE) == TIM9)  || \
                                   ((INSTANCE) == TIM10) || \
                                   ((INSTANCE) == TIM11) )

#define IS_APB1_TIMER(INSTANCE)  ( ((INSTANCE) == TIM2)  || \
                                   ((INSTANCE) == TIM3)  || \
                                   ((INSTANCE) == TIM4)  || \
                                   ((INSTANCE) == TIM5)  || \
                                   ((INSTANCE) == TIM6)  || \
                                   ((INSTANCE) == TIM7)  || \
                                   ((INSTANCE) == TIM12) || \
                                   ((INSTANCE) == TIM13) || \
                                   ((INSTANCE) == TIM14) )

#define HAL_CHANNEL_TO_TIM_CHANNEL(ch)	((ch) == HAL_TIM_ACTIVE_CHANNEL_1 ? TIM_CHANNEL_1 : \
										(ch) == HAL_TIM_ACTIVE_CHANNEL_2 ? TIM_CHANNEL_2 : \
										(ch) == HAL_TIM_ACTIVE_CHANNEL_3 ? TIM_CHANNEL_3 : \
										(ch) == HAL_TIM_ACTIVE_CHANNEL_4 ? TIM_CHANNEL_4 : 0xFFFFFFFFU)


/* Exported functions prototypes ---------------------------------------------*/
PulseGen_StatusTypeDef PulseGen_Init(PulseGen_HandleTypeDef *hPulseGen);
PulseGen_StatusTypeDef PulseGen_DeInit(PulseGen_HandleTypeDef *hPulseGen);	//TODO
PulseGen_StatusTypeDef PulseGen_SetPulseFreq(PulseGen_HandleTypeDef *hPulseGen, uint32_t PulseFreq);
PulseGen_StatusTypeDef PulseGen_Start(PulseGen_HandleTypeDef *hPulseGen);
PulseGen_StatusTypeDef PulseGen_Stop(PulseGen_HandleTypeDef *hPulseGen);
PulseGen_StatusTypeDef PulseGen_GeneratePulses(PulseGen_HandleTypeDef *hPulseGen, uint32_t nPulses);
PulseGen_StatusTypeDef PulseGen_HalfPulseCallback(TIM_HandleTypeDef *htim, PulseGen_HandleTypeDef *hPulseGen);

/* Private defines -----------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif /* INC_PULSE_GEN_H_ */
