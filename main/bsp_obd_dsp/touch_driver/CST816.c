#include "bsp_obd_dsp/touch_driver/CST816.h"
#include "bsp_obd_dsp/exio/TCA9554PWR.h"
#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"

#define POINT_NUM_MAX       (1)

#define DATA_START_REG      (0x02)
#define CHIP_ID_REG         (0xA7)
#define AutoSleep_REG       (0xFE)

static const char *TAG = "CST816";

esp_lcd_touch_handle_t tp = NULL;

static esp_err_t read_data(esp_lcd_touch_handle_t tp);
static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t del(esp_lcd_touch_handle_t tp);

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len);
static esp_err_t i2c_write_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t* data, uint8_t len);

static esp_err_t reset(esp_lcd_touch_handle_t tp);
static esp_err_t read_id(esp_lcd_touch_handle_t tp);
static void AutoSleep(esp_lcd_touch_handle_t tp, bool Sleep_State);

static i2c_master_dev_handle_t s_touch_dev = {0};

esp_err_t esp_lcd_touch_new_i2c_cst816(i2c_master_bus_handle_t i2c_bus, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp)
{
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_ARG, TAG, "Invalid i2c_bus");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_INVALID_ARG, TAG, "Invalid touch handle");

    /* Prepare main structure */
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t cst816s = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(cst816s, ESP_ERR_NO_MEM, err, TAG, "Touch handle malloc failed");

    /* Add I2C device on the bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_7,
        .device_address = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
        .scl_speed_hz = I2C_Touch_MASTER_FREQ_HZ,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_touch_dev), err, TAG, "Add I2C device failed");

    /* Communication interface */
    /* Only supported callbacks are set */
    cst816s->read_data = read_data;
    cst816s->get_xy = get_xy;
    cst816s->del = del;
    /* Mutex */
    cst816s->data.lock.owner = portMUX_FREE_VAL;
    /* Save config */
    memcpy(&cst816s->config, config, sizeof(esp_lcd_touch_config_t));

    /* Prepare pin for touch interrupt */
    if (cst816s->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = (cst816s->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = BIT64(cst816s->config.int_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO intr config failed");

        /* Register interrupt callback */
        if (cst816s->config.interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(cst816s, cst816s->config.interrupt_callback);
        }
    }
    /* Prepare pin for touch controller reset */
    if (cst816s->config.rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(cst816s->config.rst_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_config), err, TAG, "GPIO reset config failed");
    }
    /* Reset controller */
    ESP_GOTO_ON_ERROR(reset(cst816s), err, TAG, "Reset failed");

    /* Read product id */
    ESP_GOTO_ON_ERROR(read_id(cst816s), err, TAG, "Read version failed");
    *tp = cst816s;
    AutoSleep(cst816s, false);
    return ESP_OK;
err:
    if (cst816s) {
        del(cst816s);
    }
    ESP_LOGE(TAG, "Initialization failed!");
    return ret;
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp)
{
    typedef struct {
        uint8_t num;
        uint8_t x_h : 4;
        uint8_t : 4;
        uint8_t x_l;
        uint8_t y_h : 4;
        uint8_t : 4;
        uint8_t y_l;
    } data_t;

    data_t point;
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, DATA_START_REG, (uint8_t *)&point, sizeof(data_t)), TAG, "I2C read failed");

    portENTER_CRITICAL(&tp->data.lock);
    point.num = (point.num > POINT_NUM_MAX ? POINT_NUM_MAX : point.num);
    tp->data.points = point.num;
    /* Fill all coordinates */
    for (int i = 0; i < point.num; i++) {
        tp->data.coords[i].x = point.x_h << 8 | point.x_l;
        tp->data.coords[i].y = point.y_h << 8 | point.y_l;
    }
    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    /* Count of points */
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;

        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    /* Invalidate */
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t del(esp_lcd_touch_handle_t tp)
{
    /* Reset GPIO pin settings */
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
        if (tp->config.interrupt_callback) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    /* Release memory */
    free(tp);

    return ESP_OK;
}

static esp_err_t reset(esp_lcd_touch_handle_t tp)
{
#if CONFIG_OBD_HW_VERSION_V2_NEW || CONFIG_OBD_HW_VERSION_V3_NEW
    /* New boards: touch reset is a direct GPIO (I2C_Touch_RST_IO), which
       esp_lcd_touch_new_i2c_cst816 already configured as an output. */
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_set_level(tp->config.rst_gpio_num, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(tp->config.rst_gpio_num, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#else
    /* Waveshare board: touch reset is wired to the TCA9554 IO expander (EXIO1). */
    Set_EXIO(TCA9554_EXIO1,false);
    vTaskDelay(pdMS_TO_TICKS(10));
    Set_EXIO(TCA9554_EXIO1,true);
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    return ESP_OK;
}

static esp_err_t read_id(esp_lcd_touch_handle_t tp)
{
    uint8_t id;
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, CHIP_ID_REG, &id, 1), TAG, "I2C read failed");
    ESP_LOGD(TAG, "IC id: %d", id);
    return ESP_OK;
}
/*!
    @brief  Fall asleep automatically
*/
static void AutoSleep(esp_lcd_touch_handle_t tp, bool Sleep_State) {
    uint8_t Sleep_State_Set = (uint8_t)(!Sleep_State);
    i2c_write_bytes(tp, AutoSleep_REG, &Sleep_State_Set, 1);
}

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "Invalid data");

    return i2c_master_transmit_receive(s_touch_dev, (uint8_t *)&reg, 1, data, len, 1000);
}

static esp_err_t i2c_write_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t* data, uint8_t len)
{
    assert(tp != NULL);

    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(buf + 1, data, len);

    return i2c_master_transmit(s_touch_dev, buf, len + 1, 1000);
}

void Touch_Init(void)
{
    // The touch controller shares the same physical I2C bus (SCL=10, SDA=11) as the
    // TCA9554 IO expander, so reuse the bus created by I2C_Init() instead of creating a
    // second master bus on the same pins (that triggers "GPIO is not usable, maybe
    // conflict with others" and leaves both buses fighting over the pins).
    i2c_master_bus_handle_t i2c_bus = I2C_GetBusHandle();
    ESP_ERROR_CHECK(i2c_bus ? ESP_OK : ESP_ERR_INVALID_STATE);

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_WIDTH,
        .y_max = EXAMPLE_LCD_HEIGHT,
        .rst_gpio_num = I2C_Touch_RST_IO,
        .int_gpio_num = I2C_Touch_INT_IO,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    /* Initialize touch directly via I2C master bus + device */
    ESP_LOGD(TAG, "Initialize touch controller CST816");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816(i2c_bus, &tp_cfg, &tp));
}
