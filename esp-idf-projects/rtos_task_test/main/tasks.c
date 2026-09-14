#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


// Task Handler 1
void task_1(void *pvParameters)
{
    while (1)
    {
        printf("First\n");

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


// Task Handler 2
void task_2(void *pvParameters)
{
    while (1)
    {
        printf("second\n");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


// Task Handler 3
void task_3(void *pvParameters)
{
    while (1)
    {
        printf("Third\n");

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}