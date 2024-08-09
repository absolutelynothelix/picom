// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2024 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <xcb/xcb.h>
struct x_connection;
struct c2_state;
struct options;
struct win;
int inspect_main(int argc, char **argv, const char *config_file);
xcb_window_t inspect_select_window(struct x_connection *c);
void inspect_dump_window(const struct c2_state *state, const struct options *opts,
                         const struct win *w);
