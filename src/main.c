/***************************************************************************//**
 * @file
 * @brief Demo example for SLSTK3301A
 *******************************************************************************
 * # License
 * <b>Copyright 2018 Silicon Laboratories Inc. www.silabs.com
 * Modified work Copyright 2020 Connected Development, Inc. </b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

#include <stdint.h>
#include <stdlib.h>

#include "em_device.h"
#include "em_chip.h"
#include "em_cryotimer.h"
#include "em_cmu.h"
#include "em_emu.h"
#include "em_gpio.h"
#include "em_wdog.h"
#include "em_timer.h"

#include "bsp.h"
#include "bspconfig.h"
#include "segmentlcd.h"

#include "app_bumode.h"
#include "app_csen.h"
#include "app_rtcc.h"
#include "app_sensor.h"
#include "TSD305.h"

// Global state variables
static uint8_t currentApp = 0;
static bool cryoPeriod = false, lcTriggered = false;
static bool longBeep = false;

// App-specific variables
int16_t appHallField = 0;
bool appRhtCelsius = true;
float appRhtTempData = 0;
float lockedTemp = 0; // stableTemperature to display
float ambTemp = 0;
float objTemp = 0;
uint8_t longCount = 0;  // For long beep timing
uint32_t appRhtRhData = 0;
CSEN_Event_t appCsenData = CSEN_EVENT_DEFAULT;
bool appBuChargeEn = false, appBuDisCharge = false;
uint8_t appBuSelect = 3;
uint16_t appBuVoltage = 0;
uint16_t appBuAvddVoltage = 315;
uint32_t appBuRtccResetVal = 0, appBuRtccResetTs = 0, appBuBodCnt = 0, appBuBuTime = 0, appBuBodTime = 0;
uint16_t appLcCounter = 0;
uint32_t appLcRng[4];
bool btn0Pressed = false;
bool btn1Pressed = false;

uint16_t stableTempCount = 0;
uint16_t tempDisplayedCount = 0;
bool displayWait = true;
bool displayTemp = false;
bool displayMeasure = false;
uint32_t historicalTemps[4] = {0,0,0,0};

#define APP_COUNT       2
#define TEMPERATURE_DEMO       0
#define TEMPERATURE_DETAILS    1
#define OUTPUT_FREQ_Hz (6000UL)

#define WRIST_COMP 13  // Offset from wrist temp to internal temp
#define OBJ_DETECT_THRESHOLD 28.5  // Degrees c to detect an object
#define FEVER_TEMP 100.4  // temperature needed to trigger a fever notification
#define NUM_READINGS_NEEDED 4
#define TEMP_VARIATION_ALLOWED 1 // NUM_READINGS_NEEDED
                                  // consecutive readings must be in this range
                                  // to count ('F)

/**************************************************************************//**
 * @brief Setup push buttons and GPIO.
 *****************************************************************************/
static void gpioSetup(void)
{
  CMU_ClockEnable(cmuClock_HFPER, true);
  CMU_ClockEnable(cmuClock_GPIO, true);

  // Initialization of GPIO interrupt for BTN0 and BTN1
  GPIO_PinModeSet(BSP_GPIO_PB0_PORT, BSP_GPIO_PB0_PIN, gpioModeInput, 1);
  GPIO_IntConfig(BSP_GPIO_PB0_PORT, BSP_GPIO_PB0_PIN, false, true, true);
  GPIO_PinModeSet(BSP_GPIO_PB1_PORT, BSP_GPIO_PB1_PIN, gpioModeInput, 1);
  GPIO_IntConfig(BSP_GPIO_PB1_PORT, BSP_GPIO_PB1_PIN, false, true, true);

  GPIO_IntClear(0x1 << BSP_GPIO_PB0_PIN);
  GPIO_IntClear(0x1 << BSP_GPIO_PB1_PIN);
  NVIC_ClearPendingIRQ(GPIO_ODD_IRQn);
  NVIC_EnableIRQ(GPIO_ODD_IRQn);

  // Enable power to the I2C sensors
  GPIO_PinModeSet(gpioPortC, 12, gpioModePushPull, 1);
}

/******************************************************************************
 * @brief Set the green led on/ off
 *****************************************************************************/

static void setRedLed(bool on)
{
	if (on == true)
	{
		GPIO_PinModeSet(gpioPortC, 0, gpioModePushPull, 1);
	}
	else
	{
		GPIO_PinModeSet(gpioPortC, 0, gpioModePushPull, 0);
	}
}

/******************************************************************************
 * @brief Set the red led on/ off
 *****************************************************************************/

static void setGreenLed(bool on)
{
	if (on == true)
	{
		GPIO_PinModeSet(gpioPortC, 8, gpioModePushPull, 1);
	}
	else
	{
		GPIO_PinModeSet(gpioPortC, 8, gpioModePushPull, 0);
	}
}

/******************************************************************************
 * @brief Setup CRYOTIMER to generate an interrupt every second.
 *****************************************************************************/
static void cryotimerSetup(void)
{
  static CRYOTIMER_Init_TypeDef cryotimerInit = CRYOTIMER_INIT_DEFAULT;

  CMU_ClockEnable(cmuClock_CRYOTIMER, true);

  // Use LFXO as cryotimer clock source
  cryotimerInit.osc = cryotimerOscLFXO;

  // Setup CRYOTIMER period to 1 second
  cryotimerInit.presc = cryotimerPresc_128;
  cryotimerInit.period = cryotimerPeriod_64;

  // Enable CRYOTIMER period interrupt
  CRYOTIMER_IntClear(0xFFFFFFFFUL);
  CRYOTIMER_IntEnable(CRYOTIMER_IEN_PERIOD);
  NVIC_EnableIRQ(CRYOTIMER_IRQn);

  // Initialize and enable CRYOTIMER
  CRYOTIMER_Init(&cryotimerInit);
}

/**************************************************************************//**
 * @brief LCD display for different application.
 *****************************************************************************/
void drawScreen(uint8_t app)
{
  char str[13];

  switch (app) {
    // Show raw temperature as it comes in (for debug purposes)
    case TEMPERATURE_DETAILS:
       if (appRhtCelsius)
       {
          sprintf(str, "T %.2f", appRhtTempData);  // Unmodified temperature, C
       }
       else
       {
          sprintf(str, "T %.2f", ((appRhtTempData * 9) / 5 + 32));  // Unmodified temperature, F
       }
       str[8] = '\0';                            // Truncate to display size.

       SegmentLCD_Write(str);

       SegmentLCD_Symbol(LCD_SYMBOL_S13, true);
       SegmentLCD_Symbol(LCD_SYMBOL_S14, false);
       SegmentLCD_Symbol(LCD_SYMBOL_C18, 1);
      break;

    // The "production" demo -
    case TEMPERATURE_DEMO:
      sprintf(str, "T %.2f", ((lockedTemp * 9) / 5 + 32 + WRIST_COMP));  // add comp for wrist
      str[8] = '\0';                            // Truncate to display size.
      if (displayWait)
         SegmentLCD_Write("READY");
      else if (displayMeasure)
         SegmentLCD_Write("MEASURE");
      else
         SegmentLCD_Write(str);

      SegmentLCD_Symbol(LCD_SYMBOL_S13, true);
      SegmentLCD_Symbol(LCD_SYMBOL_S14, false);
      SegmentLCD_Symbol(LCD_SYMBOL_C18, 1);

      if (longCount > 0)
      {
         longCount--;  // Count it down until we get to 0
      }
      else
      {
         CMU_ClockEnable(cmuClock_TIMER0, false); // Turn off the beep
      }
      break;

    default:
      break;
  }
}

/**************************************************************************//**
 * @brief  Set up PWM for the piezo buzzer
 *****************************************************************************/

void setupPWM(void)
{
   // Set pin to push-pull output
   GPIO_PinModeSet(gpioPortC, 1, gpioModePushPull, 1);

   // Set Timer0 to toggle PC1
   CMU_ClockEnable(cmuClock_TIMER0, true);
   uint32_t hfperFreq = CMU_ClockFreqGet(cmuClock_HFPER);
   TIMER_TopBufSet(TIMER0, (hfperFreq / (2 * OUTPUT_FREQ_Hz)));
   TIMER_Init_TypeDef init = TIMER_INIT_DEFAULT;
   TIMER_Init(TIMER0, &init);

   TIMER_InitCC_TypeDef initCh = TIMER_INITCC_DEFAULT;
   initCh.cofoa = timerOutputActionToggle;
   initCh.mode = timerCCModeCompare;
   TIMER_InitCC(TIMER0, 2, &initCh);

   TIMER0->ROUTEPEN |= TIMER_ROUTEPEN_CC2PEN;
   TIMER0->ROUTELOC0 |= TIMER_ROUTELOC0_CC2LOC_LOC3;
   CMU_ClockEnable(cmuClock_TIMER0, false);
}

/**************************************************************************//**
 * @brief  Main function of EFM32TG11 demo example.
 *****************************************************************************/
int main(void)
{
  CMU_LFXOInit_TypeDef lfxoInit = CMU_LFXOINIT_DEFAULT;
  EMU_DCDCInit_TypeDef dcdcInit = EMU_DCDCINIT_STK_DEFAULT;

  // Chip errata
  CHIP_Init();

  // Setup the LFXO. It is used by RTCC and retention registers.
  lfxoInit.ctune = 0x46;
  CMU_LFXOInit(&lfxoInit);
  CMU_OscillatorEnable(cmuOsc_LFXO, true, true);
  // Use LFXO as source for LFECLK.
  CMU_ClockSelectSet(cmuClock_LFE, cmuSelect_LFXO);
  // Enable access to LE peripheral registers.
  CMU_ClockEnable(cmuClock_CORELE, true);
  appRhtCelsius = false;

  // Setup backup mode.
  // Restore values stored in retention registers if we are waking up from a backup event.
  //buSetup(&appBuRtccResetVal, &appBuRtccResetTs, &appBuBodCnt, &appBuBuTime, &appBuBodTime, &currentApp);

  // Setup RTCC for timekeeping in both active and backup modes.
  setupRTCC(appBuRtccResetVal);

  // Init DCDC regulator with kit specific parameters
  EMU_DCDCInit(&dcdcInit);

  // Setup push buttons and GPIO pins
  gpioSetup();

  // Initialize LED driver
  BSP_LedsInit();

  // Initialize the Red and Green LEDs
  setGreenLed (true);
  setRedLed (true);

  // Enable LCD without voltage boost, use LFRCO as LCD clock source
  SegmentLCD_Init(false);

  // Setup CRYOTIMER to give an interrupt once per second
  cryotimerSetup();

  // Setup humidity/temperature sensor, hall effect sensor and inductive proximity (LC) sensor
  sensorSetup();

  setupPWM();  // for the piezo buzzer output

  currentApp = TEMPERATURE_DEMO;

  // Watchdog Initialize settings

  WDOG_Init_TypeDef wdogInit = WDOG_INIT_DEFAULT;

  wdogInit.debugRun = true;

  wdogInit.em3Run = true;

  wdogInit.clkSel = wdogClkSelULFRCO;

  wdogInit.perSel = wdogPeriod_16k; // 2049 clock cycles of a 1kHz clock  ~2 seconds period

  // Initializing watchdog with chosen settings
  WDOGn_Init(DEFAULT_WDOG, &wdogInit);

  // Infinite loop
  while (1) {
    // Wait event at EM2
    EMU_EnterEM2(false);

    // Get the latest CSEN status
    appCsenData = csenGetEvent();

    // Actions that should only be performed once per second (controlled by CRYOTIMER)
    if (cryoPeriod) {
      WDOG_Feed();
      cryoPeriod = false;

      // Read temperature data
      read_temperature_and_object_temperature(&ambTemp, &objTemp);
      appRhtTempData = objTemp;

      historicalTemps[3] = historicalTemps[2];
      historicalTemps[2] = historicalTemps[1];
      historicalTemps[1] = historicalTemps[0];
      historicalTemps[0] = objTemp;

      // See if an object is close to the sensor (temperature based)
      if (objTemp > OBJ_DETECT_THRESHOLD)  // in celcius - ~90 f
      {
         BSP_LedToggle(0);
         if (stableTempCount < NUM_READINGS_NEEDED)
         {
        	setGreenLed (false);
        	setRedLed (false);

            // Waiting for a stable measurement
            // Do a couple of quick beeps to show progress
            CMU_ClockEnable(cmuClock_TIMER0, true);
            Delay(20);
            CMU_ClockEnable(cmuClock_TIMER0, false);
            Delay(20);
            displayTemp = false;
            displayMeasure = true;
            displayWait = false;

            // wait for stabilization of 1 second
            if (abs(historicalTemps[0] - historicalTemps[1] < TEMP_VARIATION_ALLOWED) &&
                  abs(historicalTemps[0] - historicalTemps[2] < TEMP_VARIATION_ALLOWED) &&
                  abs(historicalTemps[0] - historicalTemps[3] < TEMP_VARIATION_ALLOWED))
            {
               stableTempCount++;

               // If enough stable measurements in a row received, lock the temperature
               // And display the results
               if (stableTempCount >= NUM_READINGS_NEEDED)
               {
                  lockedTemp = objTemp;
                  longBeep = true;
                  CMU_ClockEnable(cmuClock_TIMER0, true); // Turn on the beep
                  if (((lockedTemp * 9) / 5 + 32 + WRIST_COMP) > FEVER_TEMP)
                  {
                	 // FEVER!  Red LED, long buzz
                	 setGreenLed (false);
                	 setRedLed (true);
                     longCount = 20;  // how long to buzz for
                  }
                  else
                  {
                	 // Normal temperature.  Green LED, short buzz
                 	 setGreenLed (true);
                 	 setRedLed (false);
                     longCount = 2;  // how long to buzz for
                  }
               }
            }
            else
            {
               // The temperature has not stabilized
               stableTempCount = 0;
            }
         }
         else
         {
            // Stable measurement achieved previously
        	// So keep displaying this until the display count
        	// expires
            BSP_LedSet(0);
            displayTemp = true;
            displayMeasure = false;
            displayWait = false;

            tempDisplayedCount++;

            if (tempDisplayedCount > 20)
            {
               // Done displaying - go back to idle
               // to wait for the next measurement
               stableTempCount = 0;
               tempDisplayedCount = 0;
               displayTemp = false;
               displayMeasure = false;
               displayWait = true;
            }
         }
      }
      else  // Temperature is below object detect, so nothing in range
      {
         if (tempDisplayedCount > 0)
         {
            // Keep displaying the stable temperature for a while
            tempDisplayedCount++;
            displayTemp = true;
            displayMeasure = false;
            displayWait = false;

            if (tempDisplayedCount > 15)
            {
               stableTempCount = 0;
               tempDisplayedCount = 0;
               displayTemp = false;
               displayMeasure = false;
               displayWait = true;
            }
         }
         else
         {
            // Done displaying the stable temperature
        	// Go back to idle, await next measurement
        	setGreenLed (true);
        	setRedLed (true);

            BSP_LedClear(0);
            displayTemp = false;
            displayMeasure = false;
            displayWait = true;
         }
      }
    }

    // Push buttons event handling
    // Allow user to switch between the demo and a debug screen
    if (btn0Pressed) {
      // Next application
      currentApp++;
      if (currentApp == APP_COUNT) {
        currentApp = 0;
      }
      // Reset flag, save current application and clear screen
      btn0Pressed = false;
      buSetCurApp(currentApp);
      SegmentLCD_AllOff();
    }

    // Allow user to switch between c and f if showing debug data
    if (btn1Pressed) {
      switch (currentApp) {
        case TEMPERATURE_DETAILS:
          // Toggle between C and F
          if (appRhtCelsius) {
            appRhtCelsius = false;
          } else {
            appRhtCelsius = true;
          }
          break;

        default:
          break;
      }
      // Reset flag
      btn1Pressed = false;
    }

    // Update LCD display
    drawScreen(currentApp);
  }
}

/**************************************************************************//**
 * @brief CRYOTIMER_IRQHandler
 * Interrupt Service Routine for CRYOTIMER Interrupt Line
 *****************************************************************************/
void CRYOTIMER_IRQHandler(void)
{
  CRYOTIMER_IntClear(0xFFFFFFFFUL);

  cryoPeriod = true;
}

/**************************************************************************//**
 * @brief LESENSE_IRQHandler
 * Interrupt Service Routine for LESENSE Interrupt Line
 *****************************************************************************/
void LESENSE_IRQHandler(void)
{
  /* Clear interrupt flag */
  LESENSE_IntClear(LESENSE_IF_CH3);

  lcTriggered = true;
}

/***************************************************************************//**
 * @brief GRIO_ODD_IRQHandler
 * Interrupt Service Routine for GPIO interrupt
 ******************************************************************************/
void GPIO_ODD_IRQHandler(void)
{
  if ((GPIO_IntGet() & _GPIO_IF_EXT_MASK) & (0x1 << BSP_GPIO_PB0_PIN)) {
    GPIO_IntClear(0x1 << BSP_GPIO_PB0_PIN);
    btn0Pressed = true;
  } else {
    GPIO_IntClear(0x1 << BSP_GPIO_PB1_PIN);
    btn1Pressed = true;
  }
}
