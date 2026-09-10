#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"


// ==================================================
// Access Queue created in main.c
// ==================================================

extern QueueHandle_t task_queue;


// ==================================================
// Shared total from Task 2
// ==================================================

int task2_total = 0;


void task_1(void *pvParameters)
{
    int count = 1;

    while (1)
    {
        // Display current count
        printf("Counter: %d\n", count);


        // Send current number to Task 2
        xQueueSend(task_queue, &count, 0);


        // Increase counter
        count++;


        // Reset after 9
        if (count > 9)
        {
            count = 1;
        }


        // Task 1 runs every 1000 ms
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}



void task_2(void *pvParameters)
{
    int received_value;
    int result;

    while (1)
    {
        // Try to receive one value from the queue
        if (xQueueReceive(task_queue, &received_value, 0) == pdPASS)
        {
            // Add 10 to received value
            result = received_value + 10;

            // Add result to accumulated total
            task2_total += result;

            // Display only the current addition
            printf(
                "Adder: Received %d + 10 = %d\n",
                received_value,
                result
            );
        }
        else
        {
            printf("Task 2: No value available in Queue\n");
        }

        // Task 2 runs every 4000 ms
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}


void task_3(void *pvParameters)
{
    while (1)
    {
        printf("Compiler: %d\n",task2_total);

        // Task 3 runs every 20000 ms
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}