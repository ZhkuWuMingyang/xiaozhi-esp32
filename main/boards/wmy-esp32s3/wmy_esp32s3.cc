#include "wifi_board.h"
#include "display/lcd_display.h"
#include "esp_lcd_ili9341.h"
#include "font_awesome_symbols.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>

#include "xl9555.h"
#include "codecs/no_audio_codec.h"
#include "led_ws2812.h"
#include "mcp_server.h"

#define TAG "wmy_esp32s3"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);


class wmy_esp32s3 : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay* display_;
    NoAudioCodecSimplexPdm *audio_codec;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void st7789_i80_init() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGI(TAG, "Initialize Intel 8080 bus");
        esp_lcd_i80_bus_handle_t i80_bus = NULL;
        esp_lcd_i80_bus_config_t bus_config = {
            .dc_gpio_num = GPIO_NUM_1,
            .wr_gpio_num = GPIO_NUM_41,
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .data_gpio_nums = {
                GPIO_NUM_40,
                GPIO_NUM_38,
                GPIO_NUM_39,
                GPIO_NUM_48,
                GPIO_NUM_45,
                GPIO_NUM_21,
                GPIO_NUM_47,
                GPIO_NUM_14,
            },
            .bus_width = 8,
            .max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
            .dma_burst_size = 64,
        };
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

        esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = GPIO_NUM_2,
            .pclk_hz = 30 * DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),// 30 fps
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_levels = {
                .dc_idle_level = 0,
                .dc_cmd_level = 0,
                .dc_dummy_level = 0,
                .dc_data_level = 1,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGI(TAG, "Install LCD driver of st7789");
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = GPIO_NUM_NC,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        xl9555_pin_write(IO1_3, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_lcd_panel_reset(panel);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_20_4,
                                        .icon_font = &font_awesome_20_4,
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
                                        .emoji_font = font_emoji_32_init(),
#else
                                        .emoji_font = font_emoji_64_init(),
#endif
                                    });
    }

    void InitializeAudioCodec() {
        audio_codec = new NoAudioCodecSimplexPdm(
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            GPIO_NUM_46, 
            GPIO_NUM_9, 
            GPIO_NUM_8, 
            GPIO_NUM_3, 
            GPIO_NUM_42);
    }

    void led_init() {
        // 初始化LED
        static ws2812_strip_handle_t led_handle;
        static int led_brightness = 0;
        ws2812_init(GPIO_NUM_18, 3, &led_handle);

        // 添加MCP工具
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.led.get_brightness", "获取led的亮度,范围是0-100", 
            PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
            return led_brightness;
        });

        mcp_server.AddTool("self.led.set_brightness", "设置led的亮度,范围是0-100,如果没有检测到具体值，则需要向设备询问具体值", 
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }), 
            [this](const PropertyList& properties) -> ReturnValue {
                led_brightness = properties["brightness"].value<int>();

                for(int i = 0; i < 3; i++) {
                    ws2812_set_brightness(led_handle, i, led_brightness);
                }
                return true;
            });

         mcp_server.AddTool("self.led.set_color", "设置LED的颜色,颜色格式为rgb,范围是0-255", 
            PropertyList({
                Property("r", kPropertyTypeInteger, 0, 255),
                Property("g", kPropertyTypeInteger, 0, 255),
                Property("b", kPropertyTypeInteger, 0, 255)
            }), 
            [this](const PropertyList& properties) -> ReturnValue {
                int r = properties["r"].value<int>();
                int g = properties["g"].value<int>();
                int b = properties["b"].value<int>();
                for(int i = 0; i < 3; i++) {
                    ws2812_write(led_handle, i, r, g, b);
                }

                return true;
            });

        mcp_server.AddTool("self.led.get_color", "获取LED的颜色,颜色格式为rgb,范围是0-255", 
            PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {

                return true;
            });
    }

public:
    wmy_esp32s3() : boot_button_(BOOT_BUTTON_GPIO) {
        xl9555_init(GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_NC, NULL);
        xl9555_ioconfig(~(IO0_0 | IO1_2 | IO1_3) & 0xFFFF);
        st7789_i80_init();
        InitializeButtons();
        InitializeAudioCodec();
        xl9555_pin_write(IO0_0 | IO0_2, 1);
        led_init();
    }

    virtual AudioCodec* GetAudioCodec() override {
        return audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(wmy_esp32s3);
