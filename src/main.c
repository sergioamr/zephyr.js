// Copyright (c) 2016, Intel Corporation.
#ifndef ZJS_LINUX_BUILD
// Zephyr includes
#include <zephyr.h>
#include "zjs_zephyr_port.h"
#else
#include "zjs_linux_port.h"
#endif // ZJS_LINUX_BUILD
#include <string.h>
#include "zjs_script.h"

// JerryScript includes
#include "jerry-api.h"

#ifdef ZJS_POOL_CONFIG
#include "zjs_pool.h"
#endif

// Platform agnostic modules/headers
#include "zjs_buffer.h"
#include "zjs_callbacks.h"
#include "zjs_common.h"
#include "zjs_console.h"
#include "zjs_event.h"
#include "zjs_modules.h"
#include "zjs_timers.h"
#include "zjs_util.h"
#ifdef BUILD_MODULE_OCF
#include "zjs_ocf_common.h"
#endif
#ifdef BUILD_MODULE_BLE
#include "zjs_ble.h"
#endif

#define ZJS_MAX_PRINT_SIZE      512

extern const char *script_gen;

// native eval handler
static jerry_value_t native_eval_handler(const jerry_value_t function_obj,
                                         const jerry_value_t this,
                                         const jerry_value_t argv[],
                                         const jerry_length_t argc)
{
    return zjs_error("native_eval_handler: eval not supported");
}

// native print handler
static jerry_value_t native_print_handler(const jerry_value_t function_obj,
                                          const jerry_value_t this,
                                          const jerry_value_t argv[],
                                          const jerry_length_t argc)
{
    jerry_size_t jlen = jerry_get_string_size(argv[0]);
    if (jlen > ZJS_MAX_PRINT_SIZE) {
        ERR_PRINT("maximum print string length exceeded\n");
        return ZJS_UNDEFINED;
    }
    char buffer[jlen + 1];
    int wlen = jerry_string_to_char_buffer(argv[0], (jerry_char_t *)buffer, jlen);
    buffer[wlen] = '\0';

    ZJS_PRINT("%s\n", buffer);
    return ZJS_UNDEFINED;
}

#ifndef ZJS_LINUX_BUILD
void main(void)
#else
int main(int argc, char *argv[])
#endif
{
    const char *script = NULL;
    jerry_value_t code_eval;
    jerry_value_t result;
    uint32_t len;

    // print newline here to make it easier to find
    // the beginning of the program
    ZJS_PRINT("\n");

#ifdef ZJS_POOL_CONFIG
    zjs_init_mem_pools();
#ifdef DUMP_MEM_STATS
    zjs_print_pools();
#endif
#endif

    jerry_init(JERRY_INIT_EMPTY);

    zjs_timers_init();
#ifdef BUILD_MODULE_CONSOLE
    zjs_console_init();
#endif
#ifdef BUILD_MODULE_BUFFER
    zjs_buffer_init();
#endif
    zjs_init_callbacks();

    // initialize modules
    zjs_modules_init();

#ifdef BUILD_MODULE_OCF
    zjs_register_service_routine(NULL, main_poll_routine);
#endif

#ifdef ZJS_LINUX_BUILD
    if (argc > 1) {
        zjs_read_script(argv[1], &script, &len);
    } else
    // slightly tricky: reuse next section as else clause
#endif
    {
        script = script_gen;
        len = strnlen(script_gen, MAX_SCRIPT_SIZE);
        if (len == MAX_SCRIPT_SIZE) {
            ZJS_PRINT("Error: Script size too large! Increase MAX_SCRIPT_SIZE.\n");
            goto error;
        }
    }

    jerry_value_t global_obj = jerry_get_global_object();

    // Todo: find a better solution to disable eval() in JerryScript.
    // For now, just inject our eval() function in the global space
    zjs_obj_add_function(global_obj, native_eval_handler, "eval");
    zjs_obj_add_function(global_obj, native_print_handler, "print");

    code_eval = jerry_parse((jerry_char_t *)script, len, false);
    if (jerry_value_has_error_flag(code_eval)) {
        ZJS_PRINT("JerryScript: cannot parse javascript\n");
        goto error;
    }

#ifdef ZJS_LINUX_BUILD
    if (argc > 1) {
        zjs_free_script(script);
    }
#endif

    result = jerry_run(code_eval);
    if (jerry_value_has_error_flag(result)) {
        ZJS_PRINT("JerryScript: cannot run javascript\n");
        goto error;
    }

    jerry_release_value(global_obj);
    jerry_release_value(code_eval);
    jerry_release_value(result);

#ifndef ZJS_LINUX_BUILD
#ifndef QEMU_BUILD
#ifdef BUILD_MODULE_BLE
    zjs_ble_enable();
#endif
#endif
#endif // ZJS_LINUX_BUILD

    while (1) {
        zjs_timers_process_events();
        zjs_service_callbacks();
        zjs_service_routines();
        // not sure if this is okay, but it seems better to sleep than
        //   busy wait
        zjs_sleep(1);
    }

error:
#ifdef ZJS_LINUX_BUILD
    return 1;
#else
    return;
#endif
}
