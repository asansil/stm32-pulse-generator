#ifndef PULSE_GENERATOR_APP_H
#define PULSE_GENERATOR_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the pulse_generator instance against the real STM32F4
   platform and starts the fixed-count test movement. */
void pulse_generator_app_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_APP_H */
