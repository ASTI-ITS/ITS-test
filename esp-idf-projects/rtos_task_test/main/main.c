#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Task handler declarations
void task_1(void *pvParameters);
void task_2(void *pvParameters);
void task_3(void *pvParameters);


void app_main(void)
{
    // Create Task 1
    xTaskCreate(task_1,"Task_1", 2048,NULL,1,NULL);

    // Create Task 2
	xTaskCreate(task_2,"Task_2", 2048,NULL,1,NULL);
	
	// Create Task 3
	xTaskCreate(task_3,"Task_3", 2048,NULL,1,NULL);
}