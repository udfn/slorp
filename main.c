#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <cairo.h>
#include <string.h>
#include <nwl/nwl.h>
#include <nwl/surface.h>
#include <nwl/cairo.h>
#include <nwl/shm.h>
#include <nwl/seat.h>
#include <sys/mman.h>
#include <pixman.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <pixman.h>
#include <png.h>
#include "config.h"

#if HAVE_JXL
#include <jxl/encode.h>
#endif

#include "xdg-shell.h"
#include "wlr-layer-shell-unstable-v1.h"
#include "wlr-screencopy-unstable-v1.h"

#define min(a,b) a > b ? b : a

struct slorp_box {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	char *name;
};

struct slorp_exts {
	int32_t x;
	int32_t y;
	int32_t x2;
	int32_t y2;
}; // Why not reuse slorp_box?

struct slorp_state {
	struct zwlr_screencopy_manager_v1 *screencopy;
	struct slorp_exts selection;
	bool selecting;
	bool mouse_down;
	bool moving_selection;
	bool has_selection;
	bool has_received_input;
	const char *text;
	struct slorp_box *boxes; // array of slorp_box
	size_t boxes_amount; // amount of boxes
	struct slorp_box *selected_box;
	uint32_t selection_scale;
	struct {
		bool freeze_frame;
		bool show_dimensions;
		bool no_keyboard_interactivity;
		bool constrain_to_output;
		bool output_box_name;
		bool jxl;
		const char *output_file_name; // don't free!
	} options;
};

static struct slorp_state g_slorp = { 0 };

struct slorp_surface_state {
	struct zwlr_screencopy_frame_v1 *screenframe;
	struct nwl_surface nwl;
	struct nwl_cairo_renderer cairo;
	uint32_t buffer_format;
	uint32_t buffer_width;
	uint32_t buffer_height;
	uint32_t buffer_stride;
	uint32_t buffer_flags;

	int shm_fd;
	bool has_bg;
	struct wl_shm_pool *shm_pool;
	size_t shm_size;
	uint8_t *shm_data;
	struct wl_buffer *shm_buffer;

	struct nwl_surface bgsurface;
	struct nwl_output *output;
};


static void destroy_shm_pool(struct slorp_surface_state *slorpsurf) {
	if (slorpsurf->shm_fd) {
		if (slorpsurf->shm_buffer) {
			wl_buffer_destroy(slorpsurf->shm_buffer);
			slorpsurf->shm_buffer = NULL;
		}
		wl_shm_pool_destroy(slorpsurf->shm_pool);
		munmap(slorpsurf->shm_data, slorpsurf->shm_size);
		close(slorpsurf->shm_fd);
		slorpsurf->shm_fd = 0;
	}
}

static void handle_screenframe_buffer(void *data,
		struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height,
		uint32_t stride) {
	struct slorp_surface_state *slorp_surface = data;
	slorp_surface->buffer_format = format;
	slorp_surface->buffer_height = height;
	slorp_surface->buffer_width = width;
	slorp_surface->buffer_stride = stride;
}

static void handle_screenframe_flags(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
	struct slorp_surface_state *slorp_surface = data;
	slorp_surface->buffer_flags = flags;
}

static void handle_screenframe_ready(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t tv_sec_hi,
		uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct slorp_surface_state *slorp_surface = data;
	zwlr_screencopy_frame_v1_destroy(slorp_surface->screenframe);
	slorp_surface->screenframe = NULL;
	nwl_surface_update(&slorp_surface->bgsurface);
	wl_buffer_destroy(slorp_surface->shm_buffer);
	wl_shm_pool_destroy(slorp_surface->shm_pool);
	slorp_surface->shm_buffer = NULL;
	struct wl_region *region = wl_compositor_create_region(slorp_surface->nwl.state->wl.compositor);
	wl_region_add(region, 0, 0, slorp_surface->buffer_width, slorp_surface->buffer_height);
	wl_surface_set_opaque_region(slorp_surface->bgsurface.wl.surface, region);
	wl_region_destroy(region);
	nwl_surface_set_need_update(&slorp_surface->nwl, true);
}

static void handle_screenframe_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct slorp_surface_state *slorp_surface = data;
	zwlr_screencopy_frame_v1_destroy(slorp_surface->screenframe);
	slorp_surface->screenframe = NULL;
}
static void handle_screenframe_damage(void *data,
		struct zwlr_screencopy_frame_v1 *zwlr_screencopy_frame_v1,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	// don't care
}
static void handle_screenframe_linux_dmabuf(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
		uint32_t width, uint32_t height) {
	// don't care
}
static void handle_screenframe_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	// Keep it stupid simple, don't do dmabuf.. yet
	struct slorp_surface_state *slorp_surface = data;
	size_t pool_size = slorp_surface->buffer_height * slorp_surface->buffer_stride;
	if (pool_size != slorp_surface->shm_size) {
		destroy_shm_pool(slorp_surface);
		slorp_surface->shm_fd = nwl_allocate_shm_file(pool_size);
		slorp_surface->shm_data = mmap(NULL, pool_size, PROT_READ|PROT_WRITE, MAP_SHARED, slorp_surface->shm_fd, 0);
		slorp_surface->shm_pool = wl_shm_create_pool(slorp_surface->nwl.state->wl.shm, slorp_surface->shm_fd, pool_size);
		slorp_surface->shm_size = pool_size;
	}
	slorp_surface->shm_buffer = wl_shm_pool_create_buffer(slorp_surface->shm_pool, 0, slorp_surface->buffer_width, slorp_surface->buffer_height, slorp_surface->buffer_stride, slorp_surface->buffer_format);
	zwlr_screencopy_frame_v1_copy(frame, slorp_surface->shm_buffer);
}

static struct zwlr_screencopy_frame_v1_listener screenframe_listener = {
	handle_screenframe_buffer,
	handle_screenframe_flags,
	handle_screenframe_ready,
	handle_screenframe_failed,
	handle_screenframe_damage,
	handle_screenframe_linux_dmabuf,
	handle_screenframe_buffer_done
};

static void get_rectangle(int32_t *x, int32_t *y, int32_t *x2, int32_t *y2) {
	int32_t y2_temp = *y2;
	int32_t x2_temp = *x2;
	*x2 = *x2 - *x;
	*y2 = *y2 - *y;
	*x = min(*x, x2_temp);
	*y = min(*y, y2_temp);
	if (*y2 < 0) {
		*y2 = -*y2;
	}
	if (*x2 < 0) {
		*x2 = -*x2;
	}
}

// yoinked from slurp because my brain wasn't working at the time :)
static bool is_box_in_box(struct slorp_box *one, struct slorp_box *two) {
	return one->x < two->x + two->width && one->x + one->width > two->x &&
		one->y < two->y + two->height && one->height + one->y > two->y;
}

static bool is_point_in_box(struct slorp_box *box, int32_t x, int32_t y) {
	return x >= box->x && x <= box->x+box->width &&
		y >= box->y && y <= box->y+box->height;
}

static void slorp_sel_update(struct nwl_surface *surface) {
	struct slorp_surface_state *slorp_surface = wl_container_of(surface, slorp_surface, nwl);
	if (g_slorp.options.freeze_frame && !slorp_surface->has_bg) {
		return; // no freezeframe, don't render yet!
	}
	struct nwl_cairo_surface *cairo_surface = nwl_cairo_renderer_get_surface(&slorp_surface->cairo, surface, false);
	cairo_t *cr = cairo_surface->ctx;
	cairo_identity_matrix(cr);
	if (g_slorp.boxes_amount > 0 && !g_slorp.has_received_input) {
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		nwl_cairo_renderer_submit(&slorp_surface->cairo, surface, 0, 0);
		return;
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.145);
	cairo_paint(cr);
	cairo_identity_matrix(cr);
	cairo_scale(cr, surface->scale, surface->scale);
	if (g_slorp.selecting || g_slorp.selected_box) {
		struct slorp_box outputbox = {
			.x = slorp_surface->output->x,
			.y = slorp_surface->output->y,
			.width = slorp_surface->output->width,
			.height = slorp_surface->output->height
		};
		struct slorp_box selectionbox;
		if (g_slorp.selecting) {
			selectionbox.x = g_slorp.selection.x;
			selectionbox.y = g_slorp.selection.y;
			selectionbox.width = g_slorp.selection.x2;
			selectionbox.height = g_slorp.selection.y2;
			get_rectangle(&selectionbox.x, &selectionbox.y, &selectionbox.width, &selectionbox.height);
		} else if (g_slorp.selected_box) {
			selectionbox.x = g_slorp.selected_box->x;
			selectionbox.y = g_slorp.selected_box->y;
			selectionbox.width = g_slorp.selected_box->width;
			selectionbox.height = g_slorp.selected_box->height;
		}
		// Don't bother if it's outside
		if (is_box_in_box(&selectionbox, &outputbox)) {
			selectionbox.x -= outputbox.x;
			selectionbox.y -= outputbox.y;
			cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
			cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
			cairo_rectangle(cr, selectionbox.x, selectionbox.y, selectionbox.width, selectionbox.height);
			cairo_fill(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
			cairo_set_source_rgba(cr, 0.5, 0.75, 0.5, 0.9);
			cairo_rectangle(cr, selectionbox.x, selectionbox.y, selectionbox.width, selectionbox.height);
			cairo_stroke(cr);
			// If not in selection mode, but mouse is dragging a box, then show it, but red..
			if (!g_slorp.selecting && g_slorp.mouse_down) {
				struct slorp_box invalid_box = {
					.x = g_slorp.selection.x,
					.y = g_slorp.selection.y,
					.width = g_slorp.selection.x2,
					.height = g_slorp.selection.y2
				};
				get_rectangle(&invalid_box.x, &invalid_box.y, &invalid_box.width, &invalid_box.height);
				if (is_box_in_box(&invalid_box, &outputbox)) {
					invalid_box.x -= outputbox.x;
					invalid_box.y -= outputbox.y;
					cairo_set_source_rgba(cr, 0.75, 0.35, 0.35, 0.9);
					cairo_rectangle(cr, invalid_box.x, invalid_box.y, invalid_box.width, invalid_box.height);
					cairo_stroke(cr);
				}
			}
			if (g_slorp.options.show_dimensions) {
				char buf[64];
				snprintf(buf, 64, "%d,%d %dx%d", selectionbox.x, selectionbox.y,
					selectionbox.width, selectionbox.height);
				cairo_text_extents_t exts;
				cairo_set_font_size(cr, 14);
				cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
				cairo_text_extents(cr, buf, &exts);
				double x_advance = exts.width;
				int text_lines = 1;
				if (g_slorp.selected_box && g_slorp.selected_box->name) {
					cairo_text_extents(cr, g_slorp.selected_box->name, &exts);
					if (x_advance < exts.x_advance) {
						x_advance = exts.width;
					}
					text_lines = 2;
				}
				x_advance += 6;
				cairo_set_source_rgba(cr, 0.05, 0.05, 0.05, 0.72);
				int y_end = selectionbox.y+selectionbox.height;
				int boxheight = 6 + (16*text_lines);
				int x_end = selectionbox.x+selectionbox.width;
				int x_offset = 10;
				int y_offset = 30;
				if (y_end > outputbox.height - (boxheight+20)) {
					y_offset = -boxheight + 10;
				}
				if (x_end+x_offset+x_advance+14 > outputbox.width) {
					x_offset = -6 - x_advance;
				}
				cairo_rectangle(cr, x_end + (x_offset - 2),
					y_end + (y_offset - 16), x_advance + 2, boxheight);
				cairo_fill(cr);
				cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.97);
				cairo_move_to(cr, x_end + x_offset, y_end + y_offset);
				cairo_show_text(cr, buf);
				if (text_lines > 1) {
					cairo_set_source_rgba(cr, 0.65, 0.65, 0.65, 0.97);
					cairo_move_to(cr, x_end + x_offset, y_end + y_offset + 16);
					cairo_show_text(cr, g_slorp.selected_box->name);
				}
			}
		}
	}
	if (g_slorp.text) {
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, 20);
		cairo_text_extents_t exts;
		cairo_text_extents(cr, g_slorp.text, &exts);
		exts.x_advance += 6;
		cairo_set_source_rgba(cr, 0.05, 0.05, 0.05, 0.75);
		cairo_rectangle(cr, 18, 20, exts.x_advance, 28);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.97);
		cairo_move_to(cr, 20, 40);
		cairo_show_text(cr, g_slorp.text);
	}
	nwl_cairo_renderer_submit(&slorp_surface->cairo, surface, 0, 0);
}

static void handle_pointer(struct nwl_surface *surface, struct nwl_seat *seat, struct nwl_pointer_event *event) {
	struct slorp_surface_state *slorp_surface = wl_container_of(surface, slorp_surface, nwl);
	bool redraw = false;
	// Hack: do nothing if we're done
	if (surface->state->num_surfaces == 0) {
		return;
	}
	if (event->changed & NWL_POINTER_EVENT_FOCUS && event->focus) {
		if (!g_slorp.has_received_input && g_slorp.boxes_amount > 0) {
			g_slorp.has_received_input = true;
			nwl_surface_set_need_update(surface, false);
		}
		nwl_seat_set_pointer_cursor(seat, "cross");
	}
	struct slorp_box cur_exts = {
		.x = g_slorp.selection.x,
		.width = g_slorp.selection.x2,
		.y = g_slorp.selection.y,
		.height = g_slorp.selection.y2,
	};

	if (event->changed & NWL_POINTER_EVENT_BUTTON) {
		 if (event->buttons & NWL_MOUSE_LEFT && !g_slorp.mouse_down) {
			g_slorp.mouse_down = true;
			g_slorp.selection.x = wl_fixed_to_int(event->surface_x) + slorp_surface->output->x;
			g_slorp.selection.y = wl_fixed_to_int(event->surface_y) + slorp_surface->output->y;
			g_slorp.selection.x2 = g_slorp.selection.x;
			g_slorp.selection.y2 = g_slorp.selection.y;
			redraw = true;
		} else if (!(event->buttons & NWL_MOUSE_LEFT) && g_slorp.mouse_down) {
			// hack: If selection was too small, select current box instead if possible
			if (!g_slorp.selecting && g_slorp.selected_box) {
				g_slorp.selection.x = g_slorp.selected_box->x;
				g_slorp.selection.y = g_slorp.selected_box->y;
				g_slorp.selection.x2 = g_slorp.selected_box->width + g_slorp.selection.x;
				g_slorp.selection.y2 = g_slorp.selected_box->height + g_slorp.selection.y;
			}
			g_slorp.selecting = false;
			g_slorp.has_selection = true;
			g_slorp.mouse_down = false;
			surface->state->num_surfaces = 0;
			return;
		} else if (!(event->buttons & NWL_MOUSE_RIGHT) && (event->buttons_prev & NWL_MOUSE_RIGHT)) {
			// rmb: abort
			surface->state->num_surfaces = 0;
			return;
		}
		g_slorp.moving_selection = event->buttons & NWL_MOUSE_MIDDLE;
	}
	if (event->changed & NWL_POINTER_EVENT_MOTION) {
		if (g_slorp.mouse_down) {
			int32_t x = wl_fixed_to_int(event->surface_x);
			int32_t y = wl_fixed_to_int(event->surface_y);
			if (g_slorp.options.constrain_to_output) {
				if (x < 0) {
					x = 0;
				} else if (x > (int32_t)surface->width) {
					x = surface->width;
				}
				if (y < 0) {
					y = 0;
				} else if (y > (int32_t)surface->height) {
					y = surface->height;
				}
			}
			// ugly hack so the whole screen is selectable :/
			if (x == (int32_t)surface->width-1) {
				x = surface->width;
			}
			if (y == (int32_t)surface->height-1) {
				y = surface->height;
			}
			if (g_slorp.moving_selection) {
				g_slorp.selection.x += x + slorp_surface->output->x - g_slorp.selection.x2;
				g_slorp.selection.y += y + slorp_surface->output->y - g_slorp.selection.y2;
				if (g_slorp.options.constrain_to_output) {
					if (g_slorp.selection.x < slorp_surface->output->x) {
						g_slorp.selection.x = slorp_surface->output->x;
					} else if (g_slorp.selection.x > slorp_surface->output->x + slorp_surface->output->width) {
						g_slorp.selection.x = slorp_surface->output->x + slorp_surface->output->width;
					}
					if (g_slorp.selection.y < slorp_surface->output->y) {
						g_slorp.selection.y = slorp_surface->output->y;
					} else if (g_slorp.selection.y > slorp_surface->output->y + slorp_surface->output->height) {
						g_slorp.selection.y = slorp_surface->output->y + slorp_surface->output->height;
					}
				}
			}
			g_slorp.selection.x2 = x + slorp_surface->output->x;
			g_slorp.selection.y2 = y + slorp_surface->output->y;
			// Only initiate selection mode if the box is big enough, because there's probably very little need for tiny boxes
			// .. unless there's a need for pixel selection?
			// ... but allow small selections if there are no predetermined boxes (why?)
			if ((abs(g_slorp.selection.x - g_slorp.selection.x2) > 8 &&
					abs(g_slorp.selection.y - g_slorp.selection.y2) > 8) ||
					g_slorp.boxes_amount == 0) {
				g_slorp.selecting = true;
			} else {
				g_slorp.selecting = false;
			}
			redraw = true;
		} else if (g_slorp.boxes_amount) {
			// Find a box and highlight it
			struct slorp_box *box;
			struct slorp_box *cur_box = g_slorp.selected_box;
			int32_t global_x = wl_fixed_to_int(event->surface_x) + slorp_surface->output->x;
			int32_t global_y = wl_fixed_to_int(event->surface_y) + slorp_surface->output->y;
			for (size_t i = g_slorp.boxes_amount; i > 0; i--) {
				box = &g_slorp.boxes[i-1];
				if (is_point_in_box(box, global_x, global_y)) {
					g_slorp.selected_box = box;
					break;
				}
			}
			if (g_slorp.selected_box != cur_box) {
				if (g_slorp.selected_box) {
					g_slorp.selection.x = g_slorp.selected_box->x;
					g_slorp.selection.y = g_slorp.selected_box->y;
					g_slorp.selection.x2 = g_slorp.selected_box->width + g_slorp.selection.x;
					g_slorp.selection.y2 = g_slorp.selected_box->height + g_slorp.selection.y;
				}
				redraw = true;
			}
		}
	}
	if (redraw) {
		struct nwl_surface *todirt;
		struct slorp_box sel_box = {
			.x = g_slorp.selection.x,
			.y = g_slorp.selection.y,
			.width = g_slorp.selection.x2,
			.height = g_slorp.selection.y2
		};
		get_rectangle(&sel_box.x, &sel_box.y, &sel_box.width, &sel_box.height);
		// See if we're selecting in a box
		if (g_slorp.mouse_down && g_slorp.boxes_amount) {
			// midpoint
			int32_t x = sel_box.x + sel_box.width/2;
			int32_t y = sel_box.y + sel_box.height/2;
			for (size_t i = g_slorp.boxes_amount; i > 0; i--) {
				struct slorp_box *box = &g_slorp.boxes[i-1];
				if (is_point_in_box(box, x, y)) {
					g_slorp.selected_box = box;
					break;
				}
			}
		}
		get_rectangle(&cur_exts.x, &cur_exts.y, &cur_exts.width, &cur_exts.height);
		uint32_t scale = 1;
		wl_list_for_each(todirt, &surface->state->surfaces, link) {
			struct slorp_surface_state *todirtstate = wl_container_of(surface, slorp_surface, nwl);
			struct slorp_box outputexts = {
				.x = todirtstate->output->x,
				.y = todirtstate->output->y,
				.width = todirtstate->output->width,
				.height = todirtstate->output->height,
			};
			if (is_box_in_box(&sel_box, &outputexts) || is_box_in_box(&cur_exts, &outputexts)) {
				scale = todirt->scale > scale ? todirt->scale : scale;
				nwl_surface_set_need_update(todirt, true);
			}
		}
		g_slorp.selection_scale = scale;
	}
}

static void handle_destroy(struct nwl_surface *surface) {
	struct slorp_surface_state *slorp_surface = wl_container_of(surface, slorp_surface, nwl);
	if (slorp_surface->shm_fd) {
		close(slorp_surface->shm_fd);
		munmap(slorp_surface->shm_data, slorp_surface->shm_size);
	}
	nwl_cairo_renderer_finish(&slorp_surface->cairo);
	free(slorp_surface);
}

static void update_bgsurface(struct nwl_surface *surface) {
	struct slorp_surface_state *dat = wl_container_of(surface, dat, bgsurface);
	if (dat->has_bg && !dat->shm_buffer) {
		return;
	}
	wl_surface_set_buffer_scale(surface->wl.surface, surface->scale);
	wl_surface_attach(surface->wl.surface, dat->shm_buffer, 0, 0);
	wl_surface_damage_buffer(surface->wl.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface->wl.surface);
	dat->has_bg = true;
}


static void init_slorp_surface(struct nwl_state *state, struct nwl_output *output) {
	struct slorp_surface_state *surface_state = calloc(sizeof(struct slorp_surface_state), 1);
	nwl_surface_init(&surface_state->nwl, state, "slorp");
	nwl_cairo_renderer_init(&surface_state->cairo);
	nwl_surface_role_layershell(&surface_state->nwl, output->output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
	zwlr_layer_surface_v1_set_anchor(surface_state->nwl.role.layer.wl, 15); // all!
	nwl_surface_set_size(&surface_state->nwl, 0, 0);
	zwlr_layer_surface_v1_set_exclusive_zone(surface_state->nwl.role.layer.wl, -1);
	if (!g_slorp.options.no_keyboard_interactivity) {
		zwlr_layer_surface_v1_set_keyboard_interactivity(surface_state->nwl.role.layer.wl, 1);
	}
	surface_state->nwl.impl.input_pointer = handle_pointer;
	surface_state->nwl.impl.destroy = handle_destroy;
	surface_state->nwl.impl.update = slorp_sel_update;
	surface_state->output = output;
	surface_state->nwl.flags |= NWL_SURFACE_FLAG_NO_AUTOCURSOR;
	surface_state->nwl.scale = output->scale;
	if (g_slorp.screencopy) {
		nwl_surface_init(&surface_state->bgsurface, state, "slorp static bg");
		surface_state->bgsurface.scale = output->scale;
		nwl_surface_role_subsurface(&surface_state->bgsurface, &surface_state->nwl);
		wl_subsurface_place_below(surface_state->bgsurface.role.subsurface.wl, surface_state->nwl.wl.surface);
		surface_state->bgsurface.impl.update = update_bgsurface;
		surface_state->screenframe = zwlr_screencopy_manager_v1_capture_output(g_slorp.screencopy, 0, output->output);
		zwlr_screencopy_frame_v1_add_listener(surface_state->screenframe, &screenframe_listener, surface_state);
	}
	wl_surface_commit(surface_state->nwl.wl.surface);
}

static bool handle_global_add(struct nwl_state *state, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	UNUSED(state);
	if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		g_slorp.screencopy = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, version);
		return true;
	}
	return false;
}

static void handle_global_bound(const struct nwl_bound_global *global) {
	if (global->kind == NWL_BOUND_GLOBAL_OUTPUT) {
		struct nwl_output *output = global->global.output;
		init_slorp_surface(output->state, output);
	}
}

static void print_usage(const char *arg) {
	printf("Usage: %s [options...]\n\n", arg);
	puts( "-F           Freeze frame.");
	puts( "-h           This help.");
	puts( "-d           Show dimensions while selecting.");
	puts( "-m text      Text to show.");
	puts( "-k           Don't take keyboard input.");
	puts( "-l           Constrain selection to one output.");
	puts( "-O filename  Save capture to this file. Implies -F.");
#if HAVE_JXL
	puts( "-X           Save capture in JPEG XL format.");
#endif
}

static void process_input() {
	char buf[4096];
	size_t amt;
	// todo: handle giant inputs that won't fit in this buf
	// .. also maybe support adding and removing boxes at runtime?
	if ((amt = read(STDIN_FILENO, buf, 4096)) > 0) {
		// It's not necessarily null terminated.. So make sure it is, just in case
		buf[amt] = '\0';
	}
	if (amt <= 0) {
		return;
	}
	char *cur = buf;
	unsigned int num_boxes = 0;
	size_t array_size = 0;
	int rectslen;
	while (cur != NULL) {
		// Probably shouldn't use scanf..
		struct slorp_box box = { 0 };
		if (sscanf(cur, "%i,%i %ix%i%n\n", &box.x, &box.y, &box.width, &box.height, &rectslen) != 4) {
			// Invalid input, probably. Stop!
			if (num_boxes) {
				return;
			}
			// Give up completely if there were no valid boxes
			fprintf(stderr, "Invalid input!\n");
			exit(1);
		}
		if (num_boxes == array_size) {
			g_slorp.boxes = realloc(g_slorp.boxes, sizeof(struct slorp_box)*(array_size + 5));
			array_size += 5;
		}
		memcpy(&g_slorp.boxes[num_boxes], &box, sizeof(struct slorp_box));
		g_slorp.boxes_amount = ++num_boxes;
		char *name_start = rectslen+cur;
		cur++;
		cur = strchr(cur, '\n');
		if (cur != name_start) {
			// Should probably keep going until there's no whitespace
			if (*name_start == ' ') {
				name_start++;
				if (*name_start == '\0') {
					// Safety? Is this needed?
					continue;
				}
			}
			char *duped = strndup(name_start, cur - name_start);
			// num_boxes was already incremented, so do this sketchy thing
			g_slorp.boxes[num_boxes-1].name = duped;
		}
	}
}

static pixman_format_code_t wayland_pixformat_to_pixman(enum wl_shm_format format) {
	// bgr is actually rgb?
	switch (format) {
		case WL_SHM_FORMAT_XRGB8888:
			return PIXMAN_x8b8g8r8;
		case WL_SHM_FORMAT_XBGR8888:
			return PIXMAN_x8r8g8b8;
		case WL_SHM_FORMAT_XRGB2101010:
			return PIXMAN_x2b10g10r10;
		case WL_SHM_FORMAT_XBGR2101010:
			return PIXMAN_x2r10g10b10;
		default: // will probably not work, but try anyway
			return PIXMAN_x8r8g8b8;
	}
}

static void unmap_all_surfaces(struct nwl_state *state) {
	// Immediately unmap all surfaces to appear more responsive
	struct nwl_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		wl_surface_attach(surface->wl.surface, NULL, 0, 0);
		// Don't forget subsurfaces!
		struct nwl_surface *subsurface;
		wl_list_for_each(subsurface, &surface->subsurfaces, link) {
			wl_surface_attach(subsurface->wl.surface, NULL, 0, 0);
			wl_surface_commit(subsurface->wl.surface);
		}
		wl_surface_commit(surface->wl.surface);
	}
	// Flushing is important too since we're not in the mainloop
	wl_display_flush(state->wl.display);
}

static void scale_image_size(struct nwl_state *state, struct slorp_box *selectionbox, uint32_t *width, uint32_t *height) {
	struct nwl_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		struct slorp_surface_state *slorp_surface = wl_container_of(surface, slorp_surface, nwl);
		struct slorp_box outputbox = {
			.x = slorp_surface->output->x,
			.y = slorp_surface->output->y,
			.width = slorp_surface->output->width,
			.height = slorp_surface->output->height
		};
		if (is_box_in_box(selectionbox, &outputbox)) {
			// How much of it is in there?
			int32_t local_x = selectionbox->x - outputbox.x;
			int32_t local_y = selectionbox->y - outputbox.y;
			int32_t local_width = selectionbox->width;
			int32_t local_height = selectionbox->height;
			if (local_x < 0) {
				local_width += local_x;
				local_x = 0;
			}
			if (local_y < 0) {
				local_height += local_y;
				local_y = 0;
			}
			int32_t diff_w = outputbox.width - (local_x + local_width);
			int32_t diff_h = outputbox.height - (local_y + local_height);
			if (diff_w < 0) {
				local_width += diff_w;
			}
			if (diff_h < 0) {
				local_height += diff_h;
			}
			*width += local_width*slorp_surface->output->scale;
			*height += local_height*slorp_surface->output->scale; 
		}
	}
}

bool save_image_png(const char* filename, uint32_t* bits, uint32_t width, uint32_t height) {
	png_image png = { 0 };
	png.width = width;
	png.height = height;
	png.format = PNG_FORMAT_RGB;
	png.version = PNG_IMAGE_VERSION;
	int ret = png_image_write_to_file(&png, filename, false, bits, width * 4, 0);
	return ret == 1;
}

#if HAVE_JXL
bool save_image_jxl(const char* filename, uint32_t* bits, uint32_t width, uint32_t height) {
	bool retval = false;
	JxlBasicInfo info;
	JxlEncoderInitBasicInfo(&info);
	info.xsize = width;
	info.ysize = height;
	info.bits_per_sample = 8;
	info.num_color_channels = 3;
	// todo: Figure out how to make jxl not need this extra channel.
	// Also lossless.
	info.num_extra_channels = 1;
	info.uses_original_profile = 0;
	info.exponent_bits_per_sample = 0;
	JxlEncoder *enc = JxlEncoderCreate(NULL);
	if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) {
		// Ugh, goto. I'd rather have defer..
		goto cleanup;
	}
	JxlPixelFormat pixel_format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
	JxlEncoderFrameSettings *setts = JxlEncoderFrameSettingsCreate(enc, NULL);
	if (JxlEncoderAddImageFrame(setts, &pixel_format, bits, 4 * width * height) != JXL_ENC_SUCCESS) {
		goto cleanup;
	}
	JxlEncoderCloseInput(enc);
	FILE *outfile = fopen(filename, "wb");
	if (outfile == NULL) {
		goto cleanup;
	}
	uint8_t outbuf[4096];
	size_t avail_out = 4096;
	JxlEncoderStatus proc_result = JXL_ENC_NEED_MORE_OUTPUT;
	while (proc_result == JXL_ENC_NEED_MORE_OUTPUT) {
		uint8_t *bufpos = &outbuf[0];
		proc_result = JxlEncoderProcessOutput(enc, &bufpos, &avail_out);
		if (avail_out != 4096) {
			fwrite(outbuf, 4096 - avail_out, 1, outfile);
		}
		avail_out = 4096;
	}
	fclose(outfile);
	retval = true;
cleanup:
	JxlEncoderDestroy(enc);
	return retval;
}
#endif

static char *strdupfilename(const char *src) {
	unsigned long len = strlen(src);
	char *dest = calloc(len+1, sizeof(char));
	unsigned long dest_pos = 0;
	for (unsigned long i = 0; i < len; i++) {
		char cur = src[i];
		if (cur == '/') {
			continue;
		}
		dest[dest_pos++] = cur;
	}
	return dest;
}

int main (int argc, char *argv[]) {
	// why set app id when it's not even used?
	struct nwl_state state = {
		.xdg_app_id = "slorp",
		.events = {
			.global_bound = handle_global_bound
		}
	};
	int opt;
	// All the options from slurp are here, to be implemented eventually, maybe..
	while ((opt = getopt(argc, argv, "XkhdonprlFO:b:c:s:B:w:f:m:")) != -1) {
		switch (opt) {
			case 'h':
				print_usage(argv[0]);
				return EXIT_SUCCESS;
			case 'd':
				g_slorp.options.show_dimensions = true;
				break;
			case 'm':
				g_slorp.text = optarg;
				break;
			case 'k':
				g_slorp.options.no_keyboard_interactivity = true;
				break;
			case 'l':
				g_slorp.options.constrain_to_output = true;
				break;
			case 'n':
				g_slorp.options.output_box_name = true;
				break;
			case 'O':
				g_slorp.options.output_file_name = optarg;
			case 'F':
				state.events.global_add = handle_global_add;
				g_slorp.options.freeze_frame = true;
				break;
			case 'X':
			#if HAVE_JXL
				g_slorp.options.jxl = true;
			#else
				fprintf(stderr, "libjxl support not compiled in!\n");
				return EXIT_FAILURE;
			#endif
				break;
			case '?':
				return EXIT_FAILURE;
			default:
				fprintf(stderr, "option %c not yet implemented..\n", opt);
				break;
		}
	}
	if (!isatty(STDIN_FILENO)) {
		process_input();
	}
	if (nwl_wayland_init(&state)) {
		return EXIT_FAILURE;
	}
	nwl_wayland_run(&state);
	if (g_slorp.screencopy) {
		zwlr_screencopy_manager_v1_destroy(g_slorp.screencopy);
	}
	int retval = EXIT_SUCCESS;
	if (g_slorp.has_selection) {
		// Unmap all surfaces here for a better snappier experience
		unmap_all_surfaces(&state);
		struct slorp_box selectionbox = {
			.x = g_slorp.selection.x,
			.y = g_slorp.selection.y,
			.width = g_slorp.selection.x2,
			.height = g_slorp.selection.y2
		};
		get_rectangle(&selectionbox.x, &selectionbox.y, &selectionbox.width, &selectionbox.height);
		if (g_slorp.options.output_file_name) {
			uint32_t scaled_width = 0;
			uint32_t scaled_height = 0;
			scale_image_size(&state, &selectionbox, &scaled_width, &scaled_height);
			uint32_t *pixbits = malloc(sizeof(uint32_t) * scaled_width * scaled_height);
			// 10bpc if there's such an output?
			pixman_image_t *image = pixman_image_create_bits(g_slorp.options.jxl ? PIXMAN_x8r8g8b8 : PIXMAN_r8g8b8,
				scaled_width, scaled_height, pixbits, scaled_width * 4);
			struct nwl_surface *surf;
			wl_list_for_each(surf, &state.surfaces, link) {
				struct slorp_surface_state *slorp_surface = wl_container_of(surf, slorp_surface, nwl);
				struct slorp_box outputbox = {
					.x = slorp_surface->output->x,
					.y = slorp_surface->output->y,
					.width = slorp_surface->output->width,
					.height = slorp_surface->output->height
				};
				if (is_box_in_box(&outputbox, &selectionbox)) {
					pixman_image_t *tmp = pixman_image_create_bits_no_clear(
						wayland_pixformat_to_pixman(slorp_surface->buffer_format), slorp_surface->buffer_width,
						slorp_surface->buffer_height, (uint32_t*)slorp_surface->shm_data, slorp_surface->buffer_stride);
					int32_t local_x = selectionbox.x - outputbox.x;
					int32_t local_y = selectionbox.y - outputbox.y;
					int32_t offset_x = 0;
					int32_t offset_y = 0;
					if (local_x < 0) {
						offset_x = -local_x;
						local_x = 0;
					}
					if (local_y < 0) {
						offset_y = -local_y;
						local_y = 0;
					}
					pixman_image_composite32(PIXMAN_OP_SRC, tmp, NULL, image, local_x * slorp_surface->output->scale, local_y * slorp_surface->output->scale,
						0, 0, offset_x, offset_y, selectionbox.width * slorp_surface->output->scale, selectionbox.height * slorp_surface->output->scale);
					pixman_image_unref(tmp);
				}
			}
			// resolve name, Imma be a bit lazy here for now and add an implied _boxname.extension :)
			char filename[512];
			#if HAVE_JXL
			char *extension = g_slorp.options.jxl ? "jxl" : "png";
			#else
			char *extension = "png";
			#endif
			if (g_slorp.selected_box && g_slorp.selected_box->name) {
				char *boxsafename = strdupfilename(g_slorp.selected_box->name);
				snprintf(filename, 511, "%s_%s.%s", g_slorp.options.output_file_name, boxsafename, extension);
				free(boxsafename);
			} else {
				snprintf(filename, 511, "%s.%s", g_slorp.options.output_file_name, extension);
			}
			bool success = false;
			#if HAVE_JXL
			if (g_slorp.options.jxl) {
				success = save_image_jxl(filename, pixbits, scaled_width, scaled_height);
			} else 
			#endif
			{
				success = save_image_png(filename, pixbits, scaled_width, scaled_height);
			}
			pixman_image_unref(image);
			free(pixbits);
			if (success) {
				puts(filename);
			} else {
				fprintf(stderr, "Failed to save image..\n");
				retval = EXIT_FAILURE;
			}
		} else {
			printf("%d,%d %dx%d", selectionbox.x, selectionbox.y, selectionbox.width, selectionbox.height);
			if (g_slorp.options.output_box_name && g_slorp.selected_box && g_slorp.selected_box->name) {
				printf(" %s\n", g_slorp.selected_box->name);
			} else {
				putchar('\n');
			}
		}
	}
	nwl_wayland_uninit(&state);
	if (g_slorp.boxes_amount) {
		// Also free names!
		for (size_t i = 0; i < g_slorp.boxes_amount; i++) {
			free(g_slorp.boxes[i].name);
		}
		free(g_slorp.boxes);
	}
	return retval;
}
