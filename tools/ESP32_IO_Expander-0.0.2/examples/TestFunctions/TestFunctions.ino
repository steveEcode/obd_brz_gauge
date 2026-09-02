#include <Arduino.h>
#include <ESP_IOExpander_Library.h>

#define EXAMPLE_I2C_NUM         (0)
#define EXAMPLE_I2C_SDA_PIN     (8)
#define EXAMPLE_I2C_SCL_PIN     (18)

/**
 * Create an ESP_IOExpander object, Currently supports:
 *      - TCA95xx (8bit)
 *      - TCA95xx (16bit)
 *      - HT8574
 */
ESP_IOExpander *expander = new ESP_IOExpander_TCA95xx_8bit(EXAMPLE_I2C_NUM, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, EXAMPLE_I2C_SCL_PIN, EXAMPLE_I2C_SDA_PIN);

void setup()
{
    Serial.begin(115200);
    Serial.println("Test begin");

    expander->init();
    expander->begin();

    Serial.println("Original status:");
    expander->printStatus();

    expander->pinMode(0, OUTPUT);
    expander->pinMode(1, OUTPUT);
    expander->multiPinMode(IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, OUTPUT);

    Serial.println("Set pint 0-3 to output mode:");
    expander->printStatus();

    expander->digitalWrite(0, LOW);
    expander->digitalWrite(1, LOW);
    expander->multiDigitalWrite(IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, LOW);

    Serial.println("Set pint 0-3 to low level:");
    expander->printStatus();

    expander->pinMode(0, INPUT);
    expander->pinMode(1, INPUT);
    expander->multiPinMode(IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, INPUT);

    Serial.println("Set pint 0-3 to input mode:");
    expander->printStatus();
}

int level[4] = {0, 0, 0, 0};
uint32_t level_temp;
String level_str;

void loop()
{
    // Read pin 0-3 level
    level[0] = expander->digitalRead(0);
    level[1] = expander->digitalRead(1);
    level_temp = expander->multiDigitalRead(IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3);
    level[2] = level_temp & IO_EXPANDER_PIN_NUM_2 ? HIGH : LOW;
    level[3] = level_temp & IO_EXPANDER_PIN_NUM_3 ? HIGH : LOW;

    Serial.print("Pin level: ");
    Serial.print(level[0]);
    Serial.print(", ");
    Serial.print(level[1]);
    Serial.print(", ");
    Serial.print(level[2]);
    Serial.print(", ");
    Serial.println(level[3]);

    sleep(1);
}
