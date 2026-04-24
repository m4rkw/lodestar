#include "stm32l4xx_hal.h"

extern RTC_HandleTypeDef hrtc_wakeup;

void RTC_WKUP_IRQHandler(void) {
  HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc_wakeup);
}
