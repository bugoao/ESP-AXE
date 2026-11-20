#ifndef ADC_H_
#define ADC_H_

#include <stdint.h>

void ADC_init(void);
// Calibrated core voltage in mV, or -1 when unavailable.
int16_t ADC_get_vcore(void);

#endif /* ADC_H_ */
