/*
 * IHRD.h
 *
 *  Created on: May 20, 2026
 *      Author: phamk2
 */

#ifndef INC_IHRD_H_
#define INC_IHRD_H_

#include <stdint.h>
#include <stdlib.h>

#define RR_HISTORY_SIZE            8

// Difference threshold between beats (ms)
// Larger values make detector less sensitive
#define IRREGULAR_THRESHOLD_MS     120

// Minimum number of abnormal intervals
// before triggering irregular flag
#define IRREGULAR_COUNT_LIMIT      3

// ================================
// Struct
// ================================

typedef struct
{
    uint32_t rr_history[RR_HISTORY_SIZE];

    uint8_t index;

    uint8_t irregular_count;

    uint8_t irregular_detected;

    float rr_average;

} IrregularHRM_t;

// ================================
// Function Prototypes
// ================================

void IRHRM_Init(IrregularHRM_t *hrm);

void IRHRM_Update(IrregularHRM_t *hrm,
                  uint32_t rr_interval);

uint8_t IRHRM_IsIrregular(IrregularHRM_t *hrm);

float IRHRM_GetAverageRR(IrregularHRM_t *hrm);



#endif /* INC_IHRD_H_ */
