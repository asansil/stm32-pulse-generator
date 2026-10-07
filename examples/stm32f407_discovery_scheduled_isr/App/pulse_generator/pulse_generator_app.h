#ifndef PULSE_GENERATOR_APP_H
#define PULSE_GENERATOR_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the pulse_generator instance against the real STM32F4
   platform and prepares it for scheduled mode. */
void pulse_generator_app_init(void);

/* Runs the demo: steps through the phases and keeps the event queue topped
   up. Non-blocking; call it on every pass of the main loop. */
void pulse_generator_app_process(void);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_APP_H */
