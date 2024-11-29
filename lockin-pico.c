#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"

#define CLOCK_FREQ_HZ 270000000

#define PWM_PIN 1
#define PWM_FREQ 500
#define DUTY_CYCLE_PERCENT 50

#define ADC_PIN 27

// For an explanation in how the PWM works, visit the URL below
// https://www.i-programmer.info/programming/hardware/14849-the-pico-in-c-basic-pwm.html?start=1
void init_pwm() {
    // Allocate gpio 1 to PWM
    gpio_set_function(PWM_PIN, GPIO_FUNC_PWM);

    // Find out which PWM slice and channel is connected to GPIO 1
    uint slice_num = pwm_gpio_to_slice_num(PWM_PIN);
    uint channel = pwm_gpio_to_channel(PWM_PIN);

    // Calculate how much we need to divide the clock frequency
    // Using 2048 instead of 4096 as in the website because of the overclock
    float clock_divider = ceil(CLOCK_FREQ_HZ / (2048 * PWM_FREQ)) / 16;
    pwm_set_clkdiv(slice_num, clock_divider);

    // Calculate the wrap value of the PWM counter (how much it counts before resetting)
    float divided_clock_freq = CLOCK_FREQ_HZ / clock_divider;
    uint16_t counter_wrap = (divided_clock_freq / PWM_FREQ) - 1;
    pwm_set_wrap(slice_num, counter_wrap);

    // Calculate the level (count value) at which the PWM should switch between 1 and 0
    uint16_t level = counter_wrap / (100 / DUTY_CYCLE_PERCENT);
    pwm_set_chan_level(slice_num, channel, level);

    // Enable PWM
    pwm_set_enabled(slice_num, true);
}

// Define the register for the ADC clock source and the ADC clock divisor
// This will be used for overclocking the ADC. For more info, see URL below
// https://forums.raspberrypi.com/viewtopic.php?t=365702
#define CLOCKS_CLK_ADC_CTRL ((uint32_t*) (CLOCKS_BASE + CLOCKS_CLK_ADC_CTRL_OFFSET))
#define CLOCKS_CLK_ADC_DIV ((uint32_t*) (CLOCKS_BASE + CLOCKS_CLK_ADC_DIV_OFFSET))

void init_adc() {
    adc_init();
    
    adc_gpio_init(ADC_PIN);

    // Set the ADC clock source register to use the system clock and the clock divisor value
    *CLOCKS_CLK_ADC_CTRL = 0x820;
    *CLOCKS_CLK_ADC_DIV = 0x200;
}

uint16_t read_adc_raw(uint gpio) {
    // ADC inputs are from 0-3 (GPIO 26-29)
    adc_select_input(gpio - 26);

    return adc_read();
}

float read_adc_voltage(uint gpio) {
    // 12-bit conversion, assume max value == ADC_VREF == 3.3 V
    const float conversion_factor = 3.3f / (1 << 12);

    uint16_t adc_raw_value = read_adc_raw(gpio);

    return adc_raw_value * conversion_factor;
}

// Only needed for the systick timer
#include "hardware/structs/systick.h"

void measure_avg_adc_time_us() {
    // Register magic to setup the systick timer
    systick_hw->csr = 0x5;
    systick_hw->rvr = 0x00FFFFFF;

    #define MAX_ITER 1200

    float arr[MAX_ITER];
    uint accumulator = 0;

    for (uint i = 0; i < MAX_ITER; i++) {
        // Get the start tick
        uint32_t start_tick = systick_hw->cvr;

        arr[i] = read_adc_voltage(ADC_PIN);

        // Get the end tick and the difference
        uint32_t end_tick = systick_hw->cvr;
        uint32_t number_of_ticks = start_tick - end_tick;

        accumulator += number_of_ticks;
    }

    // Get the time in microseconds
    float result = (accumulator / MAX_ITER) * (1000000.0 / CLOCK_FREQ_HZ);
    printf("Tempo médio da medida do ADC: %f us\n", result);

    // Prints the values
    /*printf("[");
    for (int i = 0; i < MAX_ITER; i++) {
        if ((i + 1) == MAX_ITER) {
            printf("%f]\n\n\n", arr[i]);
        } else {
            printf("%f, ", arr[i]);
        }
    }*/
}

int main()
{
    // Overclocks the device
    set_sys_clock_hz(CLOCK_FREQ_HZ, true);

    // Initializes the USB stuff
    stdio_init_all();

    init_pwm();
    init_adc();

    while (true) {
        measure_avg_adc_time_us();
        sleep_ms(5000);
    }
}
