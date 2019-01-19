// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "option.h"
#include "util.h"

#pragma GCC visibility push(hidden)

#include <string.h>

#pragma GCC visibility pop

#define OPTION_DEFAULT(option) \
    case OPTION_##option: value = OPTION_##option##_DEFAULT; break

static void initialize_options(struct options* o) {
    for (enum option i = 0; i < OPTION_MAX; ++i) {
        const char* value = NULL;
        switch (i) {
            OPTION_DEFAULT(FILENAME);
            OPTION_DEFAULT(SHUTDOWN);
            OPTION_DEFAULT(REBOOT);
        case OPTION_MAX:
            __builtin_unreachable();
        }
        o->value[i] = value;
    }
}

#define OPTION_STRING(option)                               \
    case OPTION_##option:                                   \
        string = OPTION_##option##_STRING;                  \
        string_len = sizeof(OPTION_##option##_STRING) - 1;  \
        break

static void apply_option(struct options* o, const char* arg) {
    size_t len = strlen(arg);
    for (enum option i = 0; i < OPTION_MAX; ++i) {
        const char* string = NULL;
        size_t string_len = 0;
        switch (i) {
            OPTION_STRING(FILENAME);
            OPTION_STRING(SHUTDOWN);
            OPTION_STRING(REBOOT);
        case OPTION_MAX:
            __builtin_unreachable();
        }
        if (len > string_len &&
            arg[string_len] == '=' &&
            !strncmp(arg, string, string_len)) {
            o->value[i] = &arg[string_len + 1];
        }
    }
}

void parse_options(zx_handle_t log, struct options *o, char** strings) {
    initialize_options(o);
    for (char** sp = strings; *sp != NULL; ++sp) {
        const char* arg = *sp;
        //printl(log, "option \"%s\"", arg);
        apply_option(o, arg);
    }
}
