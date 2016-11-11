// Copyright (c) 2016, Intel Corporation.

#include <string.h>
#include "zjs_util.h"
#include "zjs_common.h"
#include "zjs_promise.h"
#include "zjs_callbacks.h"

struct promise {
    uint8_t then_set;           // then() function has been set
    jerry_value_t then;         // Function registered from then()
    int32_t then_id;            // Callback ID for then JS callback
    uint8_t catch_set;          // catch() function has been set
    jerry_value_t catch;        // Function registered from catch()
    int32_t catch_id;           // Callback ID for catch JS callback
    jerry_value_t this;         // 'this' object for this promise
    void* user_handle;
    zjs_post_promise_func post;
};

struct promise* new_promise(void)
{
    struct promise* new = zjs_malloc(sizeof(struct promise));
    memset(new, 0, sizeof(struct promise));
    new->catch_id = -1;
    new->then_id = -1;
    return new;
}

// Dummy function for then/catch
static jerry_value_t null_function(const jerry_value_t function_obj,
                                   const jerry_value_t this,
                                   const jerry_value_t argv[],
                                   const jerry_length_t argc)
{
    return ZJS_UNDEFINED;
}

static void post_promise(void* h, jerry_value_t* ret_val)
{
    struct promise* handle = (struct promise*)h;
    if (handle) {
        if (handle->post) {
            handle->post(handle->user_handle);
        }
        jerry_release_value(handle->then);
        jerry_release_value(handle->catch);
        jerry_release_value(handle->this);
    }
}

static void promise_free(const uintptr_t native)
{
    struct promise* handle = (struct promise*)native;
    if (handle) {
        zjs_free(handle);
    }
}

static jerry_value_t promise_then(const jerry_value_t function_obj,
                                   const jerry_value_t this,
                                   const jerry_value_t argv[],
                                   const jerry_length_t argc)
{
    struct promise* handle = NULL;

    jerry_value_t promise_obj = zjs_get_property(this, "promise");
    jerry_get_object_native_handle(promise_obj, (uintptr_t*)&handle);

    jerry_release_value(promise_obj);
    if (jerry_value_is_function(argv[0])) {
        if (handle) {
            jerry_release_value(handle->then);
            handle->then = jerry_acquire_value(argv[0]);
            zjs_edit_js_func(handle->then_id, handle->then);
            handle->then_set = 1;
        }

        // Return the promise so it can be used by catch()
        return this;
    } else {
        return ZJS_UNDEFINED;
    }
}

static jerry_value_t promise_catch(const jerry_value_t function_obj,
                                   const jerry_value_t this,
                                   const jerry_value_t argv[],
                                   const jerry_length_t argc)
{
    struct promise* handle = NULL;

    jerry_value_t promise_obj = zjs_get_property(this, "promise");
    jerry_get_object_native_handle(promise_obj, (uintptr_t*)&handle);

    jerry_release_value(promise_obj);
    if (handle) {
        if (jerry_value_is_function(argv[0])) {
            jerry_release_value(handle->catch);
            handle->catch = jerry_acquire_value(argv[0]);
            zjs_edit_js_func(handle->catch_id, handle->catch);
            handle->catch_set = 1;
        }
    }
    return ZJS_UNDEFINED;
}

void zjs_make_promise(jerry_value_t obj, zjs_post_promise_func post,
                      void* handle)
{
    struct promise* new = new_promise();
    jerry_value_t promise_obj = jerry_create_object();

    zjs_obj_add_function(obj, promise_then, "then");
    zjs_obj_add_function(obj, promise_catch, "catch");
    jerry_set_object_native_handle(promise_obj, (uintptr_t)new, promise_free);

    new->user_handle = handle;
    new->post = post;
    new->then_set = 0;
    new->catch_set = 0;
    new->this = jerry_acquire_value(obj);

    // Add the "promise" object to the object passed as a property, because the
    // object being made to a promise may already have a native handle.
    zjs_obj_add_object(obj, promise_obj, "promise");

    DBG_PRINT("created promise, obj=%lu, promise=%p, handle=%p\n", obj, new,
              handle);
}

void zjs_fulfill_promise(jerry_value_t obj, jerry_value_t argv[], uint32_t argc)
{
    struct promise* handle = NULL;
    jerry_value_t promise_obj = zjs_get_property(obj, "promise");

    if (!jerry_value_is_object(promise_obj)) {
        ERR_PRINT("'promise' not found in object %lu\n", obj);
        return;
    }

    jerry_get_object_native_handle(promise_obj, (uintptr_t*)&handle);

    if (handle) {
        // Put *something* here in case it never gets registered
        if (!handle->then_set) {
            handle->then = jerry_create_external_function(null_function);
        }

        handle->then_id = zjs_add_callback_once(handle->then,
                                                handle->this,
                                                handle,
                                                post_promise);

        zjs_signal_callback(handle->then_id, argv, argc * sizeof(jerry_value_t));

        DBG_PRINT("fulfilling promise, obj=%lu, then_id=%lu, argv=%p, nargs=%lu\n",
                  obj, handle->then_id, argv, argc);
    } else {
        ERR_PRINT("native handle not found\n");
    }

    jerry_release_value(promise_obj);
}

void zjs_reject_promise(jerry_value_t obj, jerry_value_t argv[], uint32_t argc)
{
    struct promise* handle = NULL;
    jerry_value_t promise_obj = zjs_get_property(obj, "promise");

    if (!jerry_value_is_object(promise_obj)) {
        ERR_PRINT("'promise' not found in object %lu\n", obj);
        return;
    }

    jerry_get_object_native_handle(promise_obj, (uintptr_t*)&handle);

    if (handle) {
        // Put *something* here in case it never gets registered
        if (!handle->catch_set) {
            handle->catch = jerry_create_external_function(null_function);
        }

        handle->catch_id = zjs_add_callback_once(handle->catch,
                                                 handle->this,
                                                 handle,
                                                 post_promise);

        zjs_signal_callback(handle->catch_id, argv, argc * sizeof(jerry_value_t));

        DBG_PRINT("rejecting promise, obj=%lu, catch_id=%ld, argv=%p, nargs=%lu\n",
                  obj, handle->catch_id, argv, argc);
    } else {
        ERR_PRINT("native handle not found\n");
    }

    jerry_release_value(promise_obj);
}
