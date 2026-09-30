#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static int rpm_value = 0;  // one shared memory location
static SemaphoreHandle_t rpm_mutex;

static void task_generate_rpm(void *arg)
{
    while (1) {
        xSemaphoreTake(rpm_mutex, portMAX_DELAY);
        rpm_value += 1;  // raw counter value
        printf("Task 1: raw RPM counter = %d at address %p\n", rpm_value, (void *)&rpm_value);
        xSemaphoreGive(rpm_mutex);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void task_increment_rpm(void *arg)
{
    while (1) {
        xSemaphoreTake(rpm_mutex, portMAX_DELAY);
        int incremented = rpm_value + 10;  // computed from the same stored value
        printf("Task 2: incremented RPM = %d at address %p\n", incremented, (void *)&rpm_value);
        xSemaphoreGive(rpm_mutex);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    rpm_mutex = xSemaphoreCreateMutex();

    xTaskCreate(task_generate_rpm, "generate_rpm", 2048, NULL, 2, NULL);
    xTaskCreate(task_increment_rpm, "increment_rpm", 2048, NULL, 2, NULL);

    printf("Main task started. Both values are read/written from the same memory address.\n");
}
