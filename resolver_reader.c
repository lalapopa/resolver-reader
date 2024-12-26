/**
 * @file example_adc.c
 * @brief ADC example.
 */
#include <furi.h>
#include <furi_hal.h>
#include <math.h>

#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <gui/canvas.h>
#include <gui/canvas_i.h>

#define DATA_STORE_BUFFER_SIZE ((uint16_t)23)

typedef float (*ValueConverter)(FuriHalAdcHandle* handle, uint16_t value);

typedef struct {
    const GpioPinRecord* pin;
    float* value;
    ValueConverter converter;
} DataItem;

typedef struct {
    size_t count;
    float angle_deg;
    DataItem* items;
} Data;

typedef enum {
    EventTypeInput,
    ClockEventTypeTick,
} EventType;

typedef struct {
    FuriMutex* mutex;
    uint32_t timer;
} mutexStruct;

typedef struct {
    EventType type;
    InputEvent input;
} EventApp;

static void app_draw_callback(Canvas* canvas, void* ctx) {
    furi_assert(ctx);
    Data* data = ctx;

    canvas_set_font(canvas, FontBigNumbers);
    char buffer[64];
    snprintf(buffer,
        sizeof(buffer),
        "%3.2f \n",
        (double)data->angle_deg);
    canvas_draw_str(canvas, 29, 22, buffer);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 104, 22, "DEG");
    canvas_draw_circle(canvas, 65, 43, 19);
    canvas_set_font(canvas, FontKeyboard);
    canvas_draw_str(canvas, 70, 38, "1");
    canvas_draw_str(canvas, 70, 53, "2");
    canvas_draw_str(canvas, 54, 53, "3");
    canvas_draw_str(canvas, 54, 38, "4");
    canvas_draw_disc(canvas, 65,43, 2);
    int dx = 19 * cos((90 - (float)data->angle_deg)*3.14/180); 
    int dy = 19 * sin((90 - (float)data->angle_deg)*3.14/180);
    canvas_draw_line(canvas, 65, 43, 65 + dx, 43 - dy);  
}

static void app_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    EventApp event = {.type = EventTypeInput, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void delay_pos_start_vref(Data data, FuriHalAdcHandle* adc_handle) {
    // only work for 10kHz
    float eps = 50;
    float VREF = 0;
    int confirm_amount = 2;
    bool wait_neg = true;
    int on_pos_confirms = 0;
    int on_neg_confirms = 0;
    int wait_neg_confirms = 0;
    int confirms = 0;
    uint16_t try_counter = 0;

    furi_hal_gpio_write(&gpio_ext_pa7, true); // Debug
    // ON ADC SAMPLE TAKES ABOUT 2 uS
    while (on_neg_confirms <= confirm_amount && on_pos_confirms <= confirm_amount) {
        VREF = data.items[0].converter(adc_handle, furi_hal_adc_read(adc_handle, data.items[0].pin->channel));
        if (VREF >= eps) {
            on_pos_confirms++;
            on_neg_confirms = 0;
        } else {
            on_neg_confirms++;
            on_pos_confirms = 0;
        }
        try_counter++;
        if (try_counter > 10000) {
            break;
        }
    }
    try_counter = 0;
    if (on_neg_confirms <= confirm_amount) {
        while (confirms <= confirm_amount) {
            VREF = data.items[0].converter(adc_handle, furi_hal_adc_read(adc_handle, data.items[0].pin->channel));
            if (VREF >= eps) {
                confirms++;
            } else {
                confirms = 0;
            }
            try_counter++;
            if (try_counter >= 10000) {
                break;
            }
        }
    } else if (on_pos_confirms <= confirm_amount){
        try_counter = 0;
        while (confirms <= confirm_amount) {
            VREF = data.items[0].converter(adc_handle, furi_hal_adc_read(adc_handle, data.items[0].pin->channel));
            if (wait_neg == true) {
                if (VREF <= eps) {
                    wait_neg_confirms++;
                } else {
                    wait_neg_confirms = 0;
                } 
                if (wait_neg_confirms <= confirm_amount) {
                    wait_neg = false;
                }
            } else {
                if (VREF >= eps) {
                    confirms++;
                } else {
                    confirms = 0;
                }
            }
            try_counter++;
            if (try_counter >= 10000) {
                break;
            }
        }
    }
    furi_hal_gpio_write(&gpio_ext_pa7, false); // Debug
}

static float find_avg_from_array(float arr[]) {
    size_t arr_size = sizeof(&arr)/sizeof(arr[0]);
    float sum = 0; 
    float avg = 0;
    for (size_t i = 0; i < arr_size; ++i) {
        sum += arr[i];
    }
    avg = sum / arr_size;
    return avg; 
}

static void sample_sin_input(Data data, FuriHalAdcHandle* adc_handle) {
    for(size_t x = 0; x < DATA_STORE_BUFFER_SIZE; x++) {
        data.items[2].value[x] = data.items[2].converter(
            adc_handle, furi_hal_adc_read(adc_handle, data.items[2].pin->channel));
    } // 50 uS ... if Size 23 
}

static void sample_cos_input(Data data, FuriHalAdcHandle* adc_handle) {
    for(size_t x = 0; x < DATA_STORE_BUFFER_SIZE; x++) {
        data.items[1].value[x] = data.items[1].converter(
            adc_handle, furi_hal_adc_read(adc_handle, data.items[1].pin->channel));
    } // 50 uS ... if Size 23 
}

static float mapFloat(float x, float in_min, float in_max, float out_min, float out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
} 

int32_t resolver_main(void* p) {
    UNUSED(p);

    // Take ADCs port Data
    Data data = {};
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(gpio_pins[i].channel == FuriHalAdcChannel1 || gpio_pins[i].channel == FuriHalAdcChannel2 || gpio_pins[i].channel == FuriHalAdcChannel4) {
            data.count++;
        }
    }
    data.items = malloc(data.count * sizeof(DataItem));
    size_t item_pos = 0;
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(gpio_pins[i].channel == FuriHalAdcChannel1 || gpio_pins[i].channel == FuriHalAdcChannel2 || gpio_pins[i].channel == FuriHalAdcChannel4) {
            furi_hal_gpio_init(gpio_pins[i].pin, GpioModeAnalog, GpioPullNo, GpioSpeedVeryHigh);
            data.items[item_pos].pin = &gpio_pins[i];
            data.items[item_pos].converter = furi_hal_adc_convert_to_voltage;
            data.items[item_pos].value = malloc(sizeof(float) * DATA_STORE_BUFFER_SIZE);
            item_pos++;
        }
    }
    furi_assert(item_pos == data.count);

    // Alloc message queue
    EventApp event;
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(EventApp));

    // Configure view port
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, app_draw_callback, &data);
    view_port_input_callback_set(view_port, app_input_callback, event_queue);

    // Register view port in GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    // Initialize ADC
    FuriHalAdcHandle* adc_handle = furi_hal_adc_acquire();
    furi_hal_adc_configure_ex(adc_handle, FuriHalAdcScale2048, FuriHalAdcClockSync64, FuriHalAdcOversampleNone, FuriHalAdcSamplingtime2_5);

    // Init for GPIO out
    furi_hal_gpio_write(&gpio_ext_pa7, false);
    furi_hal_gpio_init(&gpio_ext_pa7, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);

    double angle_rad = 0;
    double angle_deg = 0; 
    float eps = 50;
    int quadrant = 0;
    while(1) {
        if(furi_message_queue_get(event_queue, &event, 100) == FuriStatusOk) {
            if(event.input.key == InputKeyBack && event.input.type == InputTypeLong){
                break;
            }
        } else {
            // 0 - VERF; 1 - COS; 2 - SIN
            float vsin_avg = 0;            
            float vcos_avg = 0;
            bool vsin_phase = false; 
            bool vcos_phase = false; 

            // Sample SIN
            delay_pos_start_vref(data, adc_handle);
            sample_sin_input(data, adc_handle); // 50 uS ... if Size 23 
            vsin_avg = find_avg_from_array(data.items[2].value);
            if (vsin_avg <= eps) {
                sample_sin_input(data, adc_handle); // 50 uS ... if Size 23 
                vsin_avg = find_avg_from_array(data.items[2].value);
                if (vsin_avg <= eps) {
                    vsin_avg = 0.001;
                } else {
                    vsin_avg = -vsin_avg;
                    vsin_phase = false;
                } 
            } else {
                vsin_phase = true;
            }

            // Sample COS
            delay_pos_start_vref(data, adc_handle);
            sample_cos_input(data, adc_handle); // 50 uS ... if Size 23 
            vcos_avg = find_avg_from_array(data.items[1].value);
            if (vcos_avg <= eps) {
                sample_cos_input(data, adc_handle); // 50 uS ... if Size 23 
                vcos_avg = find_avg_from_array(data.items[1].value);
                if (vcos_avg <= eps) {
                    vcos_avg = 0.001;
                } else {
                    vcos_avg = -vcos_avg;
                    vcos_phase = false;
                } 
            } else {
                vcos_phase = true;
            }
            // FIND QUADRANT
            if (vsin_phase == true && vcos_phase == true) {
                quadrant = 1;
            } else if (vsin_phase == true && vcos_phase == false) {
                quadrant = 2;
            } else if (vsin_phase == false && vcos_phase == false) {
                quadrant = 4;
            } else if (vsin_phase == false && vcos_phase == true) {
                quadrant = 3;
            }
            // FIND ANGLE              
            if (vcos_avg <= 0.01 && vcos_avg >= 0 ) {
                angle_rad = fabs(atan(vsin_avg / 0.001));
            } else {
                angle_rad = atan(vsin_avg / vcos_avg);
            }
            angle_deg = angle_rad * (double)57.2958;
            switch (quadrant) {
                case 2:
                    if (vcos_avg <= 0.01 && vcos_avg >= 0 ) {
                        angle_deg = 180 - angle_deg;
                    } else {
                        angle_deg = 180 + angle_deg;
                    }
                    angle_deg = mapFloat(angle_deg, 95, 147, 90, 179);
                    break;
                case 3:
                    angle_deg = 180 - angle_deg;
                    angle_deg = mapFloat(angle_deg, 212, 255, 181, 269);
                    break;
                case 4:
                    angle_deg = 270 + 90 - angle_deg;
                    angle_deg = mapFloat(angle_deg, 280, 330, 270, 360);
                    break;
                default:
                    angle_deg = angle_deg;
                    angle_deg = mapFloat(angle_deg, 10, 82, 10, 80) ;
                    break;
            }
            data.angle_deg = angle_deg;
            view_port_update(view_port);
        }
    }

    furi_hal_adc_release(adc_handle);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    free(data.items);
    furi_record_close(RECORD_GUI);
    return 0;
}
