#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#define CLOCK_FREQ_HZ 270000000

#define PWM_PIN 1
#define PWM_FREQ 500
#define DUTY_CYCLE_PERCENT 50

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

int main()
{
    set_sys_clock_hz(CLOCK_FREQ_HZ, true);

    stdio_init_all();

    init_pwm();

    while (true) {
        printf("Hello, world!\n");
        sleep_ms(1000);
    }
}
