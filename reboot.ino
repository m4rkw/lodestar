
void reboot() {
  DEBUG_FUNCTION_CALL();

  //reset GPS
  gps_off();
  //emergency power off GSM
  gsm_off(1);

#if defined(_SAM3XA_)
  //disable USB to allow reboot
  //serial monitor on the PC won't work anymore if you don't close it before reset completes
  //otherwise, close the serial monitor, detach the USB cable and connect it again

  // debug_port.end() does nothing, manually disable USB
  UDD_Detach(); // detach from Host

  cpu_irq_disable();
  rstc_start_software_reset(RSTC);
  for (;;)
  {
    // If we do not keep the watchdog happy and it times out during this wait,
    // the reset reason will be wrong when the board starts the next time around.
    WDT_Restart(WDT);
  }
#else
  __disable_irq();
  NVIC_SystemReset();
#endif
}

void modem_power_cycle() {
  DEBUG_FUNCTION_CALL();

  extern byte network_ready;
  network_ready = 0;

  gsm_off(1);
  delay(2000);
  gsm_on();

#if MODEM_BG96
  // GNSS engine state is cleared on hardware power cycle — re-enable it,
  // otherwise no NMEA will flow after recovery and collect_gps_data() will
  // time out on every subsequent attempt.
  gsm_port.print("AT+QGPSCFG=\"outport\",\"uartnmea\"\r");
  gsm_wait_for_reply(1,0);
  gsm_port.print("AT+QGPS=1\r");
  gsm_wait_for_reply(1,0);
#endif

  gsm_config();
}

void usb_console_disable() {
#if defined(_SAM3XA_)
  cpu_irq_disable();
  
  // debug_port.end() does nothing, manually disable USB serial console
  UDD_Detach(); // detach from Host
  // de-init procedure (reverses UDD_Init)
  otg_freeze_clock();
  otg_disable_pad();
  otg_disable();
  pmc_disable_udpck();
  pmc_disable_upll_clock();
  pmc_disable_periph_clk(ID_UOTGHS);
  NVIC_DisableIRQ((IRQn_Type) ID_UOTGHS);
  NVIC_ClearPendingIRQ((IRQn_Type) ID_UOTGHS);

  cpu_irq_enable();
#else
  usbd_interface_deinit();
#endif
}

void usb_console_restore() {
#if defined(_SAM3XA_)
  if (!Is_otg_enabled()) {
    // re-initialize USB
    UDD_Init();
    UDD_Attach();
  }
#else
  usbd_interface_init();
#endif
}

// override for lower power consumption (wait for interrupt)
extern "C" void yield(void) {
#if defined(INC_FREERTOS_H)
#if ((INCLUDE_xTaskGetSchedulerState == 1) || (configUSE_TIMERS == 1))
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
#endif
  {
    vTaskDelay(1); // not using taskYIELD() because lower priority tasks would not run otherwise!
    return;
  }
#endif
#if defined(_SAM3XA_)
  pmc_enable_sleepmode(0);
#else
  __WFI();
#endif
}

void cpu_slow_down() {
#if defined(_SAM3XA_)
  SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;
  pmc_mck_set_prescaler(PMC_MCKR_PRES_CLK_64);
  SystemCoreClockUpdate();
  SysTick_Config(SystemCoreClock / 1000);
#else
  // STM32L4: switch SYSCLK from 80MHz PLL to MSI 1MHz
  RCC_ClkInitTypeDef clk = {};
  clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK
                 | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0);

  // drop MSI to 1MHz
  RCC_OscInitTypeDef osc = {};
  osc.OscillatorType = RCC_OSCILLATORTYPE_MSI;
  osc.MSIState = RCC_MSI_ON;
  osc.MSIClockRange = RCC_MSIRANGE_4;  // 1 MHz
  osc.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
  osc.PLL.PLLState = RCC_PLL_OFF;
  HAL_RCC_OscConfig(&osc);

  // disable unused oscillators
  RCC_OscInitTypeDef off = {};
  off.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSI48;
  off.HSIState = RCC_HSI_OFF;
  off.HSI48State = RCC_HSI48_OFF;
  off.PLL.PLLState = RCC_PLL_NONE;
  HAL_RCC_OscConfig(&off);

  // lower voltage regulator for reduced clock
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE2);

  SystemCoreClockUpdate();
  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq() / 1000);
#endif
}

void cpu_full_speed() {
#if defined(_SAM3XA_)
  SystemInit();
  SysTick_Config(SystemCoreClock / 1000);
#else
  // STM32L4: restore full 80MHz clock via board's SystemClock_Config
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);
  SystemClock_Config();
#endif
}

// -- Independent Watchdog (IWDG) ----------------------------------------------
// Hardware watchdog clocked by LSI (32kHz), independent of system clock.
// Max timeout at prescaler /256: 4095/(32000/256) ≈ 32.77s.
// Resets the MCU if not refreshed within the timeout — covers spin loops,
// peripheral deadlocks, and any unbounded wait that the firmware can't detect.
//
// IWDG_STOP option byte (FLASH_OPTR bit 17) must be 0 to freeze IWDG in STOP2
// sleep, otherwise the chip would reset every ~32s during long sleeps.
// Factory default on STM32L476 is 0 (frozen). We verify at init.

static IWDG_HandleTypeDef hiwdg;
static byte watchdog_enabled = 0;

void watchdog_init() {
  // verify IWDG_STOP option byte is set to freeze IWDG during Stop mode,
  // otherwise enabling the watchdog would bootloop the device during sleep
  uint32_t optr = FLASH->OPTR;
  if ((optr & FLASH_OPTR_IWDG_STOP) != 0) {
    debug_print(F("WARNING: IWDG_STOP option byte would reset during sleep, watchdog disabled"));
    return;
  }

  hiwdg.Instance       = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
  hiwdg.Init.Reload    = 4095;   // ~32s at LSI/256
  hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;
  if (HAL_IWDG_Init(&hiwdg) == HAL_OK) {
    watchdog_enabled = 1;
    debug_print(F("Watchdog enabled"));
  } else {
    debug_print(F("WARNING: HAL_IWDG_Init failed"));
  }
}

void watchdog_kick() {
  if (watchdog_enabled) {
    HAL_IWDG_Refresh(&hiwdg);
  }
}

