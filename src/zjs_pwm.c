// Copyright (c) 2016, Intel Corporation.
#ifdef BUILD_MODULE_PWM
// Zephyr includes
#include <zephyr.h>
#include <pwm.h>
#include <misc/util.h>
#include <string.h>

// ZJS includes
#include "zjs_pwm.h"
#include "zjs_util.h"

static const char *ZJS_POLARITY_NORMAL = "normal";
static const char *ZJS_POLARITY_REVERSE = "reverse";

#ifdef CONFIG_BOARD_FRDM_K64F
#define PWM_DEV_COUNT 4
#else
#define PWM_DEV_COUNT 1
#endif

static struct device *zjs_pwm_dev[PWM_DEV_COUNT];

void (*zjs_pwm_convert_pin)(uint32_t orig, int *dev, int *pin) =
    zjs_default_convert_pin;

static void zjs_pwm_set(int devnum, uint32_t channel, uint32_t period,
                        uint32_t pulseWidth, const char *polarity)
{
    // requires: channel is 0-3 on Arduino 101, period is the time in hw cycles
    //             for the on/off cycle to complete, pulse width is the time in
    //             hw cycles for the signal to be on, polarity is "normal" if
    //             on means high, "reversed" if on means low
    //  effects: sets the given period and pulse width, but the true pulse
    //             must always be off for at least one hw cycle
    if (period < 1) {
        // period must be at least one cycle
        period = 1;
    }
    if (pulseWidth > period) {
        PRINT("zjs_pwm_set: pulseWidth was greater than period\n");
        pulseWidth = period;
    }

    uint32_t offduty = period - pulseWidth;

    uint32_t onTime, offTime;
    if (strcmp(polarity, ZJS_POLARITY_REVERSE)) {
        onTime = pulseWidth;
        offTime = offduty;
    }
    else {
        onTime = offduty;
        offTime = pulseWidth;
    }

    // work around the fact that Zephyr API won't allow fully on
    if (offTime == 0) {
        // must be off for at least one cycle
        offTime = 1;
        onTime -= 1;
    }

    pwm_pin_set_values(zjs_pwm_dev[devnum], channel, onTime, offTime);
}

static void zjs_set_period(jerry_value_t obj_val, double period)
{
    // requires: obj is a PWM pin object, period is in milliseconds
    //  effects: sets the PWM pin to the given period, records the period
    //             in the object
    uint32_t channel;
    double pulseWidth;
    zjs_obj_get_uint32(obj_val, "channel", &channel);
    zjs_obj_get_double(obj_val, "pulseWidth", &pulseWidth);

    int devnum, newchannel;
    zjs_pwm_convert_pin(channel, &devnum, &newchannel);

    const int BUFLEN = 10;
    char buffer[BUFLEN];
    const char *polarity = ZJS_POLARITY_NORMAL;
    if (zjs_obj_get_string(obj_val, "polarity", buffer, BUFLEN)) {
        if (!strcmp(buffer, ZJS_POLARITY_REVERSE))
            polarity = ZJS_POLARITY_REVERSE;
    }

    // update the JS object
    zjs_obj_add_number(obj_val, period, "period");

    uint32_t pulseWidthHW = pulseWidth * sys_clock_hw_cycles_per_sec / 1000;
    uint32_t periodHW = period * sys_clock_hw_cycles_per_sec / 1000;

    zjs_pwm_set(devnum, newchannel, periodHW, pulseWidthHW, polarity);
}

static jerry_value_t zjs_pwm_pin_set_period_cycles(const jerry_value_t function_obj_val,
                                                   const jerry_value_t this_val,
                                                   const jerry_value_t args_p[],
                                                   const jerry_length_t args_cnt)
{
    // requires: this_val is a PWMPin object from zjs_pwm_open, takes one
    //             argument, the period in hardware cycles, dependent on the
    //             underlying hardware (31.25ns each for Arduino 101)
    //  effects: updates the period of this PWM pin, using the finest grain
    //             units provided by the platform, providing the widest range
    if (args_cnt < 1 || !jerry_value_is_number(args_p[0]))
        return zjs_error("zjs_pwm_pin_set_period_cycles: invalid argument");

    // convert to milliseconds
    double periodHW = jerry_get_number_value(args_p[0]);
    double period = periodHW / sys_clock_hw_cycles_per_sec * 1000;

    zjs_set_period(this_val, period);
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_pwm_pin_set_period(const jerry_value_t function_obj_val,
                                            const jerry_value_t this_val,
                                            const jerry_value_t args_p[],
                                            const jerry_length_t args_cnt)
{
    // requires: this_val is a PWMPin object from zjs_pwm_open, takes one
    //             argument, the period in milliseconds (float)
    //  effects: updates the period of this PWM pin, getting as close as
    //             possible to what is requested given hardware constraints
    if (args_cnt < 1 || !jerry_value_is_number(args_p[0]))
        return zjs_error("zjs_pwm_pin_set_period: invalid argument");

    zjs_set_period(this_val, jerry_get_number_value(args_p[0]));
    return ZJS_UNDEFINED;
}

static void zjs_set_pulse_width(jerry_value_t obj_val, double pulseWidth)
{
    // requires: obj is a PWM pin object, pulseWidth is in milliseconds
    //  effects: sets the PWM pin to the given pulse width, records the pulse
    //             width in the object
    uint32_t channel;
    double period;
    zjs_obj_get_uint32(obj_val, "channel", &channel);
    zjs_obj_get_double(obj_val, "period", &period);

    int devnum, newchannel;
    zjs_pwm_convert_pin(channel, &devnum, &newchannel);

    const int BUFLEN = 10;
    char buffer[BUFLEN];
    const char *polarity = ZJS_POLARITY_NORMAL;
    if (zjs_obj_get_string(obj_val, "polarity", buffer, BUFLEN)) {
        if (!strcmp(buffer, ZJS_POLARITY_REVERSE))
            polarity = ZJS_POLARITY_REVERSE;
    }

    // update the JS object
    zjs_obj_add_number(obj_val, pulseWidth, "pulseWidth");

    // convert to hw cycles
    uint32_t pulseWidthHW = pulseWidth * sys_clock_hw_cycles_per_sec / 1000;
    uint32_t periodHW = period * sys_clock_hw_cycles_per_sec / 1000;

    zjs_pwm_set(devnum, newchannel, periodHW, pulseWidthHW, polarity);
}

static jerry_value_t zjs_pwm_pin_set_pulse_width_cycles(const jerry_value_t function_obj_val,
                                                        const jerry_value_t this_val,
                                                        const jerry_value_t args_p[],
                                                        const jerry_length_t args_cnt)
{
    // requires: this_val is a PWMPin object from zjs_pwm_open, takes one
    //             argument, the pulse width in hardware cycles, dependent on
    //             the underlying hardware (31.25ns each for Arduino 101)
    //  effects: updates the pulse width of this PWM pin
    if (args_cnt < 1 || !jerry_value_is_number(args_p[0]))
        return zjs_error("zjs_pwm_pin_set_pulse_width_cycles: invalid argument");

    // convert to milliseconds
    double pulseWidthHW = jerry_get_number_value(args_p[0]);
    double pulseWidth = pulseWidthHW / sys_clock_hw_cycles_per_sec * 1000;

    zjs_set_pulse_width(this_val, pulseWidth);
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_pwm_pin_set_pulse_width(const jerry_value_t function_obj_val,
                                                 const jerry_value_t this_val,
                                                 const jerry_value_t args_p[],
                                                 const jerry_length_t args_cnt)
{
    // requires: this_val is a PWMPin object from zjs_pwm_open, takes one
    //             argument, the pulse width in milliseconds (float)
    //  effects: updates the pulse width of this PWM pin
    if (args_cnt < 1 || !jerry_value_is_number(args_p[0]))
        return zjs_error("zjs_pwm_pin_set_pulse_width: invalid argument");

    zjs_set_pulse_width(this_val, jerry_get_number_value(args_p[0]));
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_pwm_open(const jerry_value_t function_obj_val,
                                  const jerry_value_t this_val,
                                  const jerry_value_t args_p[],
                                  const jerry_length_t args_cnt)
{
    // requires: arg 0 is an object with these members: channel (int), period in
    //             hardware cycles (defaults to 255), pulse width in hardware
    //             cycles (defaults to 0), polarity (defaults to "normal")
    //  effects: returns a new PWMPin object representing the given channel
    if (args_cnt < 1 || !jerry_value_is_object(args_p[0]))
        return zjs_error("zjs_pwm_open: invalid argument");

    // data input object
    jerry_value_t data = args_p[0];

    uint32_t channel;
    if (!zjs_obj_get_uint32(data, "channel", &channel))
        return zjs_error("zjs_pwm_open: missing required field");

    int devnum, newchannel;
    zjs_pwm_convert_pin(channel, &devnum, &newchannel);
    if (newchannel == -1)
        return zjs_error("zjs_pwm_open: invalid channel");

    double period, pulseWidth;
    zjs_obj_get_double(data, "period", &period);
    zjs_obj_get_double(data, "pulseWidth", &pulseWidth);

    const int BUFLEN = 10;
    char buffer[BUFLEN];
    const char *polarity = ZJS_POLARITY_NORMAL;
    if (zjs_obj_get_string(data, "polarity", buffer, BUFLEN)) {
        if (!strcmp(buffer, ZJS_POLARITY_REVERSE))
            polarity = ZJS_POLARITY_REVERSE;
    }

    // set the inital timing
    uint32_t pulseWidthHW = pulseWidth * sys_clock_hw_cycles_per_sec / 1000;
    uint32_t periodHW = period * sys_clock_hw_cycles_per_sec / 1000;

    zjs_pwm_set(devnum, newchannel, periodHW, pulseWidthHW, polarity);

    // create the PWMPin object
    jerry_value_t pin_obj = jerry_create_object();
    zjs_obj_add_function(pin_obj, zjs_pwm_pin_set_period, "setPeriod");
    zjs_obj_add_function(pin_obj, zjs_pwm_pin_set_period_cycles,
                         "setPeriodCycles");
    zjs_obj_add_function(pin_obj, zjs_pwm_pin_set_pulse_width, "setPulseWidth");
    zjs_obj_add_function(pin_obj, zjs_pwm_pin_set_pulse_width_cycles,
                         "setPulseWidthCycles");
    zjs_obj_add_number(pin_obj, channel, "channel");
    zjs_obj_add_number(pin_obj, period, "period");
    zjs_obj_add_number(pin_obj, pulseWidth, "pulseWidth");
    zjs_obj_add_string(pin_obj, polarity, "polarity");

    // TODO: When we implement close, we should release the reference on this
    return pin_obj;
}

jerry_value_t zjs_pwm_init()
{
    // effects: finds the PWM driver and registers the PWM JS object
    char devname[10];

    for (int i = 0; i < PWM_DEV_COUNT; i++) {
        sprintf(devname, "PWM_%d", i);
        zjs_pwm_dev[i] = device_get_binding(devname);
        if (!zjs_pwm_dev[i]) {
            PRINT("DEVICE: '%s'\n", devname);
            return zjs_error("zjs_pwm_init: cannot find PWM device");
        }
    }

    // create PWM object
    jerry_value_t pwm_obj = jerry_create_object();
    zjs_obj_add_function(pwm_obj, zjs_pwm_open, "open");
    return pwm_obj;
}
#endif // BUILD_MODULE_PWM
