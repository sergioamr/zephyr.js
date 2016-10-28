/*
 * Copyright 2015 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * @brief Shell to keep the different states of the machine
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <atomic.h>
#include <malloc.h>
#include <misc/printk.h>
#include <misc/reboot.h>
#include <ctype.h>
#include <jerry-port.h>

#include "acm-uart.h"
#include "acm-shell.h"
#include "shell-state.h"

#include "file-wrapper.h"
#include "ihex-handler.h"
#include "jerry-code.h"

#ifdef CONFIG_REBOOT
//TODO Waiting for patch https://gerrit.zephyrproject.org/r/#/c/3161/
#include <qm_init.h>
#endif

/**
 * Contains the pointer to the memory where the code will be uploaded
 * using the stub interface at file_code.c
 */
static ZFILE *file_code = NULL;

/* Configuration of the callbacks to be called */
static struct shell_state_config shell = {
    .state_flags = kShellTransferRaw
};

const char ERROR_NOT_RECOGNIZED[] = "Unknown command";
const char ERROR_NOT_ENOUGH_ARGUMENTS[] = "Not enough arguments";
const char ERROR_FILE_NOT_FOUND[] = "File not found";
const char ERROR_EXCEDEED_SIZE[] = "String too long";
const char ERROR_EMPTY_FILE[] = "File empty";
const char ERROR_FAILED_WRITING[] = "Failed writing to disk";

const char MSG_FILE_SAVED[] =
     ANSI_FG_GREEN "Saving file. " ANSI_FG_RESTORE
     "run the 'run' command to see the result";

const char MSG_FILE_ABORTED[] = ANSI_FG_RED "Aborted!";
const char MSG_EXIT[] = ANSI_FG_GREEN "Back to shell!";

const char READY_FOR_RAW_DATA[] =
    "Ready for JavaScript. \r\n" \
    "\tCtrl+Z or Ctrl+D to finish transfer.\r\n" \
    "\tCtrl+X or Ctrl+C to cancel.";

const char MSG_IMMEDIATE_MODE[] =
    "Ready to evaluate JavaScript.\r\n" \
    "\tCtrl+D or Ctrl+C to return to shell.";

const char hex_prompt[] = "[HEX]\r\n";
const char raw_prompt[] = ANSI_FG_YELLOW "RAW> " ANSI_FG_RESTORE;
const char eval_prompt[] = ANSI_FG_GREEN "js> " ANSI_FG_RESTORE;

#define CMD_TRANSFER "transfer"

#define READ_BUFFER_SIZE 4

#ifndef CONFIG_IHEX_UPLOADER_DEBUG
#define DBG(...) { ; }
#else
#define DBG printk
#endif /* CONFIG_IHEX_UPLOADER_DEBUG */

void ashell_print_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (shell.state_flags & kShellTransferIhex)
        printf("[ERROR]");

    vfprintf(stdout, format, args);
    printf("\n");
    va_end(args);
}

int32_t ashell_get_filename_buffer(const char *buf, char *destination)
{
    uint32_t arg_len = 0;
    uint32_t len = strnlen(buf, MAX_FILENAME_SIZE);
    if (len == 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    if (buf[0] == '-')
        return 0;

    ashell_get_next_arg_s(buf, len, destination, MAX_FILENAME_SIZE, &arg_len);

    if (arg_len == 0) {
        *destination = '\0';
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    return arg_len;
}

int32_t ashell_remove_file(char *buf)
{
    char filename[MAX_FILENAME_SIZE];
    if (ashell_get_filename_buffer(buf, filename) <= 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    int res = fs_unlink(filename);
    if (!res)
        return RET_OK;

    ashell_print_error("rm: cannot remove '%s': %d", filename, res);
    return RET_ERROR;
}

int32_t ashell_remove_dir(char *buf)
{
    printf("rmdir: Not implemented \n");
    return RET_OK;
}

int32_t ashell_make_dir(char *buf)
{
    printf("mkdir: Not implemented \n");
    return RET_OK;
}

int32_t ashell_disk_usage(char *buf)
{
    char filename[MAX_FILENAME_SIZE];
    if (ashell_get_filename_buffer(buf, filename) <= 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    ZFILE *file = csopen(filename, "r");
    if (file == NULL) {
        ashell_print_error(ERROR_FILE_NOT_FOUND);
        return RET_ERROR;
    }

    ssize_t size = cssize(file);
    csclose(file);

    printf("%5ld %s\n", size, filename);
    return RET_OK;
}

int32_t ashell_rename(char *buf)
{
    static struct zfs_dirent entry;
    char path_org[MAX_FILENAME_SIZE];
    char path_dest[MAX_FILENAME_SIZE];

    if (ashell_get_filename_buffer(buf, path_org) > 0) {
        /* Check if file or directory */
        if (fs_stat(path_org, &entry)) {
            ashell_print_error("mv: cannot access '%s' no such file or directory", path_org);
            return RET_ERROR;
        }
    }

    /* Tokenize and isolate the command */
    char *next = ashell_get_token_arg(buf);

    if (next == NULL) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    ashell_get_token_arg(buf);

    if (ashell_get_filename_buffer(next, path_dest) > 0) {
        /* Check if file or directory */
        if (!fs_stat(path_dest, &entry)) {
            ashell_print_error("mv: cannot access '%s' file already exists", path_dest);
            return RET_ERROR;
        }
    }

    f_rename(path_org, path_dest);
    return RET_OK;
}

int32_t ashell_javascript_error(char *buf)
{
    printk("[ERROR](%s)\n", buf);
    jerry_port_log(JERRY_LOG_LEVEL_ERROR, "stderr test (%s)\n", buf);
    return 0;
}

int32_t ashell_reboot(char *buf)
{
    acm_println("Rebooting now!");

#ifdef CONFIG_REBOOT
    //TODO Waiting for patch https://gerrit.zephyrproject.org/r/#/c/3161/
    QM_SCSS_PMU->rstc |= QM_COLD_RESET;
#endif
    sys_reboot(SYS_REBOOT_COLD);
    return RET_OK;
}

int32_t ashell_list_dir(char *buf)
{
    char filename[MAX_FILENAME_SIZE];
    static struct zfs_dirent entry;
    int32_t res;
    ZDIR dp;

    *filename = '\0';
    if (ashell_get_filename_buffer(buf, filename) > 0) {
        /* Check if file or directory */
        if (!fs_stat(filename, &entry)) {
            if (entry.type == DIR_ENTRY_FILE) {
                ashell_disk_usage(filename);
                return RET_OK;
            }
        } else {
            ashell_print_error("ls: cannot access %s: no such file or directory", filename);
            return RET_ERROR;
        }
    }

    res = fs_opendir(&dp, filename);
    if (res) {
        ashell_print_error("Error opening dir [%lu]", res);
        return RET_ERROR;
    }

    if (shell.state_flags & !kShellTransferIhex) {
        printf(ANSI_FG_LIGHT_BLUE "      .\n      ..\n" ANSI_FG_RESTORE);
    }

    for (;;) {
        res = fs_readdir(&dp, &entry);

        /* entry.name[0] == 0 means end-of-dir */
        if (res || entry.name[0] == 0) {
            break;
        }
        if (entry.type == DIR_ENTRY_DIR) {
            printf(ANSI_FG_LIGHT_BLUE "%s\n" ANSI_FG_RESTORE, entry.name);
        } else {
            char *p = entry.name;
            for (; *p; ++p)
                *p = tolower((int)*p);

            printf("%5lu %s\n",
                   entry.size, entry.name);
        }
    }

    fs_closedir(&dp);
    return RET_OK;
}

int32_t ashell_print_file(char *buf)
{
    char filename[MAX_FILENAME_SIZE];
    char data[READ_BUFFER_SIZE];
    ZFILE *file;
    size_t count;
    size_t line = 1;

    // Show not printing
    bool hidden = ashell_check_parameter(buf, 'v');
    bool lines = ashell_check_parameter(buf, 'n');
    if (lines)
        DBG(" Print lines \n");

    if (hidden)
        DBG(" Print hidden \n");

    if (ashell_get_filename_buffer(buf, filename) <= 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    if (!csexist(filename)) {
        ashell_print_error(ERROR_FILE_NOT_FOUND);
        return RET_ERROR;
    }

    DBG("Open [%s]\n", filename);
    file = csopen(filename, "r");

    /* Error getting an id for our data storage */
    if (!file) {
        ashell_print_error(ERROR_FILE_NOT_FOUND);
        return RET_ERROR;
    }

    ssize_t size = cssize(file);
    if (size == 0) {
        ashell_print_error(ERROR_EMPTY_FILE);
        csclose(file);
        return RET_OK;
    }

    csseek(file, 0, SEEK_SET);
    if (lines)
        acm_printf("%5d  ", line++);

    do {
        count = csread(data, 4, 1, file);
        for (int t = 0; t < count; t++) {
            uint8_t byte = data[t];
            if (byte == '\n' || byte == '\r') {
                acm_write("\r\n", 2);
                if (lines)
                    acm_printf("%5d  ", line++);

            } else {
                if (hidden && !isprint(byte)) {
                    acm_printf("(%x)", byte);
                } else
                    acm_write(&byte, 1);
            }
        }
    } while (count > 0);

    acm_write("\r\n", 2);
    csclose(file);
    return RET_OK;
}

int32_t ashell_parse_javascript(char *buf)
{
    char filename[MAX_FILENAME_SIZE];
    if (ashell_get_filename_buffer(buf, filename) <= 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    javascript_parse_code(filename);
    return RET_OK;
}

int32_t ashell_run_javascript(char *buf)
{
    char filename[MAX_FILENAME_SIZE];
    if (ashell_get_filename_buffer(buf, filename) <= 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    printk("[RUN][%s]\r\n", filename);

    if (shell.state_flags & kShellTransferIhex) {
        acm_print("[RUN]\n");
    }

    javascript_run_code(filename);
    return RET_OK;
}

int32_t ashell_start_raw_capture(char *filename)
{
    file_code = csopen(filename, "w+");

    /* Error getting an id for our data storage */
    if (!file_code) {
        ashell_print_error(ERROR_FILE_NOT_FOUND);
        return RET_ERROR;
    }
    return RET_OK;
}

int32_t ashell_close_capture()
{
    return csclose(file_code);
}

int32_t ashell_discard_capture()
{
    csclose(file_code);

    //TODO ashell_remove_file(file_code);
    return RET_OK;
}

int32_t ashell_eval_javascript(const char *buf, uint32_t len)
{
    const char *src = buf;

    while (len > 0) {
        uint8_t byte = *buf++;
        if (!isprint(byte)) {
            switch (byte) {
            case ASCII_END_OF_TRANS:
            case ASCII_SUBSTITUTE:
            case ASCII_END_OF_TEXT:
            case ASCII_CANCEL:
                acm_println(MSG_EXIT);
                shell.state_flags &= ~kShellEvalJavascript;
                acm_set_prompt(NULL);
                return RET_OK;
            }
        }
        len--;
    }

    javascript_eval_code(src);
    return RET_OK;
}

int32_t ashell_raw_capture(const char *buf, uint32_t len)
{
    uint8_t eol = '\n';

    while (len > 0) {
        uint8_t byte = *buf++;
        if (!isprint(byte)) {
            switch (byte) {
            case ASCII_END_OF_TRANS:
            case ASCII_SUBSTITUTE:
                acm_println(MSG_FILE_SAVED);
                shell.state_flags &= ~kShellCaptureRaw;
                acm_set_prompt(NULL);
                ashell_close_capture();
                return RET_OK;
                break;
            case ASCII_END_OF_TEXT:
            case ASCII_CANCEL:
                acm_println(MSG_FILE_ABORTED);
                shell.state_flags &= ~kShellCaptureRaw;
                acm_set_prompt(NULL);
                ashell_discard_capture();
                return RET_OK;
            case ASCII_CR:
            case ASCII_IF:
                acm_println("");
                break;
            default:
                printf("%c", byte);
            }
        } else {
            size_t written = cswrite(&byte, 1, 1, file_code);
            if (written == 0) {
                ashell_print_error(ERROR_FAILED_WRITING);
                return RET_ERROR;
            }
            DBG("%c", byte);
        }
        len--;
    }

    cswrite(&eol, 1, 1, file_code);
    return RET_OK_NO_RET;
}

int32_t ashell_read_data(char *buf)
{
    char filename[MAX_FILENAME_SIZE];
    if (shell.state_flags & kShellTransferRaw) {
        if (ashell_get_filename_buffer(buf, filename) <= 0) {
            ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
            return RET_ERROR;
        }

        acm_println(ANSI_CLEAR);
        acm_printf("Saving to '%s' \r\n", filename);
        acm_println(READY_FOR_RAW_DATA);
        acm_set_prompt(raw_prompt);
        shell.state_flags |= kShellCaptureRaw;
        ashell_start_raw_capture(filename);
    }

    if (shell.state_flags & kShellTransferIhex) {
        ashell_process_close();
    }
    return RET_OK;
}

int32_t ashell_js_immediate_mode(char *buf)
{
    shell.state_flags |= kShellEvalJavascript;
    acm_print(ANSI_CLEAR);
    acm_println(MSG_IMMEDIATE_MODE);
    acm_set_prompt(eval_prompt);
    return RET_OK;
}

int32_t ashell_set_transfer_state(char *buf)
{
    char *next;
    if (buf == 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }
    next = ashell_get_token_arg(buf);
    acm_println(buf);

    if (!strcmp("raw", buf)) {
        acm_set_prompt(NULL);
        shell.state_flags |= kShellTransferRaw;
        shell.state_flags &= ~kShellTransferIhex;
        return RET_OK;
    }

    if (!strcmp("ihex", buf)) {
        acm_set_prompt(hex_prompt);
        shell.state_flags |= kShellTransferIhex;
        shell.state_flags &= ~kShellTransferRaw;
        return RET_OK;
    }
    return RET_UNKNOWN;
}

int32_t ashell_set_state(char *buf)
{
    if (buf == 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    char *next = ashell_get_token_arg(buf);
    if (!strcmp(CMD_TRANSFER, buf)) {
        return ashell_set_transfer_state(next);
    }

    return RET_UNKNOWN;
}

int32_t ashell_get_state(char *buf)
{
    if (buf == 0) {
        ashell_print_error(ERROR_NOT_ENOUGH_ARGUMENTS);
        return RET_ERROR;
    }

    ashell_get_token_arg(buf);
    if (!strcmp(CMD_TRANSFER, buf)) {
        DBG("Flags %lu\n", shell.state_flags);

        if (shell.state_flags & kShellTransferRaw)
            acm_println("Raw");

        if (shell.state_flags & kShellTransferIhex)
            acm_println("Ihex");

        return RET_OK;
    }
    return RET_UNKNOWN;
}

int32_t ashell_at(char *buf)
{
    acm_println("OK\r\n");
    return RET_OK;
}

int32_t ashell_test(char *buf)
{
    acm_println("TEST OK\r\n");
    return RET_OK;
}

int32_t ashell_ping(char *buf)
{
    acm_println("[PONG]\r\n");
    return RET_OK;
}

int32_t ashell_clear(char *buf)
{
    if (shell.state_flags & kShellTransferIhex) {
        acm_print("[CLEAR]\n");
    } else {
        acm_print(ANSI_CLEAR);
    }
    return RET_OK;
}

int32_t ashell_stop_javascript(char *buf)
{
    javascript_stop();
    return RET_OK;
}

int32_t ashell_check_control(const char *buf, uint32_t len)
{
    while (len > 0) {
        uint8_t byte = *buf++;
        if (!isprint(byte)) {
            switch (byte) {
                case ASCII_SUBSTITUTE:
                    DBG("<CTRL + Z>");
                    break;

                case ASCII_END_OF_TRANS:
                    DBG("<CTRL + D>");
                    break;
            }
        }
        len--;
    }
    return RET_OK;
}

#define ASHELL_COMMAND(name,syntax,cmd) {name, syntax, cmd}

static const struct ashell_cmd commands[] =
{
    ASHELL_COMMAND("help",  "This help", ashell_help),
    ASHELL_COMMAND("eval",  "Evaluate JavaScript in realtime"                ,ashell_js_immediate_mode),
    ASHELL_COMMAND("clear", "Clear the terminal screen"                      ,ashell_clear),
    ASHELL_COMMAND("load",  "[FILE] Saves the input text into a file"        ,ashell_read_data),
    ASHELL_COMMAND("run",   "[FILE] Runs the JavaScript program in the file" ,ashell_run_javascript),
    ASHELL_COMMAND("parse", "[FILE] Check if the JS syntax is correct"       ,ashell_parse_javascript),
    ASHELL_COMMAND("stop",  "Stops current JavaScript execution"             ,ashell_stop_javascript),

    ASHELL_COMMAND("ls",    "[FILE] List directory contents or file stat"    ,ashell_list_dir),
    ASHELL_COMMAND("cat",   "[FILE] Print the file contents of a file"       ,ashell_print_file),
    ASHELL_COMMAND("du",    "[FILE] Estimate file space usage"               ,ashell_disk_usage),
    ASHELL_COMMAND("rm",    "[FILE] Remove file or directory"                ,ashell_remove_file),
    ASHELL_COMMAND("mv",    "[SOURCE] [DEST] Move a file to destination"     ,ashell_rename),

    ASHELL_COMMAND("rmdir", "[TODO]"                                         ,ashell_remove_dir),
    ASHELL_COMMAND("mkdir", "[TODO]"                                         ,ashell_make_dir),
    ASHELL_COMMAND("test",  "Runs your current test"                         ,ashell_test),
    ASHELL_COMMAND("error", "Prints an error using JerryScript"              ,ashell_javascript_error),
    ASHELL_COMMAND("ping",  "Prints '[PONG]' to check that we are alive"     ,ashell_ping),
    ASHELL_COMMAND("at",    "OK used by the driver when initializing"        ,ashell_at),

    ASHELL_COMMAND("set",   "Sets the input mode for 'load' accept data\r\n\ttransfer raw\r\n\ttransfer ihex\t",ashell_set_state),
    ASHELL_COMMAND("get",   "Get states on the shell"                        ,ashell_get_state),
    ASHELL_COMMAND("reboot","Reboots the device"                             ,ashell_reboot)
};

#define ASHELL_COMMANDS_COUNT (sizeof(commands)/sizeof(*commands))

int32_t ashell_help(char *buf)
{
    acm_println("'A Shell' bash\r\n");
    acm_println("Commands list:");
    for (uint32_t t = 0; t < ASHELL_COMMANDS_COUNT; t++) {
        acm_printf("%8s %s\r\n", commands[t].cmd_name, commands[t].syntax);
    }

    //acm_println("TODO: Read help file per command!");
    return RET_OK;
}

int32_t ashell_main_state(char *buf, uint32_t len)
{
    /* Raw line to be evaluated by JS */
    if (shell.state_flags & kShellEvalJavascript) {
        return ashell_eval_javascript(buf, len);
    }

    /* Capture data into the raw buffer */
    if (shell.state_flags & kShellCaptureRaw) {
        return ashell_raw_capture(buf, len);
    }

    /* Special characters check for ESC, cancel and commands */
    DBG("[BOF]%s[EOF]", buf);
    ashell_check_control(buf, len);

    uint32_t argc = ashell_get_argc(buf, len);
    DBG("[ARGS %u]\n", argc);

    if (argc == 0)
        return 0;

    /* Null terminate again, protect the castle */
    buf[len] = '\0';
    buf = ashell_skip_spaces(buf);
    if (buf == NULL)
        return 0;

    /* Tokenize and isolate the command */
    char *next = ashell_get_token_arg(buf);

    /* Begin command */
    if (shell.state_flags & kShellTransferIhex) {
        acm_print("[BCMD]\n");
    }

    for (uint8_t t = 0; t < ASHELL_COMMANDS_COUNT; t++) {
        if (!strcmp(commands[t].cmd_name, buf)) {
            int32_t res = commands[t].cb(next);
            /* End command */
            if (shell.state_flags & kShellTransferIhex) {
                acm_print("[ECMD]\n");
            }
            return res;
        }
    }

    /* Shell didn't recognize the command */
    if (shell.state_flags & kShellTransferIhex) {
        ashell_print_error("Unknown command");
    } else {
        acm_printf("%s: command not found. \r\n", buf);
        acm_println("Type 'help' for available commands.");
    }
    return RET_UNKNOWN;
}
