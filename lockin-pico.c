#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/dma.h"

#define CLOCK_FREQ_HZ 270000000

// ADC frequencies over 135 MHz showed distortions around the 2048 mark (half of the 12-bit range)
#define ADC_FREQ_DIVIDER 2
#define ADC_FREQ_HZ (CLOCK_FREQ_HZ / ADC_FREQ_DIVIDER)

#define DMA_CHANNEL 0

#define PWM_PIN 0
#define PWM_FREQ 500
#define DUTY_CYCLE_PERCENT 50

#define REFERENCE_ADC_PIN 26
#define INPUT_ADC_PIN (REFERENCE_ADC_PIN + 1)

// ADC capture buffer should fit two periods: one from the reference and another from input
uint adc_capture_buffer_size = ((2.0 * 1000000 / PWM_FREQ) / (96.0 * 1000000 / ADC_FREQ_HZ)) + 1;
uint16_t* adc_capture_buffer;

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

bool init_adc() {
    adc_init();
    
    adc_gpio_init(REFERENCE_ADC_PIN);
    adc_gpio_init(INPUT_ADC_PIN);

    // Set the ADC clock source register to use the system clock
    clock_configure(clk_adc, 0, CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, CLOCK_FREQ_HZ, ADC_FREQ_HZ);

    // Sets the ADC to switch between reading reference and input using a mask
    uint input_mask = 1 << (REFERENCE_ADC_PIN - ADC_BASE_PIN) | 1 << (INPUT_ADC_PIN - ADC_BASE_PIN);
    adc_set_round_robin(input_mask);

    // Set up the ADC FIFO to write every sample to the FIFO, call the DMA interrupt every sample,
    // disable the error bit and maintain 12-bit samples
    adc_fifo_setup(true, true, 1, false, false);

    // Get DMA channel and default config
    dma_channel_claim(DMA_CHANNEL);
    dma_channel_start(DMA_CHANNEL);
    dma_channel_config cfg = dma_channel_get_default_config(DMA_CHANNEL);

    // Reading from constant address, writing to incrementing byte addresses, transferring 16 bits
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);

    // Pace transfers based on availability of ADC samples
    channel_config_set_dreq(&cfg, DREQ_ADC);
    
    // Allocate the buffer on memory
    adc_capture_buffer = malloc(adc_capture_buffer_size * sizeof(uint16_t));
    if (adc_capture_buffer == NULL) {
        printf("ERROR WHILE ALLOCATING MEMORY FOR CAPTURE_BUFFER!\n");

        return false;
    }

    // Configure the DMA channel to read from ADC FIFO and write to capture buffer
    dma_channel_configure(DMA_CHANNEL, &cfg, adc_capture_buffer, &adc_hw->fifo, adc_capture_buffer_size, false);
}

void start_adc_sampling() {
    // ADC inputs are from 0-3 (GPIO 26-29)
    adc_select_input(REFERENCE_ADC_PIN - ADC_BASE_PIN);

    // Reset the write address back to the start of the capture buffer
    dma_channel_set_write_addr(DMA_CHANNEL, adc_capture_buffer, true);

    // Start free-running sampling mode
    adc_run(true);

    // Once DMA finishes, stop any new conversions from starting, and clean up
    // the FIFO in case the ADC was still mid-conversion
    dma_channel_wait_for_finish_blocking(DMA_CHANNEL);
    adc_run(false);
    adc_fifo_drain();
}

// Returns the average ADC value for the reference in the lower 16 bits and for the input on the higher 16 bits
uint32_t get_average_adc() {
    start_adc_sampling();

    // We should guarantee that the number of samples is even (and rounded down) since we're sampling two inputs
    uint rounded_size = floor(adc_capture_buffer_size / 2) * 2;

    uint accumulator_reference = 0;
    uint accumulator_input = 0;
    for (int i = 0; i < rounded_size; i += 2) {
        accumulator_reference += adc_capture_buffer[i];
        accumulator_input += adc_capture_buffer[i + 1];
    }

    uint16_t result_reference = round(accumulator_reference / (rounded_size / 2));
    uint16_t result_input = round(accumulator_input / (rounded_size / 2));

    return result_reference | (result_input << 16);
}

int main()
{
    // Overclocks the device
    set_sys_clock_hz(CLOCK_FREQ_HZ, true);

    // Initializes the USB stuff
    stdio_init_all();

    init_pwm();

    bool success = init_adc();
    if (!success) return 1;

    while (true) {
        const float conversion_factor = 3.3f / (1 << 12);

        uint32_t average_adc = get_average_adc();

        float average_ref = (average_adc & 0xFFFF) * conversion_factor;
        float average_input = (average_adc >> 16) * conversion_factor;

        printf("Tensão média da referência: %f V, entrada: %f V\n", average_ref, average_input);
        sleep_ms(1000);
    }
}
