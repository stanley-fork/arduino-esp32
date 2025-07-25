// Copyright 2015-2025 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "esp32-hal-uart.h"

#if SOC_UART_SUPPORTED
#include "esp32-hal.h"
#include "esp32-hal-periman.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/uart.h"
#include "hal/uart_ll.h"
#include "soc/soc_caps.h"
#include "soc/uart_struct.h"
#include "soc/uart_periph.h"
#include "rom/ets_sys.h"
#include "rom/gpio.h"

#include "driver/gpio.h"
#include "hal/gpio_hal.h"
#include "esp_rom_gpio.h"
#include "esp_private/gpio.h"

#include "driver/rtc_io.h"
#include "driver/lp_io.h"
#include "soc/uart_pins.h"
#include "esp_private/uart_share_hw_ctrl.h"

static int s_uart_debug_nr = 0;         // UART number for debug output
#define REF_TICK_BAUDRATE_LIMIT 250000  // this is maximum UART badrate using REF_TICK as clock

struct uart_struct_t {

#if !CONFIG_DISABLE_HAL_LOCKS
  SemaphoreHandle_t lock;  // UART lock
#endif

  uint8_t num;                     // UART number for IDF driver API
  bool has_peek;                   // flag to indicate that there is a peek byte pending to be read
  uint8_t peek_byte;               // peek byte that has been read but not consumed
  QueueHandle_t uart_event_queue;  // export it by some uartGetEventQueue() function
  // configuration data:: Arduino API typical data
  int8_t _rxPin, _txPin, _ctsPin, _rtsPin;  // UART GPIOs
  uint32_t _baudrate, _config;              // UART baudrate and config
  // UART ESP32 specific data
  uint16_t _rx_buffer_size, _tx_buffer_size;  // UART RX and TX buffer sizes
  bool _inverted;                             // UART inverted signal
  uint8_t _rxfifo_full_thrhd;                 // UART RX FIFO full threshold
  int8_t _uart_clock_source;                  // UART Clock Source used when it is started using uartBegin()
};

#if CONFIG_DISABLE_HAL_LOCKS

#define UART_MUTEX_LOCK()
#define UART_MUTEX_UNLOCK()

static uart_t _uart_bus_array[] = {
  {0, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#if SOC_UART_NUM > 1
  {1, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
#if SOC_UART_NUM > 2
  {2, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
#if SOC_UART_NUM > 3
  {3, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
#if SOC_UART_NUM > 4
  {4, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
#if SOC_UART_NUM > 5
  {5, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
};

#else

#define UART_MUTEX_LOCK() \
  if (uart->lock != NULL) \
    do {                  \
  } while (xSemaphoreTake(uart->lock, portMAX_DELAY) != pdPASS)
#define UART_MUTEX_UNLOCK() \
  if (uart->lock != NULL)   \
  xSemaphoreGive(uart->lock)

static uart_t _uart_bus_array[] = {
  {NULL, 0, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#if SOC_UART_NUM > 1
  {NULL, 1, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
#if SOC_UART_NUM > 2
  {NULL, 2, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
#if SOC_UART_NUM > 3
  {NULL, 3, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
#if SOC_UART_NUM > 4
  {NULL, 4, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
#if SOC_UART_NUM > 5
  {NULL, 5, false, 0, NULL, -1, -1, -1, -1, 0, 0, 0, 0, false, 0, -1},
#endif
};

#endif

#if SOC_UART_LP_NUM >= 1
// LP UART enable pins routine
static bool lp_uart_config_io(uint8_t uart_num, int8_t pin, rtc_gpio_mode_t direction, uint32_t idx) {
  /* Skip configuration if the LP_IO is -1 */
  if (pin < 0) {
    return true;
  }

  // Initialize LP_IO
  if (rtc_gpio_init(pin) != ESP_OK) {
    log_e("Failed to initialize LP_IO %d", pin);
    return false;
  }

  // Set LP_IO direction
  if (rtc_gpio_set_direction(pin, direction) != ESP_OK) {
    log_e("Failed to set LP_IO %d direction", pin);
    return false;
  }

  // Connect pins
  const uart_periph_sig_t *upin = &uart_periph_signal[uart_num].pins[idx];
#if !SOC_LP_GPIO_MATRIX_SUPPORTED  // ESP32-C6/C61/C5
  // When LP_IO Matrix is not support, LP_IO Mux must be connected to the pins
  if (rtc_gpio_iomux_func_sel(pin, upin->iomux_func) != ESP_OK) {
    log_e("Failed to set LP_IO pin %d into Mux function", pin);
    return false;
  }
#else   // So far, only ESP32-P4
  // If the configured pin is the default LP_IO Mux pin for LP UART, then set the LP_IO MUX function
  if (upin->default_gpio == pin) {
    if (rtc_gpio_iomux_func_sel(pin, upin->iomux_func) != ESP_OK) {
      log_e("Failed to set LP_IO pin %d into Mux function", pin);
      return false;
    }
  } else {
    // Otherwise, set the LP_IO Matrix and select FUNC1
    if (rtc_gpio_iomux_func_sel(pin, 1) != ESP_OK) {
      log_e("Failed to set LP_IO pin %d into Mux function GPIO", pin);
      return false;
    }
    // Connect the LP_IO to the LP UART peripheral signal
    esp_err_t ret;
    if (direction == RTC_GPIO_MODE_OUTPUT_ONLY) {
      ret = lp_gpio_connect_out_signal(pin, UART_PERIPH_SIGNAL(uart_num, idx), 0, 0);
    } else {
      ret = lp_gpio_connect_in_signal(pin, UART_PERIPH_SIGNAL(uart_num, idx), 0);
    }
    if (ret != ESP_OK) {
      log_e("Failed to connect LP_IO pin %d to UART%d signal", pin, uart_num);
      return false;
    }
  }
#endif  // SOC_LP_GPIO_MATRIX_SUPPORTED

  return true;
}

// When LP UART needs the RTC IO MUX to set the pin, it will always have fixed pins for RX, TX, CTS and RTS
static bool lpuartCheckPins(int8_t rxPin, int8_t txPin, int8_t ctsPin, int8_t rtsPin, uint8_t uart_nr) {
// check if LP UART is being used and if the pins are valid
#if !SOC_LP_GPIO_MATRIX_SUPPORTED  // ESP32-C6/C61/C5
  uint16_t lp_uart_fixed_pin = uart_periph_signal[uart_nr].pins[SOC_UART_RX_PIN_IDX].default_gpio;
  if (uart_nr >= SOC_UART_HP_NUM) {  // it is a LP UART NUM
    if (rxPin > 0 && rxPin != lp_uart_fixed_pin) {
      log_e("UART%d LP UART requires RX pin to be set to %d.", uart_nr, lp_uart_fixed_pin);
      return false;
    }
    lp_uart_fixed_pin = uart_periph_signal[uart_nr].pins[SOC_UART_TX_PIN_IDX].default_gpio;
    if (txPin > 0 && txPin != lp_uart_fixed_pin) {
      log_e("UART%d LP UART requires TX pin to be set to %d.", uart_nr, lp_uart_fixed_pin);
      return false;
    }
    lp_uart_fixed_pin = uart_periph_signal[uart_nr].pins[SOC_UART_CTS_PIN_IDX].default_gpio;
    if (ctsPin > 0 && ctsPin != lp_uart_fixed_pin) {
      log_e("UART%d LP UART requires CTS pin to be set to %d.", uart_nr, lp_uart_fixed_pin);
      return false;
    }
    lp_uart_fixed_pin = uart_periph_signal[uart_nr].pins[SOC_UART_RTS_PIN_IDX].default_gpio;
    if (rtsPin > 0 && rtsPin != lp_uart_fixed_pin) {
      log_e("UART%d LP UART requires RTS pin to be set to %d.", uart_nr, lp_uart_fixed_pin);
      return false;
    }
  }
  return true;
#else   // ESP32-P4 can set any pin for LP UART
  return true;
#endif  // SOC_LP_GPIO_MATRIX_SUPPORTED
}
#endif  // SOC_UART_LP_NUM >= 1

#ifndef GPIO_FUNC_IN_LOW
#define GPIO_FUNC_IN_LOW GPIO_MATRIX_CONST_ZERO_INPUT
#endif

#ifndef GPIO_FUNC_IN_HIGH
#define GPIO_FUNC_IN_HIGH GPIO_MATRIX_CONST_ONE_INPUT
#endif

// Negative Pin Number will keep it unmodified, thus this function can detach individual pins
// This function will also unset the pins in the Peripheral Manager and set the pin to -1 after detaching
static bool _uartDetachPins(uint8_t uart_num, int8_t rxPin, int8_t txPin, int8_t ctsPin, int8_t rtsPin) {
  if (uart_num >= SOC_UART_NUM) {
    log_e("Serial number is invalid, please use number from 0 to %u", SOC_UART_NUM - 1);
    return false;
  }
  // get UART information
  uart_t *uart = &_uart_bus_array[uart_num];
  bool retCode = true;
  //log_v("detaching UART%d pins: prev,pin RX(%d,%d) TX(%d,%d) CTS(%d,%d) RTS(%d,%d)", uart_num,
  //        uart->_rxPin, rxPin, uart->_txPin, txPin, uart->_ctsPin, ctsPin, uart->_rtsPin, rtsPin); vTaskDelay(10);

  // detaches HP and LP pins and sets Peripheral Manager and UART information
  if (rxPin >= 0 && uart->_rxPin == rxPin && perimanGetPinBusType(rxPin) == ESP32_BUS_TYPE_UART_RX) {
    //gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[rxPin], PIN_FUNC_GPIO);
    esp_rom_gpio_pad_select_gpio(rxPin);
    // avoids causing BREAK in the UART line
    if (uart->_inverted) {
      esp_rom_gpio_connect_in_signal(GPIO_FUNC_IN_LOW, UART_PERIPH_SIGNAL(uart_num, SOC_UART_RX_PIN_IDX), false);
    } else {
      esp_rom_gpio_connect_in_signal(GPIO_FUNC_IN_HIGH, UART_PERIPH_SIGNAL(uart_num, SOC_UART_RX_PIN_IDX), false);
    }
    uart->_rxPin = -1;  // -1 means unassigned/detached
    if (!perimanClearPinBus(rxPin)) {
      retCode = false;
      log_e("UART%d failed to detach RX pin %d", uart_num, rxPin);
    }
  }
  if (txPin >= 0 && uart->_txPin == txPin && perimanGetPinBusType(txPin) == ESP32_BUS_TYPE_UART_TX) {
    //gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[txPin], PIN_FUNC_GPIO);
    esp_rom_gpio_pad_select_gpio(txPin);
    esp_rom_gpio_connect_out_signal(txPin, SIG_GPIO_OUT_IDX, false, false);
    uart->_txPin = -1;  // -1 means unassigned/detached
    if (!perimanClearPinBus(txPin)) {
      retCode = false;
      log_e("UART%d failed to detach TX pin %d", uart_num, txPin);
    }
  }
  if (ctsPin >= 0 && uart->_ctsPin == ctsPin && perimanGetPinBusType(ctsPin) == ESP32_BUS_TYPE_UART_CTS) {
    //gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[ctsPin], PIN_FUNC_GPIO);
    esp_rom_gpio_pad_select_gpio(ctsPin);
    esp_rom_gpio_connect_in_signal(GPIO_FUNC_IN_LOW, UART_PERIPH_SIGNAL(uart_num, SOC_UART_CTS_PIN_IDX), false);
    uart->_ctsPin = -1;  // -1 means unassigned/detached
    if (!perimanClearPinBus(ctsPin)) {
      retCode = false;
      log_e("UART%d failed to detach CTS pin %d", uart_num, ctsPin);
    }
  }
  if (rtsPin >= 0 && uart->_rtsPin == rtsPin && perimanGetPinBusType(rtsPin) == ESP32_BUS_TYPE_UART_RTS) {
    //gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[rtsPin], PIN_FUNC_GPIO);
    esp_rom_gpio_pad_select_gpio(rtsPin);
    esp_rom_gpio_connect_out_signal(rtsPin, SIG_GPIO_OUT_IDX, false, false);
    uart->_rtsPin = -1;  // -1 means unassigned/detached
    if (!perimanClearPinBus(rtsPin)) {
      retCode = false;
      log_e("UART%d failed to detach RTS pin %d", uart_num, rtsPin);
    }
  }
  return retCode;
}

// Peripheral Manager detach callback for each specific UART PIN
static bool _uartDetachBus_RX(void *busptr) {
  // sanity check - it should never happen
  assert(busptr && "_uartDetachBus_RX bus NULL pointer.");
  uart_t *bus = (uart_t *)busptr;
  return _uartDetachPins(bus->num, bus->_rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static bool _uartDetachBus_TX(void *busptr) {
  // sanity check - it should never happen
  assert(busptr && "_uartDetachBus_TX bus NULL pointer.");
  uart_t *bus = (uart_t *)busptr;
  return _uartDetachPins(bus->num, UART_PIN_NO_CHANGE, bus->_txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static bool _uartDetachBus_CTS(void *busptr) {
  // sanity check - it should never happen
  assert(busptr && "_uartDetachBus_CTS bus NULL pointer.");
  uart_t *bus = (uart_t *)busptr;
  return _uartDetachPins(bus->num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, bus->_ctsPin, UART_PIN_NO_CHANGE);
}

static bool _uartDetachBus_RTS(void *busptr) {
  // sanity check - it should never happen
  assert(busptr && "_uartDetachBus_RTS bus NULL pointer.");
  uart_t *bus = (uart_t *)busptr;
  return _uartDetachPins(bus->num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, bus->_rtsPin);
}

static bool _uartTrySetIomuxPin(uart_port_t uart_num, int io_num, uint32_t idx) {
  // Store a pointer to the default pin, to optimize access to its fields.
  const uart_periph_sig_t *upin = &uart_periph_signal[uart_num].pins[idx];

  // In theory, if default_gpio is -1, iomux_func should also be -1, but let's be safe and test both.
  if (upin->iomux_func == -1 || upin->default_gpio == -1 || upin->default_gpio != io_num) {
    return false;
  }

  // Assign the correct function to the GPIO.
  if (upin->iomux_func == -1) {
    log_e("IO#%d has bad IOMUX internal information. Switching to GPIO Matrix UART function.", io_num);
    return false;
  }
  if (uart_num < SOC_UART_HP_NUM) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    if (upin->input) {
      gpio_iomux_input(io_num, upin->iomux_func, upin->signal);
    } else {
      gpio_iomux_output(io_num, upin->iomux_func);
    }
#else
    gpio_iomux_out(io_num, upin->iomux_func, false);
    // If the pin is input, we also have to redirect the signal, in order to bypass the GPIO matrix.
    if (upin->input) {
      gpio_iomux_in(io_num, upin->signal);
    }
#endif
  }
#if (SOC_UART_LP_NUM >= 1) && (SOC_RTCIO_PIN_COUNT >= 1)
  else {
    if (upin->input) {
      rtc_gpio_set_direction(io_num, RTC_GPIO_MODE_INPUT_ONLY);
    } else {
      rtc_gpio_set_direction(io_num, RTC_GPIO_MODE_OUTPUT_ONLY);
    }
    rtc_gpio_init(io_num);
    rtc_gpio_iomux_func_sel(io_num, upin->iomux_func);
  }
#endif
  return true;
}

static esp_err_t _uartInternalSetPin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num) {
  // Since an IO cannot route peripheral signals via IOMUX and GPIO matrix at the same time,
  // if tx and rx share the same IO, both signals need to be routed to IOs through GPIO matrix
  bool tx_rx_same_io = (tx_io_num == rx_io_num);

  // In the following statements, if the io_num is negative, no need to configure anything.
  if (tx_io_num >= 0) {
#if CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND || CONFIG_PM_SLP_DISABLE_GPIO
    // In such case, IOs are going to switch to sleep configuration (isolate) when entering sleep for power saving reason
    // But TX IO in isolate state could write garbled data to the other end
    // Therefore, we should disable the switch of the TX pin to sleep configuration
    gpio_sleep_sel_dis(tx_io_num);
#endif
    if (tx_rx_same_io || !_uartTrySetIomuxPin(uart_num, tx_io_num, SOC_UART_TX_PIN_IDX)) {
      if (uart_num < SOC_UART_HP_NUM) {
        gpio_func_sel(tx_io_num, PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(tx_io_num, UART_PERIPH_SIGNAL(uart_num, SOC_UART_TX_PIN_IDX), 0, 0);
        // output enable is set inside esp_rom_gpio_connect_out_signal func after the signal is connected
        // (output enabled too early may cause unnecessary level change at the pad)
      }
#if SOC_LP_GPIO_MATRIX_SUPPORTED
      else {
        rtc_gpio_init(tx_io_num);  // set as a LP_GPIO pin
        lp_gpio_connect_out_signal(tx_io_num, UART_PERIPH_SIGNAL(uart_num, SOC_UART_TX_PIN_IDX), 0, 0);
        // output enable is set inside lp_gpio_connect_out_signal func after the signal is connected
      }
#endif
    }
  }

  if (rx_io_num >= 0) {
#if CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND || CONFIG_PM_SLP_DISABLE_GPIO
    // In such case, IOs are going to switch to sleep configuration (isolate) when entering sleep for power saving reason
    // But RX IO in isolate state could receive garbled data into FIFO, which is not desired
    // Therefore, we should disable the switch of the RX pin to sleep configuration
    gpio_sleep_sel_dis(rx_io_num);
#endif
    if (tx_rx_same_io || !_uartTrySetIomuxPin(uart_num, rx_io_num, SOC_UART_RX_PIN_IDX)) {
      if (uart_num < SOC_UART_HP_NUM) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
        gpio_input_enable(rx_io_num);
#else
        gpio_func_sel(rx_io_num, PIN_FUNC_GPIO);
        gpio_ll_input_enable(&GPIO, rx_io_num);
#endif
        esp_rom_gpio_connect_in_signal(rx_io_num, UART_PERIPH_SIGNAL(uart_num, SOC_UART_RX_PIN_IDX), 0);
      }
#if SOC_LP_GPIO_MATRIX_SUPPORTED
      else {
        rtc_gpio_mode_t mode = (tx_rx_same_io ? RTC_GPIO_MODE_INPUT_OUTPUT : RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_set_direction(rx_io_num, mode);
        if (!tx_rx_same_io) {        // set the same pin again as a LP_GPIO will overwrite connected out_signal, not desired, so skip
          rtc_gpio_init(rx_io_num);  // set as a LP_GPIO pin
        }
        lp_gpio_connect_in_signal(rx_io_num, UART_PERIPH_SIGNAL(uart_num, SOC_UART_RX_PIN_IDX), 0);
      }
#endif
    }
  }

  if (rts_io_num >= 0 && !_uartTrySetIomuxPin(uart_num, rts_io_num, SOC_UART_RTS_PIN_IDX)) {
    if (uart_num < SOC_UART_HP_NUM) {
      gpio_func_sel(rts_io_num, PIN_FUNC_GPIO);
      esp_rom_gpio_connect_out_signal(rts_io_num, UART_PERIPH_SIGNAL(uart_num, SOC_UART_RTS_PIN_IDX), 0, 0);
      // output enable is set inside esp_rom_gpio_connect_out_signal func after the signal is connected
    }
#if SOC_LP_GPIO_MATRIX_SUPPORTED
    else {
      rtc_gpio_init(rts_io_num);  // set as a LP_GPIO pin
      lp_gpio_connect_out_signal(rts_io_num, UART_PERIPH_SIGNAL(uart_num, SOC_UART_RTS_PIN_IDX), 0, 0);
      // output enable is set inside lp_gpio_connect_out_signal func after the signal is connected
    }
#endif
  }

  if (cts_io_num >= 0 && !_uartTrySetIomuxPin(uart_num, cts_io_num, SOC_UART_CTS_PIN_IDX)) {
    if (uart_num < SOC_UART_HP_NUM) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
      gpio_pullup_en(cts_io_num);
      gpio_input_enable(cts_io_num);
#else
      gpio_func_sel(cts_io_num, PIN_FUNC_GPIO);
      gpio_set_pull_mode(cts_io_num, GPIO_PULLUP_ONLY);
      gpio_set_direction(cts_io_num, GPIO_MODE_INPUT);
#endif
      esp_rom_gpio_connect_in_signal(cts_io_num, UART_PERIPH_SIGNAL(uart_num, SOC_UART_CTS_PIN_IDX), 0);
    }
#if SOC_LP_GPIO_MATRIX_SUPPORTED
    else {
      rtc_gpio_set_direction(cts_io_num, RTC_GPIO_MODE_INPUT_ONLY);
      rtc_gpio_init(cts_io_num);  // set as a LP_GPIO pin
      lp_gpio_connect_in_signal(cts_io_num, UART_PERIPH_SIGNAL(uart_num, SOC_UART_CTS_PIN_IDX), 0);
    }
#endif
  }
  return ESP_OK;
}

// Attach function for UART
// connects the IO Pad, set Paripheral Manager and internal UART structure data
static bool _uartAttachPins(uint8_t uart_num, int8_t rxPin, int8_t txPin, int8_t ctsPin, int8_t rtsPin) {
  if (uart_num >= SOC_UART_NUM) {
    log_e("Serial number is invalid, please use number from 0 to %u", SOC_UART_NUM - 1);
    return false;
  }
  // get UART information
  uart_t *uart = &_uart_bus_array[uart_num];
  //log_v("attaching UART%d pins: prev,new RX(%d,%d) TX(%d,%d) CTS(%d,%d) RTS(%d,%d)", uart_num,
  //        uart->_rxPin, rxPin, uart->_txPin, txPin, uart->_ctsPin, ctsPin, uart->_rtsPin, rtsPin); vTaskDelay(10);

  // IDF _uartInternalSetPin() checks if the pin is used within LP UART and if it is a valid RTC IO pin
  // No need for Arduino Layer to check it again
  bool retCode = true;
  if (rxPin >= 0) {
    // forces a clean detaching from a previous peripheral
    if (perimanGetPinBusType(rxPin) != ESP32_BUS_TYPE_INIT) {
      perimanClearPinBus(rxPin);
    }
    // connect RX Pad
    bool ret = ESP_OK == _uartInternalSetPin(uart->num, UART_PIN_NO_CHANGE, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#if SOC_UART_LP_NUM >= 1
    if (ret && uart_num >= SOC_UART_HP_NUM) {  // it is a LP UART NUM
      ret &= lp_uart_config_io(uart->num, rxPin, RTC_GPIO_MODE_INPUT_ONLY, SOC_UART_RX_PIN_IDX);
    }
#endif
    if (ret) {
      ret &= perimanSetPinBus(rxPin, ESP32_BUS_TYPE_UART_RX, (void *)uart, uart_num, -1);
      if (ret) {
        uart->_rxPin = rxPin;
      }
    }
    if (!ret) {
      log_e("UART%d failed to attach RX pin %d", uart_num, rxPin);
    }
    retCode &= ret;
  }
  if (txPin >= 0) {
    // forces a clean detaching from a previous peripheral
    if (perimanGetPinBusType(txPin) != ESP32_BUS_TYPE_INIT) {
      perimanClearPinBus(txPin);
    }
    // connect TX Pad
    bool ret = ESP_OK == _uartInternalSetPin(uart->num, txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#if SOC_UART_LP_NUM >= 1
    if (ret && uart_num >= SOC_UART_HP_NUM) {  // it is a LP UART NUM
      ret &= lp_uart_config_io(uart->num, txPin, RTC_GPIO_MODE_OUTPUT_ONLY, SOC_UART_TX_PIN_IDX);
    }
#endif
    if (ret) {
      ret &= perimanSetPinBus(txPin, ESP32_BUS_TYPE_UART_TX, (void *)uart, uart_num, -1);
      if (ret) {
        uart->_txPin = txPin;
      }
    }
    if (!ret) {
      log_e("UART%d failed to attach TX pin %d", uart_num, txPin);
    }
    retCode &= ret;
  }
  if (ctsPin >= 0) {
    // forces a clean detaching from a previous peripheral
    if (perimanGetPinBusType(ctsPin) != ESP32_BUS_TYPE_INIT) {
      perimanClearPinBus(ctsPin);
    }
    // connect CTS Pad
    bool ret = ESP_OK == _uartInternalSetPin(uart->num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, ctsPin);
#if SOC_UART_LP_NUM >= 1
    if (ret && uart_num >= SOC_UART_HP_NUM) {  // it is a LP UART NUM
      ret &= lp_uart_config_io(uart->num, ctsPin, RTC_GPIO_MODE_INPUT_ONLY, SOC_UART_CTS_PIN_IDX);
    }
#endif
    if (ret) {
      ret &= perimanSetPinBus(ctsPin, ESP32_BUS_TYPE_UART_CTS, (void *)uart, uart_num, -1);
      if (ret) {
        uart->_ctsPin = ctsPin;
      }
    }
    if (!ret) {
      log_e("UART%d failed to attach CTS pin %d", uart_num, ctsPin);
    }
    retCode &= ret;
  }
  if (rtsPin >= 0) {
    // forces a clean detaching from a previous peripheral
    if (perimanGetPinBusType(rtsPin) != ESP32_BUS_TYPE_INIT) {
      perimanClearPinBus(rtsPin);
    }
    // connect RTS Pad
    bool ret = ESP_OK == _uartInternalSetPin(uart->num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, rtsPin, UART_PIN_NO_CHANGE);
#if SOC_UART_LP_NUM >= 1
    if (ret && uart_num >= SOC_UART_HP_NUM) {  // it is a LP UART NUM
      ret &= lp_uart_config_io(uart->num, rtsPin, RTC_GPIO_MODE_OUTPUT_ONLY, SOC_UART_RTS_PIN_IDX);
    }
#endif
    if (ret) {
      ret &= perimanSetPinBus(rtsPin, ESP32_BUS_TYPE_UART_RTS, (void *)uart, uart_num, -1);
      if (ret) {
        uart->_rtsPin = rtsPin;
      }
    }
    if (!ret) {
      log_e("UART%d failed to attach RTS pin %d", uart_num, rtsPin);
    }
    retCode &= ret;
  }
  return retCode;
}

// just helper functions
int8_t uart_get_RxPin(uint8_t uart_num) {
  return _uart_bus_array[uart_num]._rxPin;
}

int8_t uart_get_TxPin(uint8_t uart_num) {
  return _uart_bus_array[uart_num]._txPin;
}

void uart_init_PeriMan(void) {
  // set Peripheral Manager deInit Callback for each UART pin
  perimanSetBusDeinit(ESP32_BUS_TYPE_UART_RX, _uartDetachBus_RX);
  perimanSetBusDeinit(ESP32_BUS_TYPE_UART_TX, _uartDetachBus_TX);
  perimanSetBusDeinit(ESP32_BUS_TYPE_UART_CTS, _uartDetachBus_CTS);
  perimanSetBusDeinit(ESP32_BUS_TYPE_UART_RTS, _uartDetachBus_RTS);
}

// Routines that take care of UART events will be in the HardwareSerial Class code
void uartGetEventQueue(uart_t *uart, QueueHandle_t *q) {
  // passing back NULL for the Queue pointer when UART is not initialized yet
  *q = NULL;
  if (uart == NULL) {
    return;
  }
  *q = uart->uart_event_queue;
  return;
}

bool uartIsDriverInstalled(uart_t *uart) {
  if (uart == NULL) {
    return false;
  }

  if (uart_is_driver_installed(uart->num)) {
    return true;
  }
  return false;
}

// Negative Pin Number will keep it unmodified, thus this function can set individual pins
// When pins are changed, it will detach the previous one
bool uartSetPins(uint8_t uart_num, int8_t rxPin, int8_t txPin, int8_t ctsPin, int8_t rtsPin) {
  if (uart_num >= SOC_UART_NUM) {
    log_e("Serial number is invalid, please use number from 0 to %u", SOC_UART_NUM - 1);
    return false;
  }
  // get UART information
  uart_t *uart = &_uart_bus_array[uart_num];

#if SOC_UART_LP_NUM >= 1
  // check if LP UART is being used and if the pins are valid
  if (!lpuartCheckPins(rxPin, txPin, ctsPin, rtsPin, uart_num)) {
    return false;  // failed to set pins
  }
#endif

  bool retCode = true;
  UART_MUTEX_LOCK();

  //log_v("setting UART%d pins: prev->new RX(%d->%d) TX(%d->%d) CTS(%d->%d) RTS(%d->%d)", uart_num,
  //        uart->_rxPin, rxPin, uart->_txPin, txPin, uart->_ctsPin, ctsPin, uart->_rtsPin, rtsPin); vTaskDelay(10);

  // First step: detaches all previous UART pins
  bool rxPinChanged = rxPin >= 0 && rxPin != uart->_rxPin;
  if (rxPinChanged) {
    retCode &= _uartDetachPins(uart_num, uart->_rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  }
  bool txPinChanged = txPin >= 0 && txPin != uart->_txPin;
  if (txPinChanged) {
    retCode &= _uartDetachPins(uart_num, UART_PIN_NO_CHANGE, uart->_txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  }
  bool ctsPinChanged = ctsPin >= 0 && ctsPin != uart->_ctsPin;
  if (ctsPinChanged) {
    retCode &= _uartDetachPins(uart_num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, uart->_ctsPin, UART_PIN_NO_CHANGE);
  }
  bool rtsPinChanged = rtsPin >= 0 && rtsPin != uart->_rtsPin;
  if (rtsPinChanged) {
    retCode &= _uartDetachPins(uart_num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, uart->_rtsPin);
  }

  // Second step: attach all UART new pins
  if (rxPinChanged) {
    retCode &= _uartAttachPins(uart_num, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  }
  if (txPinChanged) {
    retCode &= _uartAttachPins(uart_num, UART_PIN_NO_CHANGE, txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  }
  if (ctsPinChanged) {
    retCode &= _uartAttachPins(uart->num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, ctsPin, UART_PIN_NO_CHANGE);
  }
  if (rtsPinChanged) {
    retCode &= _uartAttachPins(uart->num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, rtsPin);
  }
  UART_MUTEX_UNLOCK();

  if (!retCode) {
    log_e("UART%d set pins failed.", uart_num);
  }
  return retCode;
}

//
bool uartSetHwFlowCtrlMode(uart_t *uart, uart_hw_flowcontrol_t mode, uint8_t threshold) {
  if (uart == NULL) {
    return false;
  }
  // IDF will issue corresponding error message when mode or threshold are wrong and prevent crashing
  // IDF will check (mode > HW_FLOWCTRL_CTS_RTS || threshold >= SOC_UART_FIFO_LEN)
  UART_MUTEX_LOCK();
  bool retCode = (ESP_OK == uart_set_hw_flow_ctrl(uart->num, mode, threshold));
  UART_MUTEX_UNLOCK();
  return retCode;
}

// This helper function will return true if a new IDF UART driver needs to be restarted and false if the current one can continue its execution
bool _testUartBegin(
  uint8_t uart_nr, uint32_t baudrate, uint32_t config, int8_t rxPin, int8_t txPin, uint32_t rx_buffer_size, uint32_t tx_buffer_size, bool inverted,
  uint8_t rxfifo_full_thrhd
) {
  if (uart_nr >= SOC_UART_NUM) {
    return false;  // no new driver has to be installed
  }
  uart_t *uart = &_uart_bus_array[uart_nr];
  // verify if is necessary to restart the UART driver
  if (uart_is_driver_installed(uart_nr)) {
    // some parameters can't be changed unless we end the UART driver
    if (uart->_rx_buffer_size != rx_buffer_size || uart->_tx_buffer_size != tx_buffer_size || uart->_inverted != inverted
        || uart->_rxfifo_full_thrhd != rxfifo_full_thrhd) {
      return true;  // the current IDF UART driver must be terminated and a new driver shall be installed
    } else {
      return false;  // The current IDF UART driver can continue its execution
    }
  } else {
    return true;  // no IDF UART driver is running and a new driver shall be installed
  }
}

uart_t *uartBegin(
  uint8_t uart_nr, uint32_t baudrate, uint32_t config, int8_t rxPin, int8_t txPin, uint32_t rx_buffer_size, uint32_t tx_buffer_size, bool inverted,
  uint8_t rxfifo_full_thrhd
) {
  if (uart_nr >= SOC_UART_NUM) {
    log_e("UART number is invalid, please use number from 0 to %u", SOC_UART_NUM - 1);
    return NULL;  // no new driver was installed
  }
  uart_t *uart = &_uart_bus_array[uart_nr];
  log_v("UART%d baud(%ld) Mode(%x) rxPin(%d) txPin(%d)", uart_nr, baudrate, config, rxPin, txPin);

#if SOC_UART_LP_NUM >= 1
  // check if LP UART is being used and if the pins are valid
  if (!lpuartCheckPins(rxPin, txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, uart_nr)) {
    if (uart_is_driver_installed(uart_nr)) {
      return uart;  // keep the same installed driver
    } else {
      return NULL;  // no new driver was installed
    }
  }
#endif

#if !CONFIG_DISABLE_HAL_LOCKS
  if (uart->lock == NULL) {
    uart->lock = xSemaphoreCreateMutex();
    if (uart->lock == NULL) {
      log_e("HAL LOCK error.");
      return NULL;  // no new driver was installed
    }
  }
#endif

  if (uart_is_driver_installed(uart_nr)) {
    log_v("UART%d Driver already installed.", uart_nr);
    // some parameters can't be changed unless we end the UART driver
    if (uart->_rx_buffer_size != rx_buffer_size || uart->_tx_buffer_size != tx_buffer_size || uart->_inverted != inverted
        || uart->_rxfifo_full_thrhd != rxfifo_full_thrhd) {
      log_v("UART%d changing buffer sizes or inverted signal or rxfifo_full_thrhd. IDF driver will be restarted", uart_nr);
      log_v("RX buffer size: %d -> %d", uart->_rx_buffer_size, rx_buffer_size);
      log_v("TX buffer size: %d -> %d", uart->_tx_buffer_size, tx_buffer_size);
      log_v("Inverted signal: %s -> %s", uart->_inverted ? "true" : "false", inverted ? "true" : "false");
      log_v("RX FIFO full threshold: %d -> %d", uart->_rxfifo_full_thrhd, rxfifo_full_thrhd);
      uartEnd(uart_nr);
    } else {
      bool retCode = true;
      //User may just want to change some parameters, such as baudrate, data length, parity, stop bits or pins
      if (uart->_baudrate != baudrate) {
        retCode = uartSetBaudRate(uart, baudrate);
      }
      UART_MUTEX_LOCK();
      uart_word_length_t data_bits = (config & 0xc) >> 2;
      uart_parity_t parity = config & 0x3;
      uart_stop_bits_t stop_bits = (config & 0x30) >> 4;
      if (retCode && (uart->_config & 0xc) >> 2 != data_bits) {
        if (ESP_OK != uart_set_word_length(uart_nr, data_bits)) {
          log_e("UART%d changing data length failed.", uart_nr);
          retCode = false;
        } else {
          log_v("UART%d changed data length to %d", uart_nr, data_bits + 5);
        }
      }
      if (retCode && (uart->_config & 0x3) != parity) {
        if (ESP_OK != uart_set_parity(uart_nr, parity)) {
          log_e("UART%d changing parity failed.", uart_nr);
          retCode = false;
        } else {
          log_v("UART%d changed parity to %s", uart_nr, parity == 0 ? "NONE" : parity == 2 ? "EVEN" : "ODD");
        }
      }
      if (retCode && (uart->_config & 0xc30) >> 4 != stop_bits) {
        if (ESP_OK != uart_set_stop_bits(uart_nr, stop_bits)) {
          log_e("UART%d changing stop bits failed.", uart_nr);
          retCode = false;
        } else {
          log_v("UART%d changed stop bits to %d", uart_nr, stop_bits == 3 ? 2 : 1);
        }
      }
      if (retCode) {
        uart->_config = config;
      }
      if (retCode && rxPin > 0 && uart->_rxPin != rxPin) {
        retCode &= _uartDetachPins(uart_nr, uart->_rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        retCode &= _uartAttachPins(uart_nr, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (!retCode) {
          log_e("UART%d changing RX pin failed.", uart_nr);
        } else {
          log_v("UART%d changed RX pin to %d", uart_nr, rxPin);
        }
      }
      if (retCode && txPin > 0 && uart->_txPin != txPin) {
        retCode &= _uartDetachPins(uart_nr, UART_PIN_NO_CHANGE, uart->_txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        retCode &= _uartAttachPins(uart_nr, UART_PIN_NO_CHANGE, txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (!retCode) {
          log_e("UART%d changing TX pin failed.", uart_nr);
        } else {
          log_v("UART%d changed TX pin to %d", uart_nr, txPin);
        }
      }
      UART_MUTEX_UNLOCK();
      if (retCode) {
        // UART driver was already working, just return the uart_t structure, saying that no new driver was installed
        return uart;
      }
      // if we reach this point, it means that we need to restart the UART driver
      uartEnd(uart_nr);
    }
  } else {
    log_v("UART%d not installed. Starting installation", uart_nr);
  }
  uart_config_t uart_config;
  memset(&uart_config, 0, sizeof(uart_config_t));
  uart_config.flags.backup_before_sleep = false;  // new flag from IDF v5.3
  uart_config.data_bits = (config & 0xc) >> 2;
  uart_config.parity = (config & 0x3);
  uart_config.stop_bits = (config & 0x30) >> 4;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = rxfifo_full_thrhd >= UART_HW_FIFO_LEN(uart_nr) ? UART_HW_FIFO_LEN(uart_nr) - 6 : rxfifo_full_thrhd;
  log_v(
    "UART%d RX FIFO full threshold set to %d (value requested: %d || FIFO Max = %d)", uart_nr, uart_config.rx_flow_ctrl_thresh, rxfifo_full_thrhd,
    UART_HW_FIFO_LEN(uart_nr)
  );
  rxfifo_full_thrhd = uart_config.rx_flow_ctrl_thresh;  // makes sure that it will be set correctly in the struct
  uart_config.baud_rate = baudrate;
#if SOC_UART_LP_NUM >= 1
  if (uart_nr >= SOC_UART_HP_NUM) {  // it is a LP UART NUM
    if (uart->_uart_clock_source > 0) {
      uart_config.lp_source_clk = (soc_periph_lp_uart_clk_src_t)uart->_uart_clock_source;  // use user defined LP UART clock
      log_v("Setting UART%d to user defined LP clock source (%d) ", uart_nr, uart->_uart_clock_source);
    } else {
      uart_config.lp_source_clk = LP_UART_SCLK_DEFAULT;  // use default LP clock
      log_v("Setting UART%d to Default LP clock source", uart_nr);
    }
  } else
#endif  // SOC_UART_LP_NUM >= 1
  {
    if (uart->_uart_clock_source >= 0) {
      uart_config.source_clk = (soc_module_clk_t)uart->_uart_clock_source;  // use user defined HP UART clock
      log_v("Setting UART%d to user defined HP clock source (%d) ", uart_nr, uart->_uart_clock_source);
    } else {
      // there is an issue when returning from light sleep with the C6 and H2: the uart baud rate is not restored
      // therefore, uart clock source will set to XTAL for all SoC that support it. This fix solves the C6|H2 issue.
#if SOC_UART_SUPPORT_XTAL_CLK
      uart_config.source_clk = UART_SCLK_XTAL;  // valid for C2, S3, C3, C6, H2 and P4
      log_v("Setting UART%d to use XTAL clock", uart_nr);
#elif SOC_UART_SUPPORT_REF_TICK
      if (baudrate <= REF_TICK_BAUDRATE_LIMIT) {
        uart_config.source_clk = UART_SCLK_REF_TICK;  // valid for ESP32, S2 - MAX supported baud rate is 250 Kbps
        log_v("Setting UART%d to use REF_TICK clock", uart_nr);
      } else {
        uart_config.source_clk = UART_SCLK_APB;  // baudrate may change with the APB Frequency!
        log_v("Setting UART%d to use APB clock", uart_nr);
      }
#else
      // Default CLK Source: CLK_APB for ESP32|S2|S3|C3 -- CLK_PLL_F40M for C2 -- CLK_PLL_F48M for H2 -- CLK_PLL_F80M for C6|P4
      uart_config.source_clk = UART_SCLK_DEFAULT;  // baudrate may change with the APB Frequency!
      log_v("Setting UART%d to use DEFAULT clock", uart_nr);
#endif  // SOC_UART_SUPPORT_XTAL_CLK
    }
  }

  UART_MUTEX_LOCK();
  bool retCode = ESP_OK == uart_driver_install(uart_nr, rx_buffer_size, tx_buffer_size, 20, &(uart->uart_event_queue), 0);

  if (retCode) {
    retCode &= ESP_OK == uart_param_config(uart_nr, &uart_config);
  }

  if (retCode) {
    if (inverted) {
      // invert signal for both Rx and Tx
      retCode &= ESP_OK == uart_set_line_inverse(uart_nr, UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV);
    } else {
      // disable invert signal for both Rx and Tx
      retCode &= ESP_OK == uart_set_line_inverse(uart_nr, UART_SIGNAL_INV_DISABLE);
    }
  }
  // if all fine, set internal parameters
  if (retCode) {
    uart->_baudrate = baudrate;
    uart->_config = config;
    uart->_inverted = inverted;
    uart->_rxfifo_full_thrhd = rxfifo_full_thrhd;
    uart->_rx_buffer_size = rx_buffer_size;
    uart->_tx_buffer_size = tx_buffer_size;
    uart->has_peek = false;
    uart->peek_byte = 0;
#if SOC_UART_LP_NUM >= 1
    if (uart_nr >= SOC_UART_HP_NUM) {
      uart->_uart_clock_source = uart_config.lp_source_clk;
    } else
#endif
    {
      uart->_uart_clock_source = uart_config.source_clk;
    }
  }
  UART_MUTEX_UNLOCK();

  // uartSetPins detaches previous pins if new ones are used over a previous begin()
  if (retCode) {
    retCode &= uartSetPins(uart_nr, rxPin, txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  }
  if (!retCode) {
    log_e("UART%d initialization error.", uart->num);
    uartEnd(uart_nr);
    uart = NULL;
  } else {
    uartFlush(uart);
    log_v("UART%d initialization done.", uart->num);
  }
  return uart;  // a new driver was installed
}

// This function code is under testing - for now just keep it here
void uartSetFastReading(uart_t *uart) {
  if (uart == NULL) {
    return;
  }

  UART_MUTEX_LOCK();
  // override default RX IDF Driver Interrupt - no BREAK, PARITY or OVERFLOW
  uart_intr_config_t uart_intr = {
    .intr_enable_mask = UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT,  // only these IRQs - no BREAK, PARITY or OVERFLOW
    .rx_timeout_thresh = 1,
    .txfifo_empty_intr_thresh = 10,
    .rxfifo_full_thresh = 2,
  };

  ESP_ERROR_CHECK(uart_intr_config(uart->num, &uart_intr));
  UART_MUTEX_UNLOCK();
}

bool uartSetRxTimeout(uart_t *uart, uint8_t numSymbTimeout) {
  if (uart == NULL) {
    return false;
  }
  uint16_t maxRXTimeout = uart_get_max_rx_timeout(uart->num);
  if (numSymbTimeout > maxRXTimeout) {
    log_e("Invalid RX Timeout value, its limit is %d", maxRXTimeout);
    return false;
  }
  UART_MUTEX_LOCK();
  bool retCode = (ESP_OK == uart_set_rx_timeout(uart->num, numSymbTimeout));
  UART_MUTEX_UNLOCK();
  return retCode;
}

bool uartSetRxFIFOFull(uart_t *uart, uint8_t numBytesFIFOFull) {
  if (uart == NULL) {
    return false;
  }
  uint8_t rxfifo_full_thrhd = numBytesFIFOFull >= UART_HW_FIFO_LEN(uart->num) ? UART_HW_FIFO_LEN(uart->num) - 6 : numBytesFIFOFull;
  UART_MUTEX_LOCK();
  bool retCode = (ESP_OK == uart_set_rx_full_threshold(uart->num, rxfifo_full_thrhd));
  if (retCode) {
    uart->_rxfifo_full_thrhd = rxfifo_full_thrhd;
    if (rxfifo_full_thrhd != numBytesFIFOFull) {
      log_w("The RX FIFO Full value for UART%d was set to %d instead of %d", uart->num, rxfifo_full_thrhd, numBytesFIFOFull);
    }
    log_v("UART%d RX FIFO Full value set to %d from a requested value of %d", uart->num, rxfifo_full_thrhd, numBytesFIFOFull);
  } else {
    log_e("UART%d failed to set RX FIFO Full value to %d", uart->num, numBytesFIFOFull);
  }
  UART_MUTEX_UNLOCK();
  return retCode;
}

void uartEnd(uint8_t uart_num) {
  if (uart_num >= SOC_UART_NUM) {
    log_e("Serial number is invalid, please use number from 0 to %u", SOC_UART_NUM - 1);
    return;
  }
  // get UART information
  uart_t *uart = &_uart_bus_array[uart_num];

  UART_MUTEX_LOCK();
  _uartDetachPins(uart_num, uart->_rxPin, uart->_txPin, uart->_ctsPin, uart->_rtsPin);
  if (uart_is_driver_installed(uart_num)) {
    uart_driver_delete(uart_num);
  }
  UART_MUTEX_UNLOCK();
}

void uartSetRxInvert(uart_t *uart, bool invert) {
  if (uart == NULL) {
    return;
  }
#if CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5
  // POTENTIAL ISSUE :: original code only set/reset rxd_inv bit
  // IDF or LL set/reset the whole inv_mask!
  // if (invert)
  //     ESP_ERROR_CHECK(uart_set_line_inverse(uart->num, UART_SIGNAL_RXD_INV));
  // else
  //     ESP_ERROR_CHECK(uart_set_line_inverse(uart->num, UART_SIGNAL_INV_DISABLE));
  log_e("uartSetRxInvert is not supported in ESP32C6, ESP32H2 and ESP32P4");
#else
  // this implementation is better over IDF API because it only affects RXD
  // this is supported in ESP32, ESP32-S2 and ESP32-C3
  uart_dev_t *hw = UART_LL_GET_HW(uart->num);
  if (invert) {
    hw->conf0.rxd_inv = 1;
  } else {
    hw->conf0.rxd_inv = 0;
  }
#endif
}

uint32_t uartAvailable(uart_t *uart) {

  if (uart == NULL) {
    return 0;
  }

  UART_MUTEX_LOCK();
  size_t available;
  uart_get_buffered_data_len(uart->num, &available);
  if (uart->has_peek) {
    available++;
  }
  UART_MUTEX_UNLOCK();
  return available;
}

uint32_t uartAvailableForWrite(uart_t *uart) {
  if (uart == NULL) {
    return 0;
  }
  UART_MUTEX_LOCK();
  uint32_t available = uart_ll_get_txfifo_len(UART_LL_GET_HW(uart->num));
  size_t txRingBufferAvailable = 0;
  if (ESP_OK == uart_get_tx_buffer_free_size(uart->num, &txRingBufferAvailable)) {
    available = txRingBufferAvailable == 0 ? available : txRingBufferAvailable;
  }
  UART_MUTEX_UNLOCK();
  return available;
}

size_t uartReadBytes(uart_t *uart, uint8_t *buffer, size_t size, uint32_t timeout_ms) {
  if (uart == NULL || size == 0 || buffer == NULL) {
    return 0;
  }

  size_t bytes_read = 0;

  UART_MUTEX_LOCK();

  if (uart->has_peek) {
    uart->has_peek = false;
    *buffer++ = uart->peek_byte;
    size--;
    bytes_read = 1;
  }

  if (size > 0) {
    int len = uart_read_bytes(uart->num, buffer, size, pdMS_TO_TICKS(timeout_ms));
    if (len < 0) {
      len = 0;  // error reading UART
    }
    bytes_read += len;
  }

  UART_MUTEX_UNLOCK();
  return bytes_read;
}

// DEPRECATED but the original code will be kepts here as future reference when a final solution
// to the UART driver is defined in the use case of reading byte by byte from UART.
uint8_t uartRead(uart_t *uart) {
  if (uart == NULL) {
    return 0;
  }
  uint8_t c = 0;

  UART_MUTEX_LOCK();

  if (uart->has_peek) {
    uart->has_peek = false;
    c = uart->peek_byte;
  } else {

    int len = uart_read_bytes(uart->num, &c, 1, 20 / portTICK_PERIOD_MS);
    if (len <= 0) {  // includes negative return from IDF in case of error
      c = 0;
    }
  }
  UART_MUTEX_UNLOCK();
  return c;
}

uint8_t uartPeek(uart_t *uart) {
  if (uart == NULL) {
    return 0;
  }
  uint8_t c = 0;

  UART_MUTEX_LOCK();

  if (uart->has_peek) {
    c = uart->peek_byte;
  } else {
    int len = uart_read_bytes(uart->num, &c, 1, 20 / portTICK_PERIOD_MS);
    if (len <= 0) {  // includes negative return from IDF in case of error
      c = 0;
    } else {
      uart->has_peek = true;
      uart->peek_byte = c;
    }
  }
  UART_MUTEX_UNLOCK();
  return c;
}

void uartWrite(uart_t *uart, uint8_t c) {
  if (uart == NULL) {
    return;
  }
  UART_MUTEX_LOCK();
  uart_write_bytes(uart->num, &c, 1);
  UART_MUTEX_UNLOCK();
}

void uartWriteBuf(uart_t *uart, const uint8_t *data, size_t len) {
  if (uart == NULL || data == NULL || !len) {
    return;
  }

  UART_MUTEX_LOCK();
  uart_write_bytes(uart->num, data, len);
  UART_MUTEX_UNLOCK();
}

void uartFlush(uart_t *uart) {
  uartFlushTxOnly(uart, true);
}

void uartFlushTxOnly(uart_t *uart, bool txOnly) {
  if (uart == NULL) {
    return;
  }

  UART_MUTEX_LOCK();
  while (!uart_ll_is_tx_idle(UART_LL_GET_HW(uart->num)));

  if (!txOnly) {
    ESP_ERROR_CHECK(uart_flush_input(uart->num));
  }
  UART_MUTEX_UNLOCK();
}

bool uartSetBaudRate(uart_t *uart, uint32_t baud_rate) {
  if (uart == NULL) {
    return false;
  }
  bool retCode = true;
  soc_module_clk_t newClkSrc = UART_SCLK_DEFAULT;
  int8_t previousClkSrc = uart->_uart_clock_source;
#if SOC_UART_LP_NUM >= 1
  if (uart->num >= SOC_UART_HP_NUM) {  // it is a LP UART NUM
    if (uart->_uart_clock_source > 0) {
      newClkSrc = (soc_periph_lp_uart_clk_src_t)uart->_uart_clock_source;  // use user defined LP UART clock
      log_v("Setting UART%d to user defined LP clock source (%d) ", uart->num, newClkSrc);
    } else {
      newClkSrc = LP_UART_SCLK_DEFAULT;  // use default LP clock
      log_v("Setting UART%d to Default LP clock source", uart->num);
    }
  } else
#endif  // SOC_UART_LP_NUM >= 1
  {
    if (uart->_uart_clock_source >= 0) {
      newClkSrc = (soc_module_clk_t)uart->_uart_clock_source;  // use user defined HP UART clock
      log_v("Setting UART%d to use HP clock source (%d) ", uart->num, newClkSrc);
    } else {
      // there is an issue when returning from light sleep with the C6 and H2: the uart baud rate is not restored
      // therefore, uart clock source will set to XTAL for all SoC that support it. This fix solves the C6|H2 issue.
#if SOC_UART_SUPPORT_XTAL_CLK
      newClkSrc = UART_SCLK_XTAL;  // valid for C2, S3, C3, C6, H2 and P4
      log_v("Setting UART%d to use XTAL clock", uart->num);
#elif SOC_UART_SUPPORT_REF_TICK
      if (baud_rate <= REF_TICK_BAUDRATE_LIMIT) {
        newClkSrc = UART_SCLK_REF_TICK;  // valid for ESP32, S2 - MAX supported baud rate is 250 Kbps
        log_v("Setting UART%d to use REF_TICK clock", uart->num);
      } else {
        newClkSrc = UART_SCLK_APB;  // baudrate may change with the APB Frequency!
        log_v("Setting UART%d to use APB clock", uart->num);
      }
#else
      // Default CLK Source: CLK_APB for ESP32|S2|S3|C3 -- CLK_PLL_F40M for C2 -- CLK_PLL_F48M for H2 -- CLK_PLL_F80M for C6|P4
      // using newClkSrc = UART_SCLK_DEFAULT as defined in the variable declaration
      log_v("Setting UART%d to use DEFAULT clock", uart->num);
#endif  // SOC_UART_SUPPORT_XTAL_CLK
    }
  }
  UART_MUTEX_LOCK();
  // if necessary, set the correct UART Clock Source before changing the baudrate
  if (previousClkSrc < 0 || previousClkSrc != newClkSrc) {
    HP_UART_SRC_CLK_ATOMIC() {
      uart_ll_set_sclk(UART_LL_GET_HW(uart->num), newClkSrc);
    }
    uart->_uart_clock_source = newClkSrc;
  }
  if (uart_set_baudrate(uart->num, baud_rate) == ESP_OK) {
    log_v("Setting UART%d baud rate to %ld.", uart->num, baud_rate);
    uart->_baudrate = baud_rate;
  } else {
    retCode = false;
    log_e("Setting UART%d baud rate to %ld has failed.", uart->num, baud_rate);
  }
  UART_MUTEX_UNLOCK();
  return retCode;
}

uint32_t uartGetBaudRate(uart_t *uart) {
  uint32_t baud_rate = 0;

  if (uart == NULL) {
    return 0;
  }

  UART_MUTEX_LOCK();
  if (uart_get_baudrate(uart->num, &baud_rate) != ESP_OK) {
    log_e("Getting UART%d baud rate has failed.", uart->num);
    baud_rate = (uint32_t)-1;  // return value when failed
  }
  UART_MUTEX_UNLOCK();
  return baud_rate;
}

static void ARDUINO_ISR_ATTR uart0_write_char(char c) {
  while (uart_ll_get_txfifo_len(&UART0) == 0);
  uart_ll_write_txfifo(&UART0, (const uint8_t *)&c, 1);
}

#if SOC_UART_HP_NUM > 1
static void ARDUINO_ISR_ATTR uart1_write_char(char c) {
  while (uart_ll_get_txfifo_len(&UART1) == 0);
  uart_ll_write_txfifo(&UART1, (const uint8_t *)&c, 1);
}
#endif

#if SOC_UART_HP_NUM > 2
static void ARDUINO_ISR_ATTR uart2_write_char(char c) {
  while (uart_ll_get_txfifo_len(&UART2) == 0);
  uart_ll_write_txfifo(&UART2, (const uint8_t *)&c, 1);
}
#endif

#if SOC_UART_HP_NUM > 3
static void ARDUINO_ISR_ATTR uart3_write_char(char c) {
  while (uart_ll_get_txfifo_len(&UART3) == 0);
  uart_ll_write_txfifo(&UART3, (const uint8_t *)&c, 1);
}
#endif

#if SOC_UART_HP_NUM > 4
static void ARDUINO_ISR_ATTR uart4_write_char(char c) {
  while (uart_ll_get_txfifo_len(&UART4) == 0);
  uart_ll_write_txfifo(&UART4, (const uint8_t *)&c, 1);
}
#endif

void uart_install_putc() {
  switch (s_uart_debug_nr) {
    case 0: ets_install_putc1((void (*)(char)) & uart0_write_char); break;
#if SOC_UART_HP_NUM > 1
    case 1: ets_install_putc1((void (*)(char)) & uart1_write_char); break;
#endif
#if SOC_UART_HP_NUM > 2
    case 2: ets_install_putc1((void (*)(char)) & uart2_write_char); break;
#endif
#if SOC_UART_HP_NUM > 3
    case 3: ets_install_putc1((void (*)(char)) & uart3_write_char); break;
#endif
#if SOC_UART_HP_NUM > 4
    case 4: ets_install_putc1((void (*)(char)) & uart4_write_char); break;
#endif
    default: ets_install_putc1(NULL); break;
  }
  ets_install_putc2(NULL);
}

// Routines that take care of UART mode in the HardwareSerial Class code
// used to set UART_MODE_RS485_HALF_DUPLEX auto RTS for TXD for ESP32 chips
bool uartSetMode(uart_t *uart, uart_mode_t mode) {
  if (uart == NULL || uart->num >= SOC_UART_NUM) {
    return false;
  }

  UART_MUTEX_LOCK();
  bool retCode = (ESP_OK == uart_set_mode(uart->num, mode));
  UART_MUTEX_UNLOCK();
  return retCode;
}

// this function will set the uart clock source
// it must be called before uartBegin(), otherwise it won't change any thing.
bool uartSetClockSource(uint8_t uartNum, uart_sclk_t clkSrc) {
  if (uartNum >= SOC_UART_NUM) {
    log_e("UART%d is invalid. This device has %d UARTs, from 0 to %d.", uartNum, SOC_UART_NUM, SOC_UART_NUM - 1);
    return false;
  }
  uart_t *uart = &_uart_bus_array[uartNum];
#if SOC_UART_LP_NUM >= 1
  if (uart->num >= SOC_UART_HP_NUM) {
    switch (clkSrc) {
      case UART_SCLK_XTAL: uart->_uart_clock_source = LP_UART_SCLK_XTAL_D2; break;
#if CONFIG_IDF_TARGET_ESP32C5
      case UART_SCLK_RTC: uart->_uart_clock_source = LP_UART_SCLK_RC_FAST; break;
#else
      case UART_SCLK_RTC: uart->_uart_clock_source = LP_UART_SCLK_LP_FAST; break;
#endif
      case UART_SCLK_DEFAULT:
      default:                uart->_uart_clock_source = LP_UART_SCLK_DEFAULT;
    }
  } else
#endif
  {
    uart->_uart_clock_source = clkSrc;
  }
  //log_i("UART%d set clock source to %d", uart->num, uart->_uart_clock_source);
  return true;
}

void uartSetDebug(uart_t *uart) {
  // LP UART is not supported for debug
  if (uart == NULL || uart->num >= SOC_UART_HP_NUM) {
    s_uart_debug_nr = -1;
  } else {
    s_uart_debug_nr = uart->num;
  }
  uart_install_putc();
}

int uartGetDebug() {
  return s_uart_debug_nr;
}

int log_printfv(const char *format, va_list arg) {
  static char loc_buf[64];
  char *temp = loc_buf;
  uint32_t len;
  va_list copy;
  va_copy(copy, arg);
  len = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (len >= sizeof(loc_buf)) {
    temp = (char *)malloc(len + 1);
    if (temp == NULL) {
      return 0;
    }
  }
  /*
// This causes dead locks with logging in specific cases and also with C++ constructors that may send logs
#if !CONFIG_DISABLE_HAL_LOCKS
    if(s_uart_debug_nr != -1 && _uart_bus_array[s_uart_debug_nr].lock){
        xSemaphoreTake(_uart_bus_array[s_uart_debug_nr].lock, portMAX_DELAY);
    }
#endif
*/
  vsnprintf(temp, len + 1, format, arg);
  ets_printf("%s", temp);
  /*
// This causes dead locks with logging and also with constructors that may send logs
#if !CONFIG_DISABLE_HAL_LOCKS
    if(s_uart_debug_nr != -1 && _uart_bus_array[s_uart_debug_nr].lock){
        xSemaphoreGive(_uart_bus_array[s_uart_debug_nr].lock);
    }
#endif
*/
  if (len >= sizeof(loc_buf)) {
    free(temp);
  }
  // flushes TX - make sure that the log message is completely sent.
  if (s_uart_debug_nr != -1) {
    while (!uart_ll_is_tx_idle(UART_LL_GET_HW(s_uart_debug_nr)));
  }
  return len;
}

int log_printf(const char *format, ...) {
  int len;
  va_list arg;
  va_start(arg, format);
  len = log_printfv(format, arg);
  va_end(arg);
  return len;
}

static void log_print_buf_line(const uint8_t *b, size_t len, size_t total_len) {
  for (size_t i = 0; i < len; i++) {
    log_printf("%s0x%02x,", i ? " " : "", b[i]);
  }
  if (total_len > 16) {
    for (size_t i = len; i < 16; i++) {
      log_printf("      ");
    }
    log_printf("    // ");
  } else {
    log_printf(" // ");
  }
  for (size_t i = 0; i < len; i++) {
    log_printf("%c", ((b[i] >= 0x20) && (b[i] < 0x80)) ? b[i] : '.');
  }
  log_printf("\n");
}

void log_print_buf(const uint8_t *b, size_t len) {
  if (!len || !b) {
    return;
  }
  for (size_t i = 0; i < len; i += 16) {
    if (len > 16) {
      log_printf("/* 0x%04X */ ", i);
    }
    log_print_buf_line(b + i, ((len - i) < 16) ? (len - i) : 16, len);
  }
}

/*
 * if enough pulses are detected return the minimum high pulse duration + minimum low pulse duration divided by two.
 * This equals one bit period. If flag is true the function return immediately, otherwise it waits for enough pulses.
 */
unsigned long uartBaudrateDetect(uart_t *uart, bool flg) {
// Baud rate detection only works for ESP32 and ESP32S2
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
  if (uart == NULL) {
    return 0;
  }

  uart_dev_t *hw = UART_LL_GET_HW(uart->num);

  while (hw->rxd_cnt.edge_cnt < 30) {  // UART_PULSE_NUM(uart_num)
    if (flg) {
      return 0;
    }
    ets_delay_us(1000);
  }

  UART_MUTEX_LOCK();
  //log_i("lowpulse_min_cnt = %d hightpulse_min_cnt = %d", hw->lowpulse.min_cnt, hw->highpulse.min_cnt);
  unsigned long ret = ((hw->lowpulse.min_cnt + hw->highpulse.min_cnt) >> 1);
  UART_MUTEX_UNLOCK();

  return ret;
#else
  return 0;
#endif
}

/*
 * To start detection of baud rate with the uart the auto_baud.en bit needs to be cleared and set. The bit period is
 * detected calling uartBadrateDetect(). The raw baudrate is computed using the UART_CLK_FREQ. The raw baudrate is
 * rounded to the closed real baudrate.
 *
 * ESP32-C3 reports wrong baud rate detection as shown below:
 *
 * This will help in a future recall for the C3.
 * Baud Sent:          Baud Read:
 *  300        -->       19536
 * 2400        -->       19536
 * 4800        -->       19536
 * 9600        -->       28818
 * 19200       -->       57678
 * 38400       -->       115440
 * 57600       -->       173535
 * 115200      -->       347826
 * 230400      -->       701754
 *
 *
*/
void uartStartDetectBaudrate(uart_t *uart) {
  if (uart == NULL) {
    return;
  }

// Baud rate detection only works for ESP32 and ESP32S2
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
  uart_dev_t *hw = UART_LL_GET_HW(uart->num);
  hw->auto_baud.glitch_filt = 0x08;
  hw->auto_baud.en = 0;
  hw->auto_baud.en = 1;
#else

  // ESP32-C3 requires further testing
  // Baud rate detection returns wrong values

  log_e("baud rate detection for this SoC is not supported.");
  return;

  // Code bellow for C3 kept for future recall
  //hw->rx_filt.glitch_filt = 0x08;
  //hw->rx_filt.glitch_filt_en = 1;
  //hw->conf0.autobaud_en = 0;
  //hw->conf0.autobaud_en = 1;
#endif
}

unsigned long uartDetectBaudrate(uart_t *uart) {
  if (uart == NULL) {
    return 0;
  }

// Baud rate detection only works for ESP32 and ESP32S2
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2

  static bool uartStateDetectingBaudrate = false;

  if (!uartStateDetectingBaudrate) {
    uartStartDetectBaudrate(uart);
    uartStateDetectingBaudrate = true;
  }

  unsigned long divisor = uartBaudrateDetect(uart, true);
  if (!divisor) {
    return 0;
  }

  uart_dev_t *hw = UART_LL_GET_HW(uart->num);
  hw->auto_baud.en = 0;

  uartStateDetectingBaudrate = false;  // Initialize for the next round

  unsigned long baudrate = getApbFrequency() / divisor;

  //log_i("APB_FREQ = %d\nraw baudrate detected = %d", getApbFrequency(), baudrate);

  static const unsigned long default_rates[] = {300,   600,    1200,   2400,   4800,   9600,   19200,   38400,  57600,
                                                74880, 115200, 230400, 256000, 460800, 921600, 1843200, 3686400};

  size_t i;
  for (i = 1; i < sizeof(default_rates) / sizeof(default_rates[0]) - 1; i++)  // find the nearest real baudrate
  {
    if (baudrate <= default_rates[i]) {
      if (baudrate - default_rates[i - 1] < default_rates[i] - baudrate) {
        i--;
      }
      break;
    }
  }

  return default_rates[i];
#else
  log_e("baud rate detection this SoC is not supported.");
  return 0;
#endif
}

/*
 * These functions are for testing purposes only and can be used in Arduino Sketches.
 * They are utilized in the UART examples and CI.
 */

/*
   This function internally binds defined UARTs TX signal with defined RX pin of any UART (same or different).
   This creates a loop that lets us receive anything we send on the UART without external wires.
*/
void uart_internal_loopback(uint8_t uartNum, int8_t rxPin) {
  // LP UART is not supported for loopback
  if (uartNum >= SOC_UART_HP_NUM || !GPIO_IS_VALID_GPIO(rxPin)) {
    log_e("UART%d is not supported for loopback or RX pin %d is invalid.", uartNum, rxPin);
    return;
  }
#if 0  // leave this code here for future reference and need
  // forces rxPin to use GPIO Matrix and setup the pin to receive UART TX Signal - IDF 5.4.1 Change with uart_release_pin()
  gpio_func_sel((gpio_num_t)rxPin, PIN_FUNC_GPIO);
  gpio_pullup_en((gpio_num_t)rxPin);
  gpio_input_enable((gpio_num_t)rxPin);
  esp_rom_gpio_connect_in_signal(rxPin, uart_periph_signal[uartNum].pins[SOC_UART_RX_PIN_IDX].signal, false);
#endif
  esp_rom_gpio_connect_out_signal(rxPin, uart_periph_signal[uartNum].pins[SOC_UART_TX_PIN_IDX].signal, false, false);
}

/*
    This is intended to generate BREAK in an UART line
*/

// Forces a BREAK in the line based on SERIAL_8N1 configuration at any baud rate
void uart_send_break(uint8_t uartNum) {
  uint32_t currentBaudrate = 0;
  uart_get_baudrate(uartNum, &currentBaudrate);
  // calculates 10 bits of breaks in microseconds for baudrates up to 500mbps
  // This is very sensitive timing... it works fine for SERIAL_8N1
  uint32_t breakTime = (uint32_t)(10.0 * (1000000.0 / currentBaudrate));
  uart_set_line_inverse(uartNum, UART_SIGNAL_TXD_INV);
  esp_rom_delay_us(breakTime);
  uart_set_line_inverse(uartNum, UART_SIGNAL_INV_DISABLE);
}

// Sends a buffer and at the end of the stream, it generates BREAK in the line
int uart_send_msg_with_break(uint8_t uartNum, uint8_t *msg, size_t msgSize) {
  // 12 bits long BREAK for 8N1
  return uart_write_bytes_with_break(uartNum, (const void *)msg, msgSize, 12);
}

// returns the maximum valid uart RX Timeout based on the UART Source Clock and Baudrate
uint16_t uart_get_max_rx_timeout(uint8_t uartNum) {
  if (uartNum >= SOC_UART_NUM) {
    log_e("UART%d is invalid. This device has %d UARTs, from 0 to %d.", uartNum, SOC_UART_NUM, SOC_UART_NUM - 1);
    return (uint16_t)-1;
  }
  uint16_t tout_max_thresh = uart_ll_max_tout_thrd(UART_LL_GET_HW(uartNum));
  uint8_t symbol_len = 1;  // number of bits per symbol including start
  uart_parity_t parity_mode;
  uart_stop_bits_t stop_bit;
  uart_word_length_t data_bit;
  uart_ll_get_data_bit_num(UART_LL_GET_HW(uartNum), &data_bit);
  uart_ll_get_stop_bits(UART_LL_GET_HW(uartNum), &stop_bit);
  uart_ll_get_parity(UART_LL_GET_HW(uartNum), &parity_mode);
  symbol_len += (data_bit < UART_DATA_BITS_MAX) ? (uint8_t)data_bit + 5 : 8;
  symbol_len += (stop_bit > UART_STOP_BITS_1) ? 2 : 1;
  symbol_len += (parity_mode > UART_PARITY_DISABLE) ? 1 : 0;
  return (uint16_t)(tout_max_thresh / symbol_len);
}

#endif /* SOC_UART_SUPPORTED */
