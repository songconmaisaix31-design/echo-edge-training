#ifndef MOCE_ADC_CHECKED_H
#define MOCE_ADC_CHECKED_H
/* Task context only, after native chip_init. No implicit initialization.
 * ready: 0 or -3. read: 0, -1 arguments, -2 lock, -3 readiness/calibration,
 * -4 configuration/conversion, -5 unlock. Output unchanged on all errors.
 * SDK-native wait; no finite deadline guarantee. Shared ADC mutex, no new task.
 */
int drv_adc_bsp_ready(void);
int drv_adc_bsp_read(unsigned int channel, int *microvolts);
#endif
