// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztphone: Phone, for ztMETRO.
 *
 *   history	the calls made, taken and missed, the newest first. The round
 *		button at a call's left rings that number back; a tap on the
 *		rest opens it (call, text, add to People). Hold one for a
 *		menu.
 *   keypad	beside it (swipe, or tap the title): the number as it is
 *		typed, with the name if People knows it; call, text, save.
 *   (a call)	shown by itself whenever there is one: who, how it stands,
 *		how long; end call, or answer and decline.
 *
 * Calls are the modem daemon's (comm.h: "call", "answer", "hangup" on its
 * control pipe; its report says how the call stands, its log what calls there
 * were). The sound of a call is not routed on this phone yet: a call connects
 * and is silent, and the page says so.
 *
 *	ztphone				history
 *	ztphone NUMBER			the keypad with that number on it
 *	ztphone --call NUMBER		dial it now
 *	ztphone --render PAGE FILE	draw a page into a PNG (history, keypad, call, entry)
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>

#include "zrnmetrocore.h"
#include "comm.h"

#define MARGIN ZMC_MARGIN

enum page { P_HISTORY, P_KEYPAD, P_CALL, P_ENTRY };

enum action {
	ACT_KEY = 1,	/* arg: the character */
	ACT_ERASE,
	ACT_DIAL,
	ACT_TEXT,
	ACT_SAVE,
	ACT_REDIAL,	/* arg: which call of the history */
	ACT_OPEN,	/* arg: which call */
	ACT_ENTRY_CALL,
	ACT_ENTRY_TEXT,
	ACT_ENTRY_SAVE,
	ACT_HANGUP,
	ACT_ANSWER,
	ACT_MENU,
	ACT_MENU_CLOSE,
};

static char digits[40];
static struct call_item calls[MAX_CALLS];
static int n_calls;
static char state[16] = "idle", number[40];	/* the call there is, if any */
static time_t answered_at;
static int came_from = P_HISTORY;	/* where to go back to when a call ends */
static char entry[40];			/* the number the entry page is of */
static int menu_for = -1;
static bool sample;

static bool on_call(void) {
	return state[0] && strcmp(state, "idle") != 0;
}

/* ---- history -------------------------------------------------------------- */

static void describe(const struct call_item *c, char *out, size_t size) {
	char when[24];
	when_text(c->at, when, sizeof(when));
	if (!strcmp(c->type, "missed")) {
		snprintf(out, size, "Missed, %s", when);
	} else if (c->duration > 0) {
		snprintf(out, size, "%s, %s, %ld:%02ld", !strcmp(c->type, "in") ? "Incoming" : "Outgoing", when,
			c->duration / 60, c->duration % 60);
	} else {
		snprintf(out, size, "%s, %s", !strcmp(c->type, "in") ? "Incoming" : "Outgoing, no answer", when);
	}
}

static double draw_history(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "history", "keypad", ZMC_ACT_PIVOT, P_KEYPAD);
	if (!sample) {
		n_calls = calls_load(calls, MAX_CALLS);
	}
	if (n_calls == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "No calls yet. Swipe to the keypad to make one.", FONT_REGULAR, 14, MARGIN, y,
			zmc.width - 2 * MARGIN) + 20;
		return y;
	}
	double menu_at = -1;
	for (int i = 0; i < n_calls; i++) {
		struct call_item *c = &calls[i];
		bool missed = !strcmp(c->type, "missed");
		char detail[80];
		describe(c, detail, sizeof(detail));
		/* the round button rings back; the rest opens the call */
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_round_button(cr, MARGIN + 22, y + 2, "call", "", ACT_REDIAL, i);
		zmc_text_line(cr, name_of(c->number), FONT_SEMILIGHT, 24, MARGIN + 58, y, zmc.width - MARGIN - 58 - 10);
		if (missed) {
			zmc_accent(cr);
		} else {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		}
		zmc_text_line(cr, detail, FONT_REGULAR, 12.5, MARGIN + 58, y + 31, zmc.width - MARGIN - 58 - 10);
		zmc_add_hit(MARGIN + 52, y - 2, zmc.width - MARGIN - 52, 54, ACT_OPEN, i);
		if (i == menu_for) {
			menu_at = y + 52;
		}
		y += 62;
	}
	y += 20;
	if (menu_for >= 0 && menu_at >= 0) {
		static const char *const items[] = { "call", "text", "add to people" };
		bool known = strcmp(name_of(calls[menu_for].number), calls[menu_for].number) != 0;
		zmc_menu(cr, menu_at, items, known ? 2 : 3, ACT_MENU, ACT_MENU_CLOSE);
	}
	return y;
}

/* ---- keypad --------------------------------------------------------------- */

static double draw_keypad(cairo_t *cr) {
	static const struct { char key; const char *letters; } KEYS[12] = {
		{ '1', "" }, { '2', "ABC" }, { '3', "DEF" }, { '4', "GHI" }, { '5', "JKL" }, { '6', "MNO" },
		{ '7', "PQRS" }, { '8', "TUV" }, { '9', "WXYZ" }, { '*', "" }, { '0', "+" }, { '#', "" },
	};
	double y = zmc_pivot_next(cr, "keypad", "history", ZMC_ACT_PIVOT, P_HISTORY);
	y -= 14;
	/* the number so far, and under it whose it is */
	double size = strlen(digits) > 13 ? 26 : 34;
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, digits, FONT_LIGHT, size, MARGIN, y + (34 - size) / 2, zmc.width - 2 * MARGIN - 50);
	if (digits[0]) {
		const char *name = name_of(digits);
		/* backspace: an arrow-shaped key with a cross in it */
		double bx = zmc.width - MARGIN - 40, by = y + 8;
		cairo_set_line_width(cr, 2);
		cairo_move_to(cr, bx, by + 12);
		cairo_line_to(cr, bx + 12, by);
		cairo_line_to(cr, bx + 38, by);
		cairo_line_to(cr, bx + 38, by + 24);
		cairo_line_to(cr, bx + 12, by + 24);
		cairo_close_path(cr);
		cairo_move_to(cr, bx + 18, by + 7);
		cairo_line_to(cr, bx + 28, by + 17);
		cairo_move_to(cr, bx + 28, by + 7);
		cairo_line_to(cr, bx + 18, by + 17);
		cairo_stroke(cr);
		zmc_add_hit(bx - 14, y - 4, 68, 52, ACT_ERASE, 0);
		if (strcmp(name, digits)) {
			zmc_accent(cr);
			zmc_text(cr, name, FONT_REGULAR, 13.5, MARGIN, y + 42, zmc.width - 2 * MARGIN);
		}
	}
	y += 64;
	/* three across, four down, as the phone's */
	const double gap = 4, kw = (zmc.width - 2 * MARGIN - 2 * gap) / 3, kh = 58;
	for (int i = 0; i < 12; i++) {
		double x = MARGIN + (i % 3) * (kw + gap), ky = y + (i / 3) * (kh + gap);
		char label[2] = { KEYS[i].key, 0 };
		cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
		cairo_rectangle(cr, x, ky, kw, kh);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, label, FONT_LIGHT, 28, x + kw / 2, ky + (KEYS[i].letters[0] ? 5 : 11));
		if (KEYS[i].letters[0]) {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
			zmc_text_centred(cr, KEYS[i].letters, FONT_REGULAR, 10.5, x + kw / 2, ky + 39);
		}
		zmc_add_hit(x, ky, kw, kh, ACT_KEY, KEYS[i].key);
	}
	y += 4 * (kh + gap) + 8;
	/* call across the middle, in the accent colour; text and save beside it */
	double cw = zmc.width - 2 * MARGIN - 2 * 62;
	zmc_accent(cr);
	cairo_rectangle(cr, MARGIN + 62, y, cw, 46);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_centred(cr, "call", FONT_SEMILIGHT, 22, zmc.width / 2.0, y + 8);
	zmc_add_hit(MARGIN + 62, y, cw, 46, ACT_DIAL, 0);
	zmc_round_button(cr, MARGIN + 22, y + 1, "message", "", ACT_TEXT, 0);
	zmc_round_button(cr, zmc.width - MARGIN - 22, y + 1, "save", "", ACT_SAVE, 0);
	return y + 60;
}

/* ---- a call --------------------------------------------------------------- */

static double draw_call(cairo_t *cr) {
	bool incoming = !strcmp(state, "incoming"), active = !strcmp(state, "active");
	double y = zmc_header(cr, incoming ? "incoming" : active ? "in call" : "calling");
	const char *who = number[0] ? number : digits;
	const char *name = name_of(who);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, name, FONT_LIGHT, strlen(name) > 12 ? 30 : 40, MARGIN, y - 6, zmc.width - 2 * MARGIN) + 2;
	if (strcmp(name, who)) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, who, FONT_REGULAR, 16, MARGIN, y, 0);
		y += 26;
	}
	char line[48];
	if (active) {
		long s = answered_at ? (long)(time(NULL) - answered_at) : 0;
		snprintf(line, sizeof(line), "%ld:%02ld", s / 60, s % 60);
	} else {
		snprintf(line, sizeof(line), "%s", incoming ? "is calling" : !strcmp(state, "ringing") ? "ringing…" :
			"dialling…");
	}
	zmc_accent(cr);
	zmc_text(cr, line, FONT_SEMILIGHT, 26, MARGIN, y + 8, 0);
	y += 70;

	double w = zmc.width - 2 * MARGIN;
	if (incoming) {
		double half = (w - 8) / 2;
		zmc_accent(cr);
		cairo_rectangle(cr, MARGIN, y, half, 54);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.78, 0.13, 0.13);
		cairo_rectangle(cr, MARGIN + half + 8, y, half, 54);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, "answer", FONT_SEMILIGHT, 22, MARGIN + half / 2, y + 12);
		zmc_text_centred(cr, "decline", FONT_SEMILIGHT, 22, MARGIN + half + 8 + half / 2, y + 12);
		zmc_add_hit(MARGIN, y, half, 54, ACT_ANSWER, 0);
		zmc_add_hit(MARGIN + half + 8, y, half, 54, ACT_HANGUP, 0);
	} else {
		cairo_set_source_rgb(cr, 0.78, 0.13, 0.13);
		cairo_rectangle(cr, MARGIN, y, w, 54);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, "end call", FONT_SEMILIGHT, 22, zmc.width / 2.0, y + 12);
		zmc_add_hit(MARGIN, y, w, 54, ACT_HANGUP, 0);
	}
	y += 76;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "The sound of a call is not connected on this phone yet: the call is made, "
		"and neither side hears the other.", FONT_REGULAR, 12.5, MARGIN, y, w) + 16;
	return y;
}

/* ---- one call of the history ---------------------------------------------- */

static double draw_entry(cairo_t *cr) {
	double y = zmc_header(cr, "call");
	const char *name = name_of(entry);
	bool known = strcmp(name, entry) != 0;
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, name, FONT_LIGHT, 32, MARGIN, y - 8, zmc.width - 2 * MARGIN) + 8;
	y = zmc_entry(cr, y, "call", entry, ACT_ENTRY_CALL, 0);
	y = zmc_entry(cr, y, "text", entry, ACT_ENTRY_TEXT, 0);
	if (!known) {
		y = zmc_entry(cr, y, "add to people", "keep this number under a name", ACT_ENTRY_SAVE, 0);
	}
	/* the calls there have been with this number */
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Calls", FONT_REGULAR, 13, MARGIN, y + 6, 0);
	y += 30;
	for (int i = 0; i < n_calls; i++) {
		char detail[80];
		if (!same_number(calls[i].number, entry) && strcmp(calls[i].number, entry)) {
			continue;
		}
		describe(&calls[i], detail, sizeof(detail));
		if (!strcmp(calls[i].type, "missed")) {
			zmc_accent(cr);
		} else {
			cairo_set_source_rgb(cr, 1, 1, 1);
		}
		zmc_text(cr, detail, FONT_REGULAR, 15, MARGIN, y, zmc.width - 2 * MARGIN);
		y += 26;
	}
	return y + 20;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_HISTORY: return draw_history(cr);
	case P_KEYPAD: return draw_keypad(cr);
	case P_CALL: return draw_call(cr);
	case P_ENTRY: return draw_entry(cr);
	}
	return 0;
}

/* ---- what taps do --------------------------------------------------------- */

static void dial(const char *who) {
	if (!who[0] || on_call()) {
		return;
	}
	if (!modem_tell("call %s", who)) {
		return;		/* no modem daemon: nothing to show a call of */
	}
	snprintf(number, sizeof(number), "%s", who);
	snprintf(state, sizeof(state), "dialing");
	answered_at = 0;
	zmc_haptic("click");
	if (zmc.page != P_CALL) {
		came_from = zmc.page == P_ENTRY ? P_HISTORY : zmc.page;
		zmc_go(P_CALL, 1);
	}
}

static void other_app(const char *app, const char *arg) {
	char command[160], clean[40];
	size_t o = 0;
	for (const char *c = arg; *c && o + 1 < sizeof(clean); c++) {
		if (number_char(*c)) {
			clean[o++] = *c;
		}
	}
	clean[o] = 0;
	snprintf(command, sizeof(command), "%s '%s'", app, clean);
	run_app(command);
}

static void activate(int action, int arg) {
	size_t len = strlen(digits);
	switch (action) {
	case ACT_KEY:
		if (len + 1 < sizeof(digits) - 1) {
			digits[len] = (char)arg;
			digits[len + 1] = 0;
		}
		break;
	case ACT_ERASE:
		if (len) {
			digits[len - 1] = 0;
		}
		break;
	case ACT_DIAL:
		dial(digits);
		break;
	case ACT_TEXT:
		if (digits[0]) {
			other_app("ztmessages", digits);
		}
		break;
	case ACT_SAVE:
		if (digits[0]) {
			other_app("ztcontacts --add", digits);
		}
		break;
	case ACT_REDIAL:
		if (arg >= 0 && arg < n_calls) {
			dial(calls[arg].number);
		}
		break;
	case ACT_OPEN:
		if (arg >= 0 && arg < n_calls) {
			snprintf(entry, sizeof(entry), "%s", calls[arg].number);
			zmc_go(P_ENTRY, 1);
		}
		break;
	case ACT_ENTRY_CALL:
		dial(entry);
		break;
	case ACT_ENTRY_TEXT:
		other_app("ztmessages", entry);
		break;
	case ACT_ENTRY_SAVE:
		other_app("ztcontacts --add", entry);
		break;
	case ACT_HANGUP:
		modem_tell("hangup");
		zmc_haptic("click");
		break;		/* the page goes when the modem says the call is over */
	case ACT_ANSWER:
		modem_tell("answer");
		zmc_haptic("click");
		break;
	case ACT_MENU: {
		int which = menu_for;
		menu_for = -1;
		if (which < 0 || which >= n_calls) {
			break;
		}
		if (arg == 0) {
			dial(calls[which].number);
		} else if (arg == 1) {
			other_app("ztmessages", calls[which].number);
		} else {
			other_app("ztcontacts --add", calls[which].number);
		}
		break;
	}
	case ACT_MENU_CLOSE:
		menu_for = -1;
		break;
	}
}

static bool hold(int action, int arg) {
	if (zmc.page == P_HISTORY && (action == ACT_OPEN || action == ACT_REDIAL) && menu_for < 0) {
		menu_for = arg;
		return true;
	}
	if (zmc.page == P_KEYPAD && action == ACT_KEY && arg == '0') {
		/* held, 0 is the plus of a number from abroad */
		size_t len = strlen(digits);
		if (len + 1 < sizeof(digits) - 1) {
			digits[len] = '+';
			digits[len + 1] = 0;
		}
		return true;
	}
	if (zmc.page == P_KEYPAD && action == ACT_ERASE) {
		digits[0] = 0;	/* held, backspace clears it all */
		return true;
	}
	return false;
}

static void typed(const char *text) {
	if (zmc.page != P_KEYPAD) {
		return;
	}
	if (text[0] == '\b') {
		activate(ACT_ERASE, 0);
	} else if (text[0] == '\n') {
		activate(ACT_DIAL, 0);
	} else if (number_char(text[0])) {
		activate(ACT_KEY, text[0]);
	}
}

static bool back(void) {
	if (menu_for >= 0) {
		menu_for = -1;
		zmc_redraw();
		return true;
	}
	if (zmc.page == P_ENTRY) {
		zmc_go(P_HISTORY, -1);
		return true;
	}
	if (zmc.page == P_CALL) {
		/* the call goes on; back only leaves its page */
		zmc_go(came_from, -1);
		return true;
	}
	return false;
}

/* How the call stands, from the modem daemon's report: once a second. */
static void tick(void) {
	char now[16], who[40];
	bool was = on_call();
	modem_value("call", now, sizeof(now));
	modem_value("call_number", who, sizeof(who));
	if (!now[0]) {
		snprintf(now, sizeof(now), "idle");
	}
	bool changed = strcmp(now, state) != 0;
	/* (just dialled: the daemon may not have said so yet) */
	if (!strcmp(state, "dialing") && !strcmp(now, "idle") && !changed) {
		return;
	}
	snprintf(state, sizeof(state), "%s", now);
	if (who[0]) {
		snprintf(number, sizeof(number), "%s", who);
	}
	if (!strcmp(state, "active") && !answered_at) {
		answered_at = time(NULL);
	}
	if (on_call() && !was && zmc.page != P_CALL) {
		came_from = zmc.page == P_ENTRY ? P_HISTORY : zmc.page;
		zmc_go(P_CALL, 1);
	} else if (!on_call() && was) {
		answered_at = 0;
		number[0] = 0;
		if (zmc.page == P_CALL) {
			zmc_go(came_from, -1);
		} else {
			zmc_redraw();
		}
	} else if (changed || (zmc.page == P_CALL && !strcmp(state, "active"))) {
		zmc_redraw();	/* the state, or the clock of the call */
	}
}

static const struct zmc_app phone = {
	.app_id = "ztphone", .title = "Phone", .caps = "PHONE",
	.render = render, .activate = activate, .back = back, .text = typed, .hold = hold,
};

int main(int argc, char **argv) {
	contacts_load();
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		/* made-up calls: only for looking at the pages */
		static const struct { const char *type; int ago; const char *number; long lasted; } made[] = {
			{ "missed", 1800, "0771234567", 0 }, { "out", 9000, "0112345678", 95 },
			{ "in", 90000, "0771234567", 312 }, { "out", 200000, "0712223334", 0 },
		};
		sample = true;
		snprintf(contacts[0].name, sizeof(contacts[0].name), "Amara Perera");
		snprintf(contacts[0].mobile, sizeof(contacts[0].mobile), "0771234567");
		n_contacts = 1;
		for (size_t i = 0; i < 4; i++) {
			snprintf(calls[i].type, sizeof(calls[i].type), "%s", made[i].type);
			calls[i].at = time(NULL) - made[i].ago;
			snprintf(calls[i].number, sizeof(calls[i].number), "%s", made[i].number);
			calls[i].duration = made[i].lasted;
		}
		n_calls = 4;
		snprintf(digits, sizeof(digits), "0771234567");
		snprintf(entry, sizeof(entry), "0771234567");
		int page = !strcmp(argv[2], "keypad") ? P_KEYPAD : !strcmp(argv[2], "call") ? P_CALL :
			!strcmp(argv[2], "entry") ? P_ENTRY : P_HISTORY;
		if (page == P_CALL) {
			snprintf(state, sizeof(state), "active");
			snprintf(number, sizeof(number), "0771234567");
			answered_at = time(NULL) - 83;
		}
		return zmc_render_png(&phone, page, 664, argv[3]);
	}
	int first = P_HISTORY;
	const char *call_now = NULL;
	if (argc == 3 && !strcmp(argv[1], "--call")) {
		call_now = argv[2];
	} else if (argc == 2 && argv[1][0] != '-') {
		snprintf(digits, sizeof(digits), "%.38s", argv[1]);
		first = P_KEYPAD;
	}
	n_calls = calls_load(calls, MAX_CALLS);
	/* a call already there (it rang, and the app was opened for it) */
	modem_value("call", state, sizeof(state));
	modem_value("call_number", number, sizeof(number));
	if (!state[0]) {
		snprintf(state, sizeof(state), "idle");
	}
	if (call_now && !on_call() && modem_tell("call %s", call_now)) {
		snprintf(number, sizeof(number), "%.38s", call_now);
		snprintf(state, sizeof(state), "dialing");
	}
	if (on_call()) {
		first = P_CALL;
	}
	zmc_every(1000, tick);
	return zmc_run(&phone, first);
}
