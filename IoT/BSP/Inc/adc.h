#ifndef __ADC_H
#define __ADC_H

#include "stm32f1xx.h"

void adc_dma_init(uint32_t Dst);
void adc_dma_enable(uint16_t cndtr);

#endif
