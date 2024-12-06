#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <tusb.h>
#include <complex.h>
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

#define INPUT_SAMPLE_ITERATIONS 1024
#define INPUT_SAMPLE_SIZE 4

#define RI 9500
#define RS 100000

// ADC capture buffer should fit two periods: one from the reference and another from input
uint adc_capture_buffer_size = ((1000000 / PWM_FREQ) / (96.0 * 1000000 / ADC_FREQ_HZ)) + 1;
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
    adc_capture_buffer = calloc(adc_capture_buffer_size, sizeof(uint16_t));
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

int* get_input_samples(int input_iterations) {
    // We should guarantee that the number of samples is even (and rounded down) since we're sampling two inputs
    uint rounded_size = floor(adc_capture_buffer_size / 2) * 2;

    // Calculate the interval between one sample and another
    double sampling_frequency_us = 96.0 * 1000000 / ADC_FREQ_HZ;
    double input_sample_interval_us = 1000000 / (INPUT_SAMPLE_SIZE * PWM_FREQ);
    double sample_index_spacing =  input_sample_interval_us / sampling_frequency_us;

    // Allocate the memory for the samples
    int* input_samples = calloc(INPUT_SAMPLE_SIZE, sizeof(int));
    if (input_samples == NULL) {
        printf("ERROR WHILE ALLOCATING MEMORY FOR INPUT SAMPLES BUFFER!\n");

        return NULL;
    }

    // The size is 3 bytes more than the length (considering the chars '[', ']' and '\0')
    uint indicator_length = 30;
    uint indicator_size = indicator_length + 3;
    char progress_indicator[indicator_size];
    for (int i = 0; i < indicator_size; i++) {
        if (i == 0) progress_indicator[i] = '[';
        else if (i == (indicator_size - 2)) progress_indicator[i] = ']';
        else if (i == (indicator_size - 1)) progress_indicator[i] = '\0';
        else progress_indicator[i] = ' ';
    }

    // Instead of getting the samples in one period, average between multiple ones to remove noise
    for (int i = 0; i < input_iterations; i++) {
        start_adc_sampling();

        // Get the average value of the reference and input values
        uint accumulator_reference = 0;
        uint accumulator_input = 0;
        for (int i = 0; i < rounded_size; i += 2) {
            accumulator_reference += adc_capture_buffer[i];
            accumulator_input += adc_capture_buffer[i + 1];
        }
        uint16_t average_ref = round(accumulator_reference / (rounded_size / 2));
        uint16_t average_input = round(accumulator_input / (rounded_size / 2));

        // Initialize the variable containing the index of the first reference sample after zero crossing as -1 (UINT_MAX)
        uint zero_index = -1;

        uint16_t previous_reference_value = 0;
        // Initializes the current reference value as the last reference sample
        uint16_t current_reference_value = adc_capture_buffer[rounded_size - 2];
        for (int i = 0; i < rounded_size; i += 2) {
            // Update the loop values
            previous_reference_value = current_reference_value;
            current_reference_value = adc_capture_buffer[i];

            // If the previous value is under the average and the current is over, zero crossing has ocurred
            if (previous_reference_value < average_ref && current_reference_value >= average_ref) {
                zero_index = i;
                break;
            }
        }

        // If the index of the first reference sample is still as UINT_MAX, we couldn't find the zero crossing
        if (zero_index == -1) {
            printf("ERROR WHILE SEARCHING FOR ZERO CROSSING ON REFERENCE!\n");
            continue;
        }

        // Use modular arithmetic to acquire the samples without overflow, making sure we're getting an input (odd) sample
        double sample = zero_index + 1;
        for (int j = 0; j < INPUT_SAMPLE_SIZE; j++) {
            uint nearest_odd_sample = (uint) sample % 2 != 0 ? sample : (sample + 1);
            input_samples[j] += (adc_capture_buffer[nearest_odd_sample] - average_input);

            sample = fmod(sample + sample_index_spacing, rounded_size);
        }

        // Calculate the loop progress to print on the screen
        uint progress = indicator_length * (i + 1) / input_iterations;
        uint percentage = 100 * progress / indicator_length;
        for (int j = 1; j <= progress; j++) {
            progress_indicator[j] = '=';
        }
        printf("\rMeasuring: %s %d%%", progress_indicator, percentage);
    }

    printf("\n");

    // Get the average of the acquired samples
    for (int i = 0; i < INPUT_SAMPLE_SIZE; i++) {
        input_samples[i] = round(input_samples[i] / input_iterations);
    }
    
    return input_samples;
}

void print_samples(int* samples) {
    const float conversion_factor = 3.3f / (1 << 12);

    printf("Samples: [");
    for (int i = 0; i < INPUT_SAMPLE_SIZE; i++) {
        printf(" %lf", samples[i] * conversion_factor);
    }
    printf(" ]\n");
}

double complex get_voltage(int* samples) {
    double quadrature = samples[0] - samples[2];
    double inphase = samples[1] - samples[3];

    return (inphase + quadrature * I);
}

double complex calculate_result(int* open_circuit_samples, int* dut_samples) {
    double complex dut_open_voltage = get_voltage(open_circuit_samples);
    double complex dut_short_voltage = 0 + 0 * I; // Considering a perfect short
    double complex dut_voltage = get_voltage(dut_samples);

    double complex result = (RI * RS * (dut_voltage - dut_short_voltage)) / (RI + RS * (dut_open_voltage - dut_voltage));

    return result;
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

    // Wait for USB connection
    while (!tud_cdc_connected()) sleep_ms(100);

    // Clear the screen
    printf("\e[1;1H\e[2J");

    printf("\n-------------------------------------------------\n");
    printf("Set up the DUT as open circuit and press Enter...\n");
    getchar();

    int* open_circuit_samples = get_input_samples(8192);
    if (open_circuit_samples == NULL) return 1;
    print_samples(open_circuit_samples);

    printf("\nSet up the DUT as the impedance to be measured and press Enter...\n");
    getchar();

    while (true) {
        int* dut_samples = get_input_samples(8192);
        if (dut_samples == NULL) return 1;
        print_samples(dut_samples);

        double complex result = calculate_result(open_circuit_samples, dut_samples);
        printf("Result: %lf%+lfi\n", creal(result), cimag(result));

        printf("\nTo measure again, press Enter...\n");
        getchar();

        free(dut_samples);
    }
}
