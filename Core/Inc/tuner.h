#ifndef TUNER_H
#define TUNER_H

#include "main.h"

void Tuner_Start(DFSDM_Filter_HandleTypeDef *filter, UART_HandleTypeDef *uart);
void Tuner_Poll(void);

#endif
