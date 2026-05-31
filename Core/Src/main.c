/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>				// snprintf for BPM string formatting
#include "stm32g0xx_hal.h"		// STM32G0 HAL driver
#include "ssd1306.h"			// OLED display driver
#include "ssd1306_fonts.h"		// OLED font definitions
#include "IHRD.h"				// Irregular Heart Rate Detection module
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define ADC_BUFFER_SIZE 128

// --- High-Pass Filter (DC removal) ---
// Cutoff ~0.5Hz: higher value = lower cutoff, more DC rejection
#define HP_ALPHA                0.95f

// --- Low-Pass Filter (noise removal) ---
// Cutoff ~10Hz: lower value = lower cutoff, more smoothing
// LP_BETA is automatically 1 - LP_ALPHA
#define LP_ALPHA                0.2f
#define LP_BETA                 (1.0f - LP_ALPHA)

// --- Sample Rate ---
#define SAMPLE_RATE_HZ          400.0f
#define MS_PER_SAMPLE           (1000.0f / SAMPLE_RATE_HZ)   // 2.5ms

// --- BPM Averaging ---
// Higher = faster response, lower = smoother reading
#define BPM_AVG_ALPHA           0.2f
#define BPM_AVG_BETA            (1.0f - BPM_AVG_ALPHA)

// --- BPM Validity Range ---
#define BPM_MIN                 40.0f
#define BPM_MAX                 180.0f

// --- Threshold: separate baseline and peak envelope trackers ---
// Baseline tracks slow-moving signal floor (rises fast, falls slow)
#define BASELINE_ATTACK         0.95f
#define BASELINE_DECAY          0.999f
// Peak envelope tracks signal ceiling (rises fast, falls slow)
#define ENVELOPE_ATTACK         0.90f
#define ENVELOPE_DECAY          0.9995f
// Peak must exceed this fraction of the baseline-to-envelope range
#define THRESH_TRIGGER_FRAC     0.55f

// --- Signal Quality / Not-Worn Detection ---
// Minimum AC amplitude (envelope - baseline) to consider signal valid
#define AC_MIN_AMPLITUDE        200.0f

// --- Peak Width Gate (glitch rejection) ---
// Minimum samples the signal must be locally max across (5 = 12.5ms)
#define PEAK_WINDOW             5

// --- Refractory: short debounce only, physiological gate is RR_MIN_MS ---
// Fix: REFRACTORY_SAMPLES (debounce) must be LESS than RR_MIN_MS in samples
// 80 samples = 200ms debounce, RR_MIN_MS = 300ms is the real gate (now reachable)
#define REFRACTORY_SAMPLES      80

// --- RR Interval Validity (ms) ---
#define RR_MIN_MS               300.0f    // ~200 BPM max
#define RR_MAX_MS               1500.0f   // ~40 BPM min

// --- LED Beat Flash Duration (ms) ---
#define BEAT_FLASH_MS           80

// --- Button ---
#define BTN_DEBOUNCE_MS    200    // Minimum ms between valid button presses

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
// DMA
uint16_t adc_buffer[ADC_BUFFER_SIZE];
volatile uint8_t data_ready = 0;

// ADC sample
uint16_t adc_value = 0;

// Timing
uint32_t sample_count = 0;
uint32_t last_peak_sample = 0;
uint32_t last_display_update = 0;
uint32_t last_beat_flash_ms =   0;

// BPM
float bpm = 0;
float bpm_avg = 0;
uint8_t signal_valid = 0;     // 0 = not worn / noisy

// Filtering
float hp = 0;
float lp = 0;
float ppg_signal = 0;
float prev_input = 0;

// 5-sample window for glitch-resistant peak detection
float sig_history[PEAK_WINDOW] = {0};

// Separate baseline and peak envelope (replaces single adaptive threshold)
float sig_baseline =            2048.0f;   // start at mid-scale
float sig_envelope =            2048.0f;

// OLED: track last displayed value to avoid full redraws
int prev_bpm_display =         -1;
uint8_t prev_valid_display =    2;     // force first draw

// IHRD
IrregularHRM_t hrm;
uint8_t prev_ihrd_display = 2;
uint8_t ihrd_alert_active = 0;    // Persistent flag — stays set until button clears it
uint8_t prev_ihrd_alert   = 2;    // Force first draw

// Button
uint32_t last_btn_press_ms = 0;   // Tracks last debounced press timestamp

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  ssd1306_Init();		// Initialize OLED over I2C
  IRHRM_Init(&hrm);		// Initialize IHRD module
  HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_SET);	// red LED start OFF
  HAL_GPIO_WritePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin, GPIO_PIN_SET);// blue LED start OFF
  //Display Project Title to OLED Display
  ssd1306_Fill(Black);
  ssd1306_SetCursor(0, 0);
  ssd1306_WriteString("PHRM", Font_7x10, White);
  ssd1306_UpdateScreen();

  // Start TIM3 — triggers ADC at 400Hz
  HAL_TIM_Base_Start(&htim3);

  // Start ADC — fills buffer via DMA at 400Hz
  HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, ADC_BUFFER_SIZE);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  // Check if DMA buffer is full
	  // This flag is set inside HAL_ADC_ConvCpltCallback()
	  if (data_ready)
	  {
		  data_ready = 0;

		  for (int i = 0; i < ADC_BUFFER_SIZE; i++)
		  {
			  adc_value = adc_buffer[i];
			  sample_count++;

			  // --- Filtering ---
			  hp = HP_ALPHA * (hp + (float)adc_value - prev_input);
			  prev_input = (float)adc_value;
			  lp = LP_ALPHA * hp + LP_BETA * lp;
			  ppg_signal = lp;

			  // --- Separate baseline and envelope trackers ---
			  // Baseline tracks signal floor
			  if (ppg_signal < sig_baseline)
				  sig_baseline = BASELINE_ATTACK * sig_baseline
							   + (1.0f - BASELINE_ATTACK) * ppg_signal;
			  else
				  sig_baseline = BASELINE_DECAY  * sig_baseline
							   + (1.0f - BASELINE_DECAY)  * ppg_signal;

			  // Envelope tracks signal ceiling
			  if (ppg_signal > sig_envelope)
				  sig_envelope = ENVELOPE_ATTACK * sig_envelope
							   + (1.0f - ENVELOPE_ATTACK) * ppg_signal;
			  else
				  sig_envelope = ENVELOPE_DECAY  * sig_envelope
							   + (1.0f - ENVELOPE_DECAY)  * ppg_signal;

			  // --- Signal quality / not-worn gate ---
			  float ac_amplitude = sig_envelope - sig_baseline;			// Signal swing — used for quality gate and threshold scaling
			  signal_valid = (ac_amplitude >= AC_MIN_AMPLITUDE) ? 1 : 0;// Gate all processing on minimum signal strength

			  // --- 5-sample history shift (glitch-resistant local max) ---
			  for (int j = PEAK_WINDOW - 1; j > 0; j--)
				  sig_history[j] = sig_history[j - 1];
			  sig_history[0] = ppg_signal;

			  // --- Peak detection: center of 5-sample window must be global max ---
			  // Only run if signal is valid (worn and not noisy)
			  if (signal_valid)
			  {
				  float center = sig_history[PEAK_WINDOW / 2];  // sig_history[2]
				  uint8_t is_peak = 1;	// Assume local max until a neighbor disproves it
				  for (int j = 0; j < PEAK_WINDOW; j++)
				  {
					  if (j == PEAK_WINDOW / 2) continue;	// Skip center sample
					  if (sig_history[j] >= center) { is_peak = 0; break; }	// Neighbor >= center: not a peak
				  }

				  // Scaled threshold: fraction of baseline-to-envelope range
				  float threshold_trigger = sig_baseline
										  + THRESH_TRIGGER_FRAC * ac_amplitude;

				  if (is_peak && center > threshold_trigger)	 // Valid peak: local max above scaled threshold
				  {
					  uint32_t delta_samples = sample_count - last_peak_sample;	// Samples elapsed since last beat

					  // Short debounce only — physiological check below is the real gate
					  if (delta_samples > REFRACTORY_SAMPLES)
					  {
						  float delta_ms = delta_samples * MS_PER_SAMPLE;	// Convert sample count to milliseconds

						  // Physiological gate (NOW reachable since debounce < this)
						  if (delta_ms > RR_MIN_MS && delta_ms < RR_MAX_MS)
						  {
							  bpm = 60000.0f / delta_ms;	// Instantaneous BPM from RR interval

							  if (bpm > BPM_MIN && bpm < BPM_MAX)
								  bpm_avg = BPM_AVG_BETA * bpm_avg + BPM_AVG_ALPHA * bpm;	// Exponential moving average smooths reading

							  // Feed RR interval into IHRD
							  IRHRM_Update(&hrm, (uint32_t)delta_ms);

							  // Flash red LED on valid beat
							  HAL_GPIO_WritePin(GPIOA, LED_RED_Pin, GPIO_PIN_RESET);
							  last_beat_flash_ms = HAL_GetTick();
						  	  }
						  last_peak_sample = sample_count;	// Record sample index of this beat
					  	  }
				  	  }
			  	  }
		  	  }
		  } // end sample loop

		  // --- Turn off beat flash after BEAT_FLASH_MS ---
		  if (HAL_GetTick() - last_beat_flash_ms > BEAT_FLASH_MS)
			  HAL_GPIO_WritePin(GPIOA, LED_RED_Pin, GPIO_PIN_SET);

		  // --- Button: clear IHRD alert on press ---
		  static uint8_t btn_stable = 0;    // consecutive LOW read counter
		  static uint8_t btn_pressed = 0;	// Prevents re-trigger while button held
		  if (HAL_GPIO_ReadPin(BTN_GPIO_Port, BTN_Pin) == GPIO_PIN_RESET)  // active low
		  {
			  if (btn_stable < 255) btn_stable++;
		  }
		  else
		  {
			  btn_stable = 0;    // reset on any HIGH reading
			  btn_pressed = 0;	// Button released — allow next press
		  }

		  if (btn_stable >= 5 && !btn_pressed)    // require 5 consecutive LOW reads (~5 loop iterations)
		  {
				if (HAL_GetTick() - last_btn_press_ms > BTN_DEBOUNCE_MS)
				{
					btn_pressed       = 1;		// Block re-trigger until button released
					last_btn_press_ms = HAL_GetTick();   // Record press time
					HAL_GPIO_TogglePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin); // Toggle green on each press
					ihrd_alert_active = 0;     // Clear persistent alert
					prev_ihrd_alert   = 2;     // Force OLED alert row redraw
					IRHRM_ClearAlert(&hrm);    // Reset irregular state
					prev_ihrd_display = 2;     // Reset ihrd state tracker to force full redraw
				}
		  }

		  // --- OLED update: only redraw changed content ---
		  if ((HAL_GetTick() - last_display_update) > 500)
		  {
			  last_display_update = HAL_GetTick();	// Reset 500ms display timer
			  int bpm_int = (int)bpm_avg;	// Integer BPM for display
			  uint8_t ihrd_state  = signal_valid ? IRHRM_IsIrregular(&hrm) : 2;	// 0=regular 1=irregular 2=not worn
			  if (ihrd_state == 1)
			      ihrd_alert_active = 1;    // Latch — only cleared by button press

			  // Only redraw BPM number if value or validity changed
			  if (bpm_int != prev_bpm_display || ihrd_state != prev_ihrd_display ||
			      signal_valid != prev_valid_display || ihrd_alert_active != prev_ihrd_alert)
			  {
				  // Update display state trackers
				  prev_bpm_display   = bpm_int;
				  prev_valid_display = signal_valid;
				  prev_ihrd_display  = ihrd_state;
				  prev_ihrd_alert    = ihrd_alert_active;    // Track persistent alert state

				  // Clear entire area below title (y=10 to y=63)
				  // handles both alert→normal and normal→alert transitions
				  ssd1306_FillRectangle(0, 10, 127, 63, Black);

				  if (ihrd_alert_active)
				  {
					  // --- Large irregular rhythm alert ---
					  ssd1306_SetCursor(0, 20);
					  ssd1306_WriteString("WARNING!", Font_16x24, White);
					  ssd1306_SetCursor(0, 50);
					  ssd1306_WriteString("! Irreg Rhythm !", Font_7x10, White);
					  HAL_GPIO_WritePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin, GPIO_PIN_RESET); // blue ON
				  }
				  else
				  {
					  // --- Normal display: BPM row ---
					  ssd1306_SetCursor(0, 20);
					  if (signal_valid)
					  {
						  char bpm_text[12];
						  snprintf(bpm_text, sizeof(bpm_text), "BPM: %d", bpm_int);
						  ssd1306_WriteString(bpm_text, Font_11x18, White);
					  }
					  else
					  {
						  ssd1306_WriteString("BPM: ---", Font_11x18, White);
					  }

					  // --- Status row ---
					  ssd1306_SetCursor(0, 50);
					  if (ihrd_state == 0)
						  ssd1306_WriteString("Regular", Font_7x10, White);

					  HAL_GPIO_WritePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin, GPIO_PIN_SET); // blue OFF
				  }
				  ssd1306_UpdateScreen();
			  }
		  }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// DMA transfer complete — signal main loop that buffer is ready to process
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
        data_ready = 1;	// Set flag; main loop processes buffer and clears it
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
