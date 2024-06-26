/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
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
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bsp/board.h"
#include "tusb.h"
#include "FreeRTOS_CLI.h"
#include "usb_descriptors.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct gamepad_t_{
	uint16_t debounced_gpio;
	uint16_t previous_debounced_gpio;
	uint32_t gpio_debounce_time[16];
	uint8_t  mapped_button[16];
	bool 	 updated;
}gamepad_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define USBD_STACK_SIZE    (3*configMINIMAL_STACK_SIZE/2) * (CFG_TUSB_DEBUG ? 2 : 1)
#define CDC_STACK_SIZE      2*configMINIMAL_STACK_SIZE
#define HID_STACK_SIZE      configMINIMAL_STACK_SIZE
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;

osThreadId defaultTaskHandle;
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
void StartDefaultTask(void const * argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Blink pattern
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum  {
  BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};

TimerHandle_t blinky_tm;

// USB Device Driver task
// This top level thread process all usb events and invoke callbacks
void usb_device_task(void* param)
{
  (void) param;

  // init device stack on configured roothub port
  // This should be called after scheduler/kernel is started.
  // Otherwise it could cause kernel issue since USB IRQ handler does use RTOS queue API.
  tud_init(BOARD_TUD_RHPORT);

  // RTOS forever loop
  while (1)
  {
    // put this thread to waiting state until there is new events
    tud_task();

    // following code only run if tud_task() process at least 1 event
    //tud_cdc_write_flush();
  }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
  xTimerChangePeriod(blinky_tm, pdMS_TO_TICKS(BLINK_MOUNTED), 0);
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
  xTimerChangePeriod(blinky_tm, pdMS_TO_TICKS(BLINK_NOT_MOUNTED), 0);
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
  (void) remote_wakeup_en;
  xTimerChangePeriod(blinky_tm, pdMS_TO_TICKS(BLINK_SUSPENDED), 0);
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
  xTimerChangePeriod(blinky_tm, pdMS_TO_TICKS(BLINK_MOUNTED), 0);
}

uint16_t gpio_get_all(void){
	uint16_t raw_gpio = GPIOA->IDR;
	// Remove GPIOB Pin 11 from pin mask
	// if a button is pressed, its bit is set
	raw_gpio = raw_gpio & 0x1;
	return raw_gpio;
}

void gamepad_init(gamepad_t *gamepad, uint8_t *mapped_buttons){
	for (int i=0;i<16;i++){
		gamepad->mapped_button[i] = *mapped_buttons++;
		gamepad->gpio_debounce_time[i] = 0;
	}
	gamepad->updated = false;
	gamepad->debounced_gpio = 0;
	//gamepad->previous_debounced_gpio = 0;
}


bool debounceGpioGetAll(gamepad_t *gamepad){
	uint16_t raw_gpio = gpio_get_all();
	if (gamepad->debounced_gpio == (raw_gpio & 0x1)) {
		if (gamepad->debounced_gpio) {
			gamepad->updated = true;
			return true;
		}else{
			gamepad->updated = false;
			return false;
		}
	}

	uint32_t now = board_millis();
	// check each button use case GPIO for state
	for (int pin = 0; pin < 16; pin++) {
		uint16_t pin_mask = 1 << pin;
		if (pin != 11){
			// Allow debouncer to change state if button state changed and debounce delay threshold met
			if ((gamepad->debounced_gpio & pin_mask) != (raw_gpio & pin_mask) && ((now - gamepad->gpio_debounce_time[pin]) > 5)) {
				gamepad->debounced_gpio ^= pin_mask;
				gamepad->gpio_debounce_time[pin] = now;
			}
		}
	}
	gamepad->updated = true;
	return true;
}


#if 1
#define MAX_INPUT_LENGTH    64
#define MAX_OUTPUT_LENGTH   512
SemaphoreHandle_t sem_cdc_rx, sem_cdc_tx;

void CDC_Transmit_HS(void const* buffer, uint32_t bufsize){
	tud_cdc_write((uint8_t *)buffer, bufsize);
    tud_cdc_write_flush();
    xSemaphoreTake(sem_cdc_tx, portMAX_DELAY);
}

static BaseType_t prvTaskStatsCommand( char *pcWriteBuffer,
                                          size_t xWriteBufferLen,
										  const char *pcCommandString )
{
    static BaseType_t state = 0;

    if (!state){
        /* For simplicity, this function assumes the output buffer is large enough
        to hold all the text generated by executing the vTaskList() API function,
        so the xWriteBufferLen parameter is not used. */
        char *head = "Name          State  Priority  Stack  Number\n\r";
        ( void ) xWriteBufferLen;

        /* pcWriteBuffer is used directly as the vTaskList() parameter, so the table
        generated by executing vTaskList() is written directly into the output
        buffer. */
        strcpy(pcWriteBuffer, head);
        vTaskList( &pcWriteBuffer[strlen(head)]);

        /* The entire table was written directly to the output buffer.  Execution
        of this command is complete, so return pdFALSE. */
        state = 1;
        return pdTRUE;
    }else{
        state = 0;
        strcpy(pcWriteBuffer, "\n\r");
        return pdFALSE;
    }
}

static const CLI_Command_Definition_t xTasksCommand =
{
    "tasks",
	"tasks: Lists all the installed tasks\r\n\r\n",
	prvTaskStatsCommand,
    0
};


static BaseType_t prvRunTimeStatsCommand( char *pcWriteBuffer,
                                          size_t xWriteBufferLen,
										  const char *pcCommandString )
{
    /* For simplicity, this function assumes the output buffer is large enough
    to hold all the text generated by executing the vTaskList() API function,
    so the xWriteBufferLen parameter is not used. */
	char *head = "Name          Abs Time      % Time\n\r";
    ( void ) xWriteBufferLen;

    /* pcWriteBuffer is used directly as the vTaskList() parameter, so the table
    generated by executing vTaskList() is written directly into the output
    buffer. */
    strcpy(pcWriteBuffer, head);
    vTaskGetRunTimeStats( &pcWriteBuffer[strlen(head)]);

    /* The entire table was written directly to the output buffer.  Execution
    of this command is complete, so return pdFALSE. */
    return pdFALSE;
}

static const CLI_Command_Definition_t xRunTimeCommand =
{
    "runtime",
	"runtime: Lists the tasks runtime info\r\n\r\n",
	prvRunTimeStatsCommand,
    0
};
//--------------------------------------------------------------------+
// USB CDC
//--------------------------------------------------------------------+
void cdc_task(void* params)
{
  (void) params;
  sem_cdc_rx = xSemaphoreCreateBinary();
  sem_cdc_tx = xSemaphoreCreateBinary();

	char *welcome = "FreeRTOS Console is running!\n\r";
	char cRxedChar;
	uint32_t cInputIndex = 0;
	BaseType_t xMoreDataToFollow;
	/* The input and output buffers are declared static to keep them off the stack. */
	static char pcOutputString[ MAX_OUTPUT_LENGTH ], pcInputString[ MAX_INPUT_LENGTH ];

	do {
		vTaskDelay(10);
	}while (!tud_cdc_connected());

	FreeRTOS_CLIRegisterCommand( &xTasksCommand );
	FreeRTOS_CLIRegisterCommand( &xRunTimeCommand );
	CDC_Transmit_HS(welcome, strlen(welcome));

  // RTOS forever loop
	while(1)
	{
			/* This implementation reads a single character at a time.  Wait in the
			Blocked state until a character is received. */
			xSemaphoreTake(sem_cdc_rx, portMAX_DELAY);
			// read
			(void)tud_cdc_read(&cRxedChar, 1);

			if( cRxedChar == '\r' )
			{
				/* A newline character was received, so the input command string is
				complete and can be processed.  Transmit a line separator, just to
				make the output easier to read. */
				(void)CDC_Transmit_HS((uint8_t *)"\n\r", 2);

				/* The command interpreter is called repeatedly until it returns
				pdFALSE.  See the "Implementing a command" documentation for an
				exaplanation of why this is. */
				do
				{
					/* Send the command string to the command interpreter.  Any
					output generated by the command interpreter will be placed in the
					pcOutputString buffer. */
					xMoreDataToFollow = FreeRTOS_CLIProcessCommand
								  (
									  pcInputString,   /* The command string.*/
									  pcOutputString,  /* The output buffer. */
									  MAX_OUTPUT_LENGTH/* The size of the output buffer. */
								  );

					/* Write the output generated by the command interpreter to the
					console. */
					(void)CDC_Transmit_HS((uint8_t *)pcOutputString, strlen( pcOutputString ) );

				} while( xMoreDataToFollow != pdFALSE );

				/* All the strings generated by the input command have been sent.
				Processing of the command is complete.  Clear the input string ready
				to receive the next command. */
				cInputIndex = 0;
				memset( pcInputString, 0x00, MAX_INPUT_LENGTH );
			}
			else
			{
				/* The if() clause performs the processing after a newline character
				is received.  This else clause performs the processing if any other
				character is received. */

				if( cRxedChar == '\n' )
				{
					/* Ignore carriage returns. */
				}
				else if( cRxedChar == '\b' )
				{
					/* Backspace was pressed.  Erase the last character in the input
					buffer - if there are any. */
					if( cInputIndex > 0 )
					{
						cInputIndex--;
						pcInputString[ cInputIndex ] = '\0';
					}
				}
				else
				{
					/* A character was entered.  It was not a new line, backspace
					or carriage return, so it is accepted as part of the input and
					placed into the input buffer.  When a n is entered the complete
					string will be passed to the command interpreter. */
					if( cInputIndex < MAX_INPUT_LENGTH )
					{
						pcInputString[ cInputIndex ] = cRxedChar;
						(void)CDC_Transmit_HS((uint8_t *)&cRxedChar, 1);
						cInputIndex++;
					}
				}
			}
	}
#if 0
  while ( 1 )
  {
    // connected() check for DTR bit
    // Most but not all terminal client set this when making connection
    // if ( tud_cdc_connected() )
    {
      xSemaphoreTake(sem_cdc_rx, portMAX_DELAY);
      // There are data available
      while ( tud_cdc_available() )
      {
        uint8_t buf[64];

        // read and echo back
        uint32_t count = tud_cdc_read(buf, sizeof(buf));
        (void) count;

        // Echo back
        // Note: Skip echo by commenting out write() and write_flush()
        // for throughput test e.g
        //    $ dd if=/dev/zero of=/dev/ttyACM0 count=10000
        tud_cdc_write(buf, count);
        tud_cdc_write_flush();
        xSemaphoreTake(sem_cdc_tx, portMAX_DELAY);
      }
    }

    // For ESP32-Sx this delay is essential to allow idle how to run and reset watchdog
    //vTaskDelay(1);
  }
#endif
}

// Invoked when cdc when line state changed e.g connected/disconnected
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
  (void) itf;
  (void) rts;

  // TODO set some indicator
  if ( dtr )
  {
    // Terminal connected
  }else
  {
    // Terminal disconnected
  }
}

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf)
{
  (void) itf;
  xSemaphoreGive(sem_cdc_rx);
}

void tud_cdc_tx_complete_cb(uint8_t itf) {
	  (void) itf;
	  xSemaphoreGive(sem_cdc_tx);
}
#endif

//--------------------------------------------------------------------+
// USB HID
//--------------------------------------------------------------------+
uint32_t count = 0;
//static void send_hid_report(uint8_t report_id, uint32_t btn)
static void send_hid_report(uint8_t report_id, gamepad_t *gamepad)
{
  // skip if hid is not ready yet
  if ( !tud_hid_ready() ) return;

  switch(report_id)
  {
    case REPORT_ID_KEYBOARD:
    {
      // use to avoid send multiple consecutive zero report for keyboard
      static bool has_keyboard_key = false;

#if 0
      if ( btn )
      {
        uint8_t keycode[6] = { 0 };
        count++;
        if (count>=600){
        	if (count > 800){
        		count = 0;
        	}
            keycode[0] = HID_KEY_ARROW_RIGHT;
            keycode[1] = HID_KEY_SPACE;
        }else{
            keycode[0] = HID_KEY_ARROW_RIGHT;
        }
#endif
        if (gamepad->updated)
        {
          uint8_t keycode[6] = { 0 };
          int idx = 0;
          for (int pin=0; pin<16;pin++){
          	uint16_t pin_mask = 1 << pin;
          	//uint16_t pin = gamepad->debounced_gpio & pin_mask;
          	//uint16_t previous_pin_state = gamepad->previous_debounced_gpio & pin_mask;

          	if (gamepad->debounced_gpio & pin_mask){
          		keycode[idx++] = gamepad->mapped_button[pin];
          		if (idx >= 6) {
          			// max of 6 different buttons pressed at same time
          			break;
          		}
          	}
          }
        tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keycode);
        has_keyboard_key = true;
      }else
      {
        // send empty key report if previously has key pressed
        if (has_keyboard_key) tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
        has_keyboard_key = false;
      }
    }
    break;

    case REPORT_ID_MOUSE:
    {
      int8_t const delta = 5;

      // no button, right + down, no scroll, no pan
      tud_hid_mouse_report(REPORT_ID_MOUSE, 0x00, delta, delta, 0, 0);
    }
    break;

#if 0
    case REPORT_ID_CONSUMER_CONTROL:
    {
      // use to avoid send multiple consecutive zero report
      static bool has_consumer_key = false;

      if ( btn )
      {
        // volume down
        uint16_t volume_down = HID_USAGE_CONSUMER_VOLUME_DECREMENT;
        tud_hid_report(REPORT_ID_CONSUMER_CONTROL, &volume_down, 2);
        has_consumer_key = true;
      }else
      {
        // send empty key report (release key) if previously has key pressed
        uint16_t empty_key = 0;
        if (has_consumer_key) tud_hid_report(REPORT_ID_CONSUMER_CONTROL, &empty_key, 2);
        has_consumer_key = false;
      }
    }
    break;

    case REPORT_ID_GAMEPAD:
    {
      // use to avoid send multiple consecutive zero report for keyboard
      static bool has_gamepad_key = false;

      hid_gamepad_report_t report =
      {
        .x   = 0, .y = 0, .z = 0, .rz = 0, .rx = 0, .ry = 0,
        .hat = 0, .buttons = 0
      };

      if ( btn )
      {
        report.hat = GAMEPAD_HAT_UP;
        report.buttons = GAMEPAD_BUTTON_A;
        tud_hid_report(REPORT_ID_GAMEPAD, &report, sizeof(report));

        has_gamepad_key = true;
      }else
      {
        report.hat = GAMEPAD_HAT_CENTERED;
        report.buttons = 0;
        if (has_gamepad_key) tud_hid_report(REPORT_ID_GAMEPAD, &report, sizeof(report));
        has_gamepad_key = false;
      }
    }
    break;
#endif

    default: break;
  }
}

static gamepad_t gamepad;
static uint8_t mapped_buttons[16] = {HID_KEY_A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

void hid_task(void* param)
{
  (void) param;
  gamepad_init(&gamepad, (uint8_t *)&mapped_buttons);

  while(1)
  {
    // Poll every 2ms
    vTaskDelay(pdMS_TO_TICKS(2));

    //uint32_t const btn = board_button_read();
    bool state_changed = debounceGpioGetAll(&gamepad);

    // Remote wakeup
    if ( tud_suspended() && state_changed )
    {
      // Wake up host if we are in suspend mode
      // and REMOTE_WAKEUP feature is enabled by host
      tud_remote_wakeup();
    }
    else
    {
      // Send the 1st of report chain, the rest will be sent by tud_hid_report_complete_cb()
      //send_hid_report(REPORT_ID_KEYBOARD, btn);
    	send_hid_report(REPORT_ID_KEYBOARD, &gamepad);
    }
  }
}

// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) instance;
  (void) len;

  //uint8_t next_report_id = report[0] + 1;

  //if (next_report_id < REPORT_ID_COUNT)
  //{
    //send_hid_report(next_report_id, board_button_read());
	  //send_hid_report(REPORT_ID_KEYBOARD, board_button_read());
  send_hid_report(REPORT_ID_KEYBOARD, &gamepad);
  //}
}


// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
  // TODO not Implemented
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;

  return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
  (void) instance;

  if (report_type == HID_REPORT_TYPE_OUTPUT)
  {
    // Set keyboard LED e.g Capslock, Numlock etc...
    if (report_id == REPORT_ID_KEYBOARD)
    {
      // bufsize should be (at least) 1
      if ( bufsize < 1 ) return;

      uint8_t const kbd_leds = buffer[0];

      if (kbd_leds & KEYBOARD_LED_CAPSLOCK)
      {
        // Capslock On: disable blink, turn led on
        xTimerStop(blinky_tm, portMAX_DELAY);
        board_led_write(true);
      }else
      {
        // Caplocks Off: back to normal blink
        board_led_write(false);
        xTimerStart(blinky_tm, portMAX_DELAY);
      }
    }
  }
}

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
void led_blinky_cb(TimerHandle_t xTimer)
{
  (void) xTimer;
  static bool led_state = false;

  board_led_write(led_state);
  led_state = 1 - led_state; // toggle
}
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
  //SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  board_init();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  //osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 512);
  //defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  blinky_tm = xTimerCreate(NULL, pdMS_TO_TICKS(BLINK_NOT_MOUNTED), true, NULL, led_blinky_cb);
  xTaskCreate( usb_device_task, "USB Device", USBD_STACK_SIZE, NULL, configMAX_PRIORITIES-1, NULL);
  xTaskCreate( cdc_task, "Console", CDC_STACK_SIZE, NULL, configMAX_PRIORITIES-3, NULL);
  xTaskCreate( hid_task, "USB HID", HID_STACK_SIZE, NULL, configMAX_PRIORITIES-2, NULL);


  xTimerStart(blinky_tm, 0);
  /* USER CODE END RTOS_THREADS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 1679;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
extern unsigned int ulHighFrequencyTimerTicks;
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
#if 0
void StartDefaultTask(void const * argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}
#endif

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
  if (htim->Instance == TIM2) {
	  ulHighFrequencyTimerTicks++;
  }
  /* USER CODE END Callback 1 */
}

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

#ifdef  USE_FULL_ASSERT
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
