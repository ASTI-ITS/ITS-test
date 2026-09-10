#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"


// Task function declarations
void task_1(void *pvParameters);
void task_2(void *pvParameters);
void task_3(void *pvParameters);


// Queue handle
QueueHandle_t task_queue;


void app_main(void)
{
    // Create a queue that can hold 5 integers
    task_queue = xQueueCreate(5, sizeof(int));

    // Check if queue was created successfully
    if (task_queue == NULL)
    {
        printf("ERROR: Queue creation failed!\n");
        return;
    }

    printf("Queue created successfully.\n");

        // Create Task 1
    xTaskCreate(task_1,"Task_1", 2048,NULL,1,NULL);

    // Create Task 2
	xTaskCreate(task_2,"Task_2", 2048,NULL,1,NULL);
	
	// Create Task 3
	xTaskCreate(task_3,"Task_3", 2048,NULL,1,NULL);
}