#ifndef PULSE_GENERATOR_APP_H
#define PULSE_GENERATOR_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the pulse_generator instance against the real STM32F4
   platform and starts the first fixed-count test movement. */
void pulse_generator_app_init(void);

/* Runs the test sequence: once a movement has ended and the pause after it
   has elapsed, starts the next one. Call it from the main loop; the event
   callback cannot start a movement itself. */
void pulse_generator_app_process(void);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_APP_H */
