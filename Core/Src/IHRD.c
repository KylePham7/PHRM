/*
 * IHRD.c
 *
 *  Created on: May 20, 2026
 *      Author: phamk2
 */

#include "IHRD.h"
#include <stdlib.h>
#include <stdint.h>

///////////////////////////////////////////
// Initialize irregular rhythm module
///////////////////////////////////////////
void IRHRM_Init(IrregularHRM_t *hrm)
{
    for (int i = 0; i < RR_HISTORY_SIZE; i++)
    {
        hrm->rr_history[i] = 0;
    }

    hrm->index = 0;

    hrm->irregular_count = 0;

    hrm->irregular_detected = 0;

    hrm->rr_average = 0;
}

///////////////////////////////////////////
// Update rhythm analysis
///////////////////////////////////////////
void IRHRM_Update(IrregularHRM_t *hrm,
                  uint32_t rr_interval)
{
    // Store newest RR interval
    hrm->rr_history[hrm->index] = rr_interval;

    // Circular buffer update
    hrm->index++;

    if (hrm->index >= RR_HISTORY_SIZE)
    {
        hrm->index = 0;
    }

    ///////////////////////////////////////
    // Compute average RR interval
    ///////////////////////////////////////
    uint32_t sum = 0;
    uint8_t valid_count = 0;

    for (int i = 0; i < RR_HISTORY_SIZE; i++)
    {
        if (hrm->rr_history[i] > 0)
        {
            sum += hrm->rr_history[i];
            valid_count++;
        }
    }

    if (valid_count == 0)
    {
        return;
    }

    hrm->rr_average = (float)sum / valid_count;

    ///////////////////////////////////////
    // Compare interval to average
    ///////////////////////////////////////
    int32_t difference;

    difference = abs((int32_t)(rr_interval - hrm->rr_average));

    if (difference > IRREGULAR_THRESHOLD_MS)
    {
        hrm->irregular_count++;
    }
    else
    {
        if (hrm->irregular_count > 0)
        {
            hrm->irregular_count--;
        }
    }

    ///////////////////////////////////////
    // Trigger detection
    ///////////////////////////////////////
    if (hrm->irregular_count >= IRREGULAR_COUNT_LIMIT)
    {
        hrm->irregular_detected = 1;
    }
    else
    {
        hrm->irregular_detected = 0;
    }
}

///////////////////////////////////////////
// Return detection state
///////////////////////////////////////////
uint8_t IRHRM_IsIrregular(IrregularHRM_t *hrm)
{
    return hrm->irregular_detected;
}

///////////////////////////////////////////
// Return average RR interval
///////////////////////////////////////////
float IRHRM_GetAverageRR(IrregularHRM_t *hrm)
{
    return hrm->rr_average;
}
