#include "bsp_obd_dsp/exio/TCA9554PWR.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "TCA9554PWR";

/* TCA9554 sits on the main I2C bus (NUM_0, new API) */
static i2c_master_dev_handle_t get_dev(void)
{
    static i2c_master_dev_handle_t s_dev;
    static bool s_initialized = false;
    if (!s_initialized) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_7,
            .device_address = TCA9554_ADDRESS,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        i2c_master_bus_add_device(I2C_GetBusHandle(), &dev_cfg, &s_dev);
        s_initialized = true;
    }
    return s_dev;
}

/*****************************************************  Operation register REG   ****************************************************/
uint8_t Read_REG(uint8_t REG)                                // Read the value of the TCA9554PWR register REG
{
    uint8_t bitsStatus = 0;
    i2c_master_transmit_receive(get_dev(), &REG, 1, &bitsStatus, 1,
                                (int)I2C_MASTER_TIMEOUT_MS);
    return bitsStatus;
}
void Write_REG(uint8_t REG,uint8_t Data)                    // Write Data to the REG register of the TCA9554PWR
{
    uint8_t buf[2] = { REG, Data };
    i2c_master_transmit(get_dev(), buf, 2, (int)I2C_MASTER_TIMEOUT_MS);
}

/********************************************************** Set EXIO mode **********************************************************/
void Mode_EXIO(uint8_t Pin,uint8_t State)                 // Set the mode of the TCA9554PWR Pin. The default is Output mode (output mode or input mode). State: 0= Output mode 1= input mode
{
    uint8_t bitsStatus = Read_REG(TCA9554_CONFIG_REG);
    uint8_t Data = (0x01 << (Pin-1)) | bitsStatus;
    Write_REG(TCA9554_CONFIG_REG,Data);
}
void Mode_EXIOS(uint8_t PinState)                        // Set the mode of the 7 pins from the TCA9554PWR with PinState
{
    Write_REG(TCA9554_CONFIG_REG,PinState);
}

/********************************************************** Read EXIO status **********************************************************/
uint8_t Read_EXIO(uint8_t Pin)                            // Read the level of the TCA9554PWR Pin
{
    uint8_t inputBits =Read_REG(TCA9554_INPUT_REG);
    uint8_t bitStatus = (inputBits >> (Pin-1)) & 0x01;
    return bitStatus;
}
uint8_t Read_EXIOS(void)                                  // Read the level of all pins of TCA9554PWR
{
  uint8_t inputBits = Read_REG(TCA9554_INPUT_REG);
  return inputBits;
}

/********************************************************** Set the EXIO output status **********************************************************/
void Set_EXIO(uint8_t Pin,bool State)                  // Sets the level state of the Pin without affecting the other pins(PIN：1~8)
{
    uint8_t Data = 0;
    uint8_t bitsStatus = Read_REG(TCA9554_OUTPUT_REG);
    if(Pin < 9 && Pin > 0){
        if(State)
            Data = (0x01 << (Pin-1)) | bitsStatus;
        else
            Data = (~(0x01 << (Pin-1)) & bitsStatus);
        Write_REG(TCA9554_OUTPUT_REG,Data);
    }
    else
        ESP_LOGW(TAG, "Parameter error, please enter the correct parameter!");

}
void Set_EXIOS(uint8_t PinState)                     // Set 7 pins to the PinState state such as :PinState=0x23, 0010 0011 state (the highest bit is not used)
{
    Write_REG(TCA9554_OUTPUT_REG,PinState);
}

/********************************************************** Flip EXIO state **********************************************************/
void Set_Toggle(uint8_t Pin)                              // Flip the level of the TCA9554PWR Pin
{
    uint8_t bitsStatus = Read_EXIO(Pin);
    Set_EXIO(Pin,(bool)!bitsStatus);
}


/********************************************************* TCA9554PWR Initializes the device ***********************************************************/
void TCA9554PWR_Init(uint8_t PinState)                  // Set the seven pins to PinState state, for example :PinState=0x23, 0010 0011 State (the highest bit is not used) (Output mode or input mode) 0= Output mode 1= Input mode. The default value is output mode
{
    Mode_EXIOS(PinState);
}

esp_err_t EXIO_Init(void)
{
    TCA9554PWR_Init(0x00);
    return ESP_OK;
}
