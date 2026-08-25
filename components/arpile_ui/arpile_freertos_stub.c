/* Weak implementation of uxTaskGetSystemState to allow linking.
   Provides a minimal stub since the full FreeRTOS task stats may not be enabled.
   This is provided because the arpile_ui component's sysmon application
   calls this function. */

typedef unsigned long UBaseType_t;

UBaseType_t uxTaskGetSystemState(void)
{
    /* Return 0 indicating no task state available. */
    return 0;
}