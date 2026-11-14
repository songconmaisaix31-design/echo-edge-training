#ifndef ECW_ADC_H
#define ECW_ADC_H
#include "ecw_internal.h"
bool ecw_adc_available(void);
bool ecw_adc_port_valid(const Ecw6700Port *port);
int ecw_adc_open(const Ecw6700Port *port);
int ecw_adc_close(const Ecw6700Port *port);
EcwIoResult ecw_adc_sample(unsigned pin, int *microvolts);
BspStatus ecw_adc_descriptor(BspPort id, const Ecw6700Port **port, uint32_t *caps);
#endif
