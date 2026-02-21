#include <stdio.h>
#include "hardware_definitions.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ESP_INTR_FLAG_DEFAULT 0

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    if (gpio_num == BOOT_PIN)
    {
        gpio_set_level(STATUS_LED_PIN, gpio_get_level(BOOT_PIN));
    }
}

void configure_gpios (void)
{
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = STATUS_LED_PIN_BITMASK;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //disable interrupt
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    //set as output mode
    io_conf.mode = GPIO_MODE_INPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = BOOT_PIN_BITMASK;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 1;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //change gpio interrupt type for one pin
    gpio_set_intr_type(BOOT_PIN, GPIO_INTR_ANYEDGE);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(BOOT_PIN, gpio_isr_handler, (void*) BOOT_PIN);

    gpio_dump_io_configuration(stdout, SOC_GPIO_VALID_GPIO_MASK);
}

void app_main(void)
{
    //configure the gpios
    configure_gpios();

    // button is pressed the isr is called and led is controlled in ISR nothing happeing in the while loop
    while (1)
    {
        vTaskDelay(500/portTICK_PERIOD_MS);
    }
}
