/* Weak fallback for uxTaskGetSystemState: only links if FreeRTOS task-stats
 * are disabled in sdkconfig; a real implementation always overrides this. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

__attribute__((weak)) UBaseType_t uxTaskGetSystemState(
    TaskStatus_t *const pxTaskStatusArray,
    const UBaseType_t uxArraySize,
    configRUN_TIME_COUNTER_TYPE *const pulTotalRunTime)
{
    (void)pxTaskStatusArray;
    (void)pulTotalRunTime;
    return 0;
}