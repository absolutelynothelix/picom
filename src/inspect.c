// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2024 Yuxuan Shui <yshuiv7@gmail.com>

#include <X11/Xlib.h>
#include <stddef.h>
#include <stdint.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xproto.h>

#include "inspect.h"

#include "c2.h"
#include "common.h"
#include "config.h"
#include "log.h"
#include "wm/defs.h"
#include "wm/win.h"
#include "x.h"

xcb_window_t inspect_select_window(struct x_connection *c) {
	xcb_font_t font = x_new_id(c);
	xcb_cursor_t cursor = x_new_id(c);
	const char font_name[] = "cursor";
	static const uint16_t CROSSHAIR_CHAR = 34;
	XCB_AWAIT_VOID(xcb_open_font, c->c, font, sizeof(font_name) - 1, font_name);
	XCB_AWAIT_VOID(xcb_create_glyph_cursor, c->c, cursor, font, font, CROSSHAIR_CHAR,
	               CROSSHAIR_CHAR + 1, 0, 0, 0, 0xffff, 0xffff, 0xffff);
	auto grab_reply = XCB_AWAIT(
	    xcb_grab_pointer, c->c, false, c->screen_info->root,
	    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE, XCB_GRAB_MODE_SYNC,
	    XCB_GRAB_MODE_ASYNC, c->screen_info->root, cursor, XCB_CURRENT_TIME);
	if (grab_reply->status != XCB_GRAB_STATUS_SUCCESS) {
		log_fatal("Failed to grab pointer");
		return 1;
	}
	free(grab_reply);

	// Let the user pick a window by clicking on it, mostly stolen from
	// xprop
	xcb_window_t target = XCB_NONE;
	int buttons_pressed = 0;
	while ((target == XCB_NONE) || (buttons_pressed > 0)) {
		XCB_AWAIT_VOID(xcb_allow_events, c->c, XCB_ALLOW_ASYNC_POINTER,
		               XCB_CURRENT_TIME);
		xcb_generic_event_t *ev = xcb_wait_for_event(c->c);
		if (!ev) {
			log_fatal("Connection to X server lost");
			return 1;
		}
		switch (XCB_EVENT_RESPONSE_TYPE(ev)) {
		case XCB_BUTTON_PRESS: {
			xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;
			if (target == XCB_NONE) {
				target = e->child;
				if (target == XCB_NONE) {
					target = e->root;
				}
			}
			buttons_pressed++;
			break;
		}
		case XCB_BUTTON_RELEASE: {
			if (buttons_pressed > 0) {
				buttons_pressed--;
			}
			break;
		}
		default: break;
		}
		free(ev);
	}
	XCB_AWAIT_VOID(xcb_ungrab_pointer, c->c, XCB_CURRENT_TIME);
	return target;
}

struct c2_match_state {
	const struct c2_state *state;
	const struct win *w;
	bool print_value;
};

static bool c2_match_once_and_log(const c2_lptr_t *cond, void *data) {
	struct c2_match_state *state = data;
	void *rule_data = NULL;
	printf("    %s ... ", c2_lptr_to_str(cond));
	bool matched = c2_match_one(state->state, state->w, cond, rule_data);
	printf("%s", matched ? "\033[1;32mmatched\033[0m" : "not matched");
	if (state->print_value && matched) {
		printf("/%lu", (unsigned long)(intptr_t)rule_data);
		state->print_value = false;
	}
	printf("\n");
	return false;
}

#define BOLD(str) "\033[1m" str "\033[0m"

void inspect_dump_window(const struct c2_state *state, const struct options *opts,
                         const struct win *w) {
	struct c2_match_state match_state = {
	    .state = state,
	    .w = w,
	};
	printf("Checking " BOLD("transparent-clipping-exclude") ":\n");
	c2_list_foreach(opts->transparent_clipping_blacklist, c2_match_once_and_log,
	                &match_state);
	printf("Checking " BOLD("shadow-exclude") ":\n");
	c2_list_foreach(opts->shadow_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("fade-exclude") ":\n");
	c2_list_foreach(opts->fade_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("clip-shadow-above") ":\n");
	c2_list_foreach(opts->shadow_clip_list, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("focus-exclude") ":\n");
	c2_list_foreach(opts->focus_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("invert-color-include") ":\n");
	c2_list_foreach(opts->invert_color_list, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("blur-background-exclude") ":\n");
	c2_list_foreach(opts->blur_background_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("unredir-if-possible-exclude") ":\n");
	c2_list_foreach(opts->unredir_if_possible_blacklist, c2_match_once_and_log,
	                &match_state);
	printf("Checking " BOLD("rounded-corners-exclude") ":\n");
	c2_list_foreach(opts->rounded_corners_blacklist, c2_match_once_and_log, &match_state);

	match_state.print_value = true;
	printf("Checking " BOLD("opacity-rule") ":\n");
	c2_list_foreach(opts->opacity_rules, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("corner-radius-rule") ":\n");
	c2_list_foreach(opts->corner_radius_rules, c2_match_once_and_log, &match_state);

	printf("\nHere are some rule(s) that match this window:\n");
	if (w->name != NULL) {
		printf("    name = '%s'\n", w->name);
	}
	if (w->class_instance != NULL) {
		printf("    class_i = '%s'\n", w->class_instance);
	}
	if (w->class_general != NULL) {
		printf("    class_g = '%s'\n", w->class_general);
	}
	if (w->role != NULL) {
		printf("    role = '%s'\n", w->role);
	}
	if (w->window_types != 0) {
		for (int i = 0; i < NUM_WINTYPES; i++) {
			if (w->window_types & (1 << i)) {
				printf("    window_type = '%s'\n", WINTYPES[i].name);
			}
		}
	}
	printf("    %sfullscreen\n", w->is_fullscreen ? "" : "! ");
	if (w->bounding_shaped) {
		printf("    bounding_shaped\n");
	}
	printf("    border_width = %d\n", w->g.border_width);
}
