// Copyright 2018 Josh Pieper, jjp@pobox.com.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bldc_servo.h"

#include <cmath>
#include <functional>

#include "mbed.h"
#include "serial_api_hal.h"

#include "PeripheralPins.h"

#include "mjlib/base/assert.h"
#include "mjlib/base/windowed_average.h"

#include "moteus/irq_callback_table.h"
#include "moteus/foc.h"
#include "moteus/math.h"
#include "moteus/stm32f446_async_uart.h"
#include "moteus/stm32_serial.h"

namespace micro = mjlib::micro;

namespace moteus {

namespace {

float Limit(float a, float min, float max) {
  if (a < min) { return min; }
  if (a > max) { return max; }
  return a;
}

template <typename Array>
int MapConfig(const Array& array, int value) {
  static_assert(sizeof(array) > 0);
  int result = 0;
  for (const auto& item : array) {
    if (value <= item) { return result; }
    result++;
  }
  // Never return past the end.
  return result - 1;
}

constexpr float kRateHz = 40000.0;
constexpr int kCalibrateCount = 256;

// The maximum amount the absolute encoder can change in one cycle
// without triggering a fault.  Measured relative to 32767
constexpr int16_t kMaxPositionDelta = 1000;

// mbed seems to configure the Timer clock input to 90MHz.  We want
// 80kHz up/down rate for 40kHz freqency, so:
constexpr uint32_t kPwmCounts = 90000000 / 80000;

IRQn_Type FindUpdateIrq(TIM_TypeDef* timer) {
  if (timer == TIM1) {
    return TIM1_UP_TIM10_IRQn;
  } else if (timer == TIM2) {
    return TIM2_IRQn;
  } else if (timer == TIM3) {
    return TIM3_IRQn;
  } else if (timer == TIM4) {
    return TIM4_IRQn;
  } else if (timer == TIM8) {
    return TIM8_UP_TIM13_IRQn;
  } else {
    MJ_ASSERT(false);
  }
  return TIM1_UP_TIM10_IRQn;
}

volatile uint32_t* FindCcr(TIM_TypeDef* timer, PinName pin) {
  const auto function = pinmap_function(pin, PinMap_PWM);

  const auto inverted = STM_PIN_INVERTED(function);
  MJ_ASSERT(!inverted);

  const auto channel = STM_PIN_CHANNEL(function);

  switch (channel) {
    case 1: { return &timer->CCR1; }
    case 2: { return &timer->CCR2; }
    case 3: { return &timer->CCR3; }
    case 4: { return &timer->CCR4; }
  }
  MJ_ASSERT(false);
  return nullptr;
}

uint32_t FindSqr(PinName pin) {
  const auto function = pinmap_function(pin, PinMap_ADC);

  const auto channel = STM_PIN_CHANNEL(function);
  return channel;
}
}

class BldcServo::Impl {
 public:
  Impl(micro::PersistentConfig* persistent_config,
       micro::TelemetryManager* telemetry_manager,
       PositionSensor* position_sensor,
       MotorDriver* motor_driver,
       const Options& options)
      : options_(options),
        position_sensor_(position_sensor),
        motor_driver_(motor_driver),
        pwm1_(options.pwm1),
        pwm2_(options.pwm2),
        pwm3_(options.pwm3),
        current1_(options.current1),
        current2_(options.current2),
        vsense_(options.vsense),
        debug_out_(options.debug_out),
        debug_serial_([&]() {
            Stm32Serial::Options d_options;
            d_options.tx = options.debug_uart_out;
            d_options.baud_rate = 5000000;
            return d_options;
          }()) {

    persistent_config->Register("servo", &config_,
                                std::bind(&Impl::UpdateConfig, this));
    telemetry_manager->Register("servo_stats", &status_);
    telemetry_manager->Register("servo_cmd", &telemetry_data_);
    telemetry_manager->Register("servo_control", &control_);

    MJ_ASSERT(!g_impl_);
    g_impl_ = this;

    ConfigureADC();
    ConfigureTimer();

    if (options_.debug_uart_out != NC) {
      const auto uart = pinmap_peripheral(
          options.debug_uart_out, PinMap_UART_TX);
      debug_uart_ = reinterpret_cast<USART_TypeDef*>(uart);
      auto dma_pair = Stm32F446AsyncUart::MakeDma(
          static_cast<UARTName>(uart));
      debug_uart_dma_tx_ = dma_pair.tx;

      debug_uart_dma_tx_.stream->PAR =
          reinterpret_cast<uint32_t>(&(debug_uart_->DR));
      debug_uart_dma_tx_.stream->CR =
          debug_uart_dma_tx_.channel |
          DMA_SxCR_MINC |
          DMA_MEMORY_TO_PERIPH;
    }
  }

  ~Impl() {
    g_impl_ = nullptr;
  }

  void Command(const CommandData& data) {
    MJ_ASSERT(data.mode != kFault);
    MJ_ASSERT(data.mode != kEnabling);
    MJ_ASSERT(data.mode != kCalibrating);
    MJ_ASSERT(data.mode != kCalibrationComplete);

    // Actually setting values will happen in the interrupt routine,
    // so we need to update this atomically.
    CommandData* next = next_data_;
    *next = data;

    telemetry_data_ = data;

    std::swap(current_data_, next_data_);
  }

  Status status() const { return status_; }

  void UpdateConfig() {
  }

  void PollMillisecond() {
    volatile Mode* mode_volatile = &status_.mode;
    Mode mode = *mode_volatile;
    if (mode == kEnabling) {
      motor_driver_->Enable(true);
      *mode_volatile = kCalibrating;
    }
  }

 private:
  void ConfigureTimer() {
    const auto pwm1_timer = pinmap_peripheral(options_.pwm1, PinMap_PWM);
    const auto pwm2_timer = pinmap_peripheral(options_.pwm2, PinMap_PWM);
    const auto pwm3_timer = pinmap_peripheral(options_.pwm3, PinMap_PWM);

    // All three must be the same and be valid.
    MJ_ASSERT(pwm1_timer != 0 &&
                pwm1_timer == pwm2_timer &&
                pwm2_timer == pwm3_timer);
    timer_ = reinterpret_cast<TIM_TypeDef*>(pwm1_timer);


    pwm1_ccr_ = FindCcr(timer_, options_.pwm1);
    pwm2_ccr_ = FindCcr(timer_, options_.pwm2);
    pwm3_ccr_ = FindCcr(timer_, options_.pwm3);


    // Enable the update interrupt.
    timer_->DIER = TIM_DIER_UIE;

    // Enable the update interrupt.
    timer_->CR1 =
        // Center-aligned mode 2.  The counter counts up and down
        // alternatively.  Output compare interrupt flags of channels
        // configured in output are set only when the counter is
        // counting up.
        (2 << TIM_CR1_CMS_Pos) |

        // ARR register is buffered.
        TIM_CR1_ARPE;

    // Update once per up/down of the counter.
    timer_->RCR |= 0x01;

    // Set up PWM.

    timer_->PSC = 0; // No prescaler.
    timer_->ARR = kPwmCounts;

    // NOTE: We don't use IrqCallbackTable here because we need the
    // absolute minimum latency possible.
    const auto irqn = FindUpdateIrq(timer_);
    NVIC_SetVector(irqn, reinterpret_cast<uint32_t>(&Impl::GlobalInterrupt));
    HAL_NVIC_SetPriority(irqn, 0, 0);
    NVIC_EnableIRQ(irqn);

    // Reinitialize the counter and update all registers.
    timer_->EGR |= TIM_EGR_UG;

    // Finally, enable the timer.
    timer_->CR1 |= TIM_CR1_CEN;
  }

  void ConfigureADC() {
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_ADC2_CLK_ENABLE();
    __HAL_RCC_ADC3_CLK_ENABLE();

    // Triple mode: Regular simultaneous mode only.
    ADC->CCR = (0x16 << ADC_CCR_MULTI_Pos);

    // Turn on all the converters.
    ADC1->CR2 = ADC_CR2_ADON;
    ADC2->CR2 = ADC_CR2_ADON;
    ADC3->CR2 = ADC_CR2_ADON;

    // We rely on the AnalogIn members to configure the pins as
    // inputs, however they won't
    ADC1->SQR3 = FindSqr(options_.current1);
    ADC2->SQR3 = FindSqr(options_.current2);
    ADC3->SQR3 = FindSqr(options_.vsense);

    MJ_ASSERT(reinterpret_cast<uint32_t>(ADC1) ==
                pinmap_peripheral(options_.current1, PinMap_ADC));
    MJ_ASSERT(reinterpret_cast<uint32_t>(ADC2) ==
                pinmap_peripheral(options_.current2, PinMap_ADC));
    MJ_ASSERT(reinterpret_cast<uint32_t>(ADC3) ==
                pinmap_peripheral(options_.vsense, PinMap_ADC));

    constexpr uint16_t kCycleMap[] = {
      3, 15, 28, 56, 84, 112, 144, 480,
    };

    // Set sample times to the same thing across the board
    const uint32_t cycles = MapConfig(kCycleMap, config_.adc_cycles);
    const uint32_t all_cycles =
        (cycles << 0) |
        (cycles << 3) |
        (cycles << 6) |
        (cycles << 9) |
        (cycles << 12) |
        (cycles << 15) |
        (cycles << 18) |
        (cycles << 21) |
        (cycles << 24);
    ADC1->SMPR1 = all_cycles;
    ADC1->SMPR2 = all_cycles;
    ADC2->SMPR1 = all_cycles;
    ADC2->SMPR2 = all_cycles;
    ADC3->SMPR1 = all_cycles;
    ADC3->SMPR2 = all_cycles;
  }

  // CALLED IN INTERRUPT CONTEXT.
  static void GlobalInterrupt() {
    g_impl_->ISR_HandleTimer();
  }

  // CALLED IN INTERRUPT CONTEXT.
  void ISR_HandleTimer() {

    if ((timer_->SR & TIM_SR_UIF) &&
        (timer_->CR1 & TIM_CR1_DIR)) {
      ISR_DoTimer();
    }

    // Reset the status register.
    timer_->SR = 0x00;
  }

  void ISR_DoTimer() {
    debug_out_ = 1;

    // No matter what mode we are in, always sample our ADC and
    // position sensors.
    ISR_DoSense();

    SinCos sin_cos{status_.electrical_theta};

    ISR_CalculateCurrentState(sin_cos);
    ISR_DoControl(sin_cos);

    ISR_MaybeEmitDebug();
  }

  void ISR_DoSense() {
    uint32_t adc1 = 0;
    uint32_t adc2 = 0;
    uint32_t adc3 = 0;

    for (uint16_t i = 0; i < config_.adc_sample_count; i++) {
      // Start conversion.
      ADC1->CR2 |= ADC_CR2_SWSTART;

      while ((ADC1->SR & ADC_SR_EOC) == 0);
      adc1 += ADC1->DR;
      adc2 += ADC2->DR;
      adc3 += ADC3->DR;
    }

    debug_out_ = 0;

    status_.adc1_raw = adc1 / config_.adc_sample_count;
    status_.adc2_raw = adc2 / config_.adc_sample_count;
    status_.adc3_raw = adc3 / config_.adc_sample_count;

    // We are now out of the most time critical portion of the ISR,
    // although it is still all pretty time critical since it runs
    // at 40kHz.  But time spent until now actually limits the
    // maximum duty cycle we can achieve, whereas time spent below
    // just eats cycles the rest of the code could be using.

    // Sample the position.
    const uint16_t old_position_raw = status_.position_raw;
    status_.position_raw = position_sensor_->Sample();

    status_.electrical_theta =
        k2Pi * ::fmodf(
            (static_cast<float>(status_.position_raw) / 65536.0f *
             (config_.motor_poles / 2.0f)) - config_.motor_offset, 1.0f);
    const int16_t delta_position =
        static_cast<int16_t>(status_.position_raw - old_position_raw);
    if (status_.mode != kStopped &&
        std::abs(delta_position) > kMaxPositionDelta) {
      // We probably had an error when reading the position.  We must fault.
      status_.mode = kFault;
      status_.fault = errc::kEncoderFault;
    }

    status_.unwrapped_position_raw += delta_position;
    velocity_filter_.Add(delta_position * config_.unwrapped_position_scale *
                         (1.0f / 65536.0f) * kRateHz);
    status_.velocity = velocity_filter_.average();

    status_.unwrapped_position =
        status_.unwrapped_position_raw * config_.unwrapped_position_scale *
        (1.0f / 65536.0f);
  }

  void ISR_MaybeEmitDebug() {
    if (debug_uart_ == nullptr) { return; }

    debug_buf_[0] = 0x5a;
    debug_buf_[1] = static_cast<uint8_t>(255.0f * status_.electrical_theta / k2Pi);
    debug_buf_[2] = static_cast<int8_t>(control_.i_d_A * 2.0f);
    int16_t measured_d_a = static_cast<int16_t>(status_.d_A * 500.0f);
    std::memcpy(&debug_buf_[3], &measured_d_a, sizeof(measured_d_a));
    int16_t measured_pid_d_p = 32767.0f * status_.pid_d.p / 12.0f;
    std::memcpy(&debug_buf_[5], &measured_pid_d_p, sizeof(measured_pid_d_p));
    int16_t measured_pid_d_i = 32767.0f * status_.pid_d.integral / 12.0f;
    std::memcpy(&debug_buf_[7], &measured_pid_d_i, sizeof(measured_pid_d_i));
    int16_t control_d_V = 32767.0f * control_.d_V / 12.0f;
    std::memcpy(&debug_buf_[9], &control_d_V, sizeof(control_d_V));

    debug_buf_[11] = static_cast<int8_t>(127.0f * status_.velocity / 10.0f);

    *debug_uart_dma_tx_.status_clear |= debug_uart_dma_tx_.all_status();
    debug_uart_dma_tx_.stream->NDTR = sizeof(debug_buf_);
    debug_uart_dma_tx_.stream->M0AR = reinterpret_cast<uint32_t>(&debug_buf_);
    debug_uart_dma_tx_.stream->CR |= DMA_SxCR_EN;

    debug_uart_->CR3 |= USART_CR3_DMAT;
  }

  // This is called from the ISR.
  void ISR_CalculateCurrentState(const SinCos& sin_cos) {
    status_.cur1_A = (status_.adc1_raw - status_.adc1_offset) * config_.i_scale_A;
    status_.cur2_A = (status_.adc2_raw - status_.adc2_offset) * config_.i_scale_A;
    status_.bus_V = status_.adc3_raw * config_.v_scale_V;

    DqTransform dq{sin_cos,
          status_.cur1_A,
          0.0f - (status_.cur1_A + status_.cur2_A),
          status_.cur2_A
          };
    status_.d_A = dq.d;
    status_.q_A = dq.q;
  }

  void ISR_MaybeChangeMode(CommandData* data) {
    // We are requesting a different mode than we are in now.  Do our
    // best to advance if possible.
    switch (data->mode) {
      case kNumModes:
      case kFault:
      case kCalibrating:
      case kCalibrationComplete: {
        // These should not be possible.
        MJ_ASSERT(false);
        return;
      }
      case kStopped: {
        // It is always valid to enter stopped mode.
        status_.mode = kStopped;
        return;
      }
      case kEnabling: {
        // We can never change out from enabling in ISR context.
        return;
      }
      case kPwm:
      case kVoltage:
      case kVoltageFoc:
      case kCurrent:
      case kPosition: {
        switch (status_.mode) {
          case kNumModes: {
            MJ_ASSERT(false);
            return;
          }
          case kFault: {
            // We cannot leave a fault state directly into an active state.
            return;
          }
          case kStopped: {
            // From a stopped state, we first have to enter the
            // calibrating state.
            ISR_StartCalibrating();
            return;
          }
          case kEnabling:
          case kCalibrating: {
            // We can only leave this state when calibration is
            // complete.
            return;
          }
          case kCalibrationComplete:
          case kPwm:
          case kVoltage:
          case kVoltageFoc:
          case kCurrent:
          case kPosition: {
            // Yep, we can do this.
            status_.mode = data->mode;
            return;
          }
        }
      }
    }
  }

  void ISR_StartCalibrating() {
    status_.mode = kEnabling;

    // The main context will set our state to kCalibrating when the
    // motor driver is fully enabled.

    (*pwm1_ccr_) = 0;
    (*pwm2_ccr_) = 0;
    (*pwm3_ccr_) = 0;

    // Power should already be false for any state we could possibly
    // be in, but lets just be certain.
    motor_driver_->Power(false);

    calibrate_adc1_ = 0;
    calibrate_adc2_ = 0;
    calibrate_count_ = 0;
  }

  void ISR_ClearPid() {
    const bool current_pid_active = [&]() {
      switch (status_.mode) {
        case kNumModes:
        case kStopped:
        case kFault:
        case kEnabling:
        case kCalibrating:
        case kCalibrationComplete:
        case kPwm:
        case kVoltage:
        case kVoltageFoc:
          return false;
        case kCurrent:
        case kPosition:
          return true;
      }
      return false;
    }();

    if (!current_pid_active) {
      status_.pid_d = {};
      status_.pid_q = {};
    }

    const bool position_pid_active = [&]() {
      switch (status_.mode) {
        case kNumModes:
        case kStopped:
        case kFault:
        case kEnabling:
        case kCalibrating:
        case kCalibrationComplete:
        case kPwm:
        case kVoltage:
        case kVoltageFoc:
        case kCurrent:
          return false;
        case kPosition:
          return true;
      }
      return false;
    }();

    if (!position_pid_active) {
      status_.pid_position = {};
    }
  }

  void ISR_DoControl(const SinCos& sin_cos) {
    // current_data_ is volatile, so read it out now, and operate on
    // the pointer for the rest of the routine.
    CommandData* data = current_data_;

    control_ = {};

    if (data->set_position) {
      status_.unwrapped_position_raw =
          static_cast<int32_t>(*data->set_position * 65536.0f);
      data->set_position = {};
    }

    // See if we need to update our current mode.
    if (data->mode != status_.mode) {
      ISR_MaybeChangeMode(data);

      if (status_.mode != kStopped) {
        if (motor_driver_->fault()) {
          status_.mode = kFault;
          status_.fault = errc::kMotorDriverFault;
          return;
        }
        if (status_.bus_V > config_.max_voltage) {
          status_.mode = kFault;
          status_.fault = errc::kOverVoltage;
          return;
        }
      }
    }

    // Ensure unused PID controllers have zerod state.
    ISR_ClearPid();

    if (status_.mode != kFault) {
      status_.fault = errc::kSuccess;
    }

    switch (status_.mode) {
      case kNumModes:
      case kStopped: {
        ISR_DoStopped();
        break;
      }
      case kFault: {
        ISR_DoFault();
        break;
      }
      case kEnabling: {
        break;
      }
      case kCalibrating: {
        ISR_DoCalibrating();
        break;
      }
      case kCalibrationComplete: {
        break;
      }
      case kPwm: {
        ISR_DoPwmControl(data->pwm);
        break;
      }
      case kVoltage: {
        ISR_DoVoltageControl(data->phase_v);
        break;
      }
      case kVoltageFoc: {
        ISR_DoVoltageFOC(data->theta, data->voltage);
        break;
      }
      case kCurrent: {
        ISR_DoCurrent(sin_cos, data->i_d_A, data->i_q_A);
        break;
      }
      case kPosition: {
        ISR_DoPosition(sin_cos, data->position,
                       data->velocity, data->max_current);
        break;
      }
    }
  }

  void ISR_DoStopped() {
    motor_driver_->Enable(false);
    motor_driver_->Power(false);
    *pwm1_ccr_ = 0;
    *pwm2_ccr_ = 0;
    *pwm3_ccr_ = 0;
  }

  void ISR_DoFault() {
    motor_driver_->Power(false);
    *pwm1_ccr_ = 0;
    *pwm2_ccr_ = 0;
    *pwm3_ccr_ = 0;
  }

  void ISR_DoCalibrating() {
    calibrate_adc1_ += status_.adc1_raw;
    calibrate_adc2_ += status_.adc2_raw;
    calibrate_count_++;

    if (calibrate_count_ < kCalibrateCount) {
      return;
    }

    const uint16_t new_adc1_offset = calibrate_adc1_ / kCalibrateCount;
    const uint16_t new_adc2_offset = calibrate_adc2_ / kCalibrateCount;

    if (std::abs(static_cast<int>(new_adc1_offset) - 2048) > 200 ||
        std::abs(static_cast<int>(new_adc2_offset) - 2048) > 200) {
      // Error calibrating.  Just fault out.
      status_.mode = kFault;
      status_.fault = errc::kCalibrationFault;
      return;
    }

    status_.adc1_offset = new_adc1_offset;
    status_.adc2_offset = new_adc2_offset;
    status_.mode = kCalibrationComplete;
  }

  void ISR_DoPwmControl(const Vec3& pwm) {
    control_.pwm.a = LimitPwm(pwm.a);
    control_.pwm.b = LimitPwm(pwm.b);
    control_.pwm.c = LimitPwm(pwm.c);

    *pwm1_ccr_ = static_cast<uint16_t>(control_.pwm.a * kPwmCounts);
    *pwm3_ccr_ = static_cast<uint16_t>(control_.pwm.b * kPwmCounts);
    *pwm2_ccr_ = static_cast<uint16_t>(control_.pwm.c * kPwmCounts);

    motor_driver_->Power(true);
  }

  void ISR_DoVoltageControl(const Vec3& voltage) {
    control_.voltage = voltage;

    auto voltage_to_pwm = [this](float v) {
      return 0.5f + 2.0f * v / status_.bus_V;
    };

    ISR_DoPwmControl(Vec3{
        voltage_to_pwm(voltage.a),
            voltage_to_pwm(voltage.b),
            voltage_to_pwm(voltage.c)});
  }

  void ISR_DoVoltageFOC(float theta, float voltage) {
    SinCos sc(theta);
    InverseDqTransform idt(sc, 0, voltage);
    ISR_DoVoltageControl(Vec3{idt.a, idt.b, idt.c});
  }

  void ISR_DoCurrent(const SinCos& sin_cos, float i_d_A, float i_q_A) {
    control_.i_d_A = i_d_A;
    control_.i_q_A = i_q_A;

    control_.d_V =
        (config_.feedforward_scale * (
            i_d_A * config_.motor_resistance -
            status_.velocity * config_.motor_v_per_hz)) +
        pid_d_.Apply(status_.d_A, i_d_A, 0.0f, 0.0f, kRateHz);
    control_.q_V =
        (config_.feedforward_scale * i_q_A * config_.motor_resistance) +
        pid_q_.Apply(status_.q_A, i_q_A, 0.0f, 0.0f, kRateHz);

    InverseDqTransform idt(sin_cos, control_.d_V, control_.q_V);

    ISR_DoVoltageControl(Vec3{idt.a, idt.b, idt.c});
  }

  void ISR_DoPosition(const SinCos& sin_cos, float position, float velocity, float max_current) {
    const float measured_velocity = status_.velocity;

    const float unlimited_d_A =
        pid_position_.Apply(status_.unwrapped_position, position,
                            measured_velocity, velocity,
                            kRateHz);
    const float d_A = Limit(unlimited_d_A, -max_current, max_current);
    MJ_ASSERT(std::abs(d_A) <= max_current);

    ISR_DoCurrent(sin_cos, d_A, 0.0f);
  }

  float LimitPwm(float in) {
    // We can't go full duty cycle or we wouldn't have time to sample
    // the current.
    return Limit(in, 0.1f, 0.9f);
  }

  const Options options_;
  PositionSensor* const position_sensor_;
  MotorDriver* const motor_driver_;

  Config config_;
  TIM_TypeDef* timer_ = nullptr;
  ADC_TypeDef* const adc1_ = ADC1;
  ADC_TypeDef* const adc2_ = ADC2;
  ADC_TypeDef* const adc3_ = ADC3;

  // We create these to initialize our pins as output and PWM mode,
  // but otherwise don't use them.
  PwmOut pwm1_;
  PwmOut pwm2_;
  PwmOut pwm3_;

  volatile uint32_t* pwm1_ccr_ = nullptr;
  volatile uint32_t* pwm2_ccr_ = nullptr;
  volatile uint32_t* pwm3_ccr_ = nullptr;

  AnalogIn current1_;
  AnalogIn current2_;
  AnalogIn vsense_;

  // This is just for debugging.
  DigitalOut debug_out_;

  CommandData data_buffers_[2] = {};

  // CommandData has its data updated to the ISR by first writing the
  // new command into (*next_data_) and then swapping it with
  // current_data_.
  CommandData* volatile current_data_{&data_buffers_[0]};
  CommandData* volatile next_data_{&data_buffers_[1]};

  // This copy of CommandData exists solely for telemetry, and should
  // never be read by an ISR.
  CommandData telemetry_data_;

  // These values should only be modified from within the ISR.
  mjlib::base::WindowedAverage<float, 32> velocity_filter_;
  Status status_;
  Control control_;
  uint32_t calibrate_adc1_ = 0;
  uint32_t calibrate_adc2_ = 0;
  uint16_t calibrate_count_ = 0;

  mjlib::base::PID pid_d_{&config_.pid_dq, &status_.pid_d};
  mjlib::base::PID pid_q_{&config_.pid_dq, &status_.pid_q};
  mjlib::base::PID pid_position_{&config_.pid_position, &status_.pid_position};

  Stm32Serial debug_serial_;
  USART_TypeDef* debug_uart_ = nullptr;
  Stm32F446AsyncUart::Dma debug_uart_dma_tx_;
  char debug_buf_[12] = {};

  static Impl* g_impl_;
};

BldcServo::Impl* BldcServo::Impl::g_impl_ = nullptr;

BldcServo::BldcServo(micro::Pool* pool,
                     micro::PersistentConfig* persistent_config,
                     micro::TelemetryManager* telemetry_manager,
                     PositionSensor* position_sensor,
                     MotorDriver* motor_driver,
                     const Options& options)
    : impl_(pool,
            persistent_config, telemetry_manager,
            position_sensor, motor_driver,
            options) {}
BldcServo::~BldcServo() {}

void BldcServo::PollMillisecond() {
  impl_->PollMillisecond();
}

void BldcServo::Command(const CommandData& data) {
  impl_->Command(data);
}

BldcServo::Status BldcServo::status() const {
  return impl_->status();
}

}