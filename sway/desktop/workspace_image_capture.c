#include <math.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "ext-workspace-image-capture-source-v1-protocol.h"
#include "log.h"
#include "sway/server.h"
#include "sway/tree/container.h"
#include "sway/output.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"

// Workspace-local composition, independent of the display scene. Capture policy:
// window surfaces and subsurfaces on black, without shell layers or decorations.
struct sway_workspace_image_capture {
	struct wlr_ext_image_capture_source_v1 base;
	struct sway_workspace *workspace;
	struct wlr_scene *scene;
	struct wlr_scene_tree *content;
	struct wlr_scene_tree_capture *capture;
	struct wlr_swapchain *swapchain;
	struct wlr_renderer *renderer;
	struct wl_event_source *timer;
	size_t started;
	struct wl_listener workspace_destroy;
	struct wl_listener display_destroy;
	struct wlr_output_state state;
	struct wlr_drm_syncobj_timeline_waiter waiter;
	bool waiting;
	bool valid_frame;
	bool constraints_ready;
};

static struct wlr_output *source_output(struct sway_workspace_image_capture *source) {
	struct sway_output *output = source->workspace->current.output;
	return output ? output->wlr_output : NULL;
}

static bool source_configure(struct sway_workspace_image_capture *source) {
	struct wlr_output *output = source_output(source);
	if (!output || !output->renderer || !output->allocator) {
		return false;
	}
	int width, height;
	wlr_output_effective_resolution(output, &width, &height);
	int pixel_width = round(width * output->scale);
	int pixel_height = round(height * output->scale);
	if (width <= 0 || height <= 0 || pixel_width <= 0 || pixel_height <= 0) {
		return false;
	}
	if (!wlr_scene_tree_capture_configure(source->capture, width, height)) {
		return false;
	}
	if (source->constraints_ready && source->swapchain &&
			source->renderer == output->renderer &&
			source->swapchain->allocator == output->allocator &&
			source->swapchain->width == pixel_width &&
			source->swapchain->height == pixel_height &&
			source->swapchain->format.format == output->render_format) {
		return true;
	}
	// Let wlroots negotiate render-target formats and modifiers. Texture formats
	// describe sampling support and cannot be used as render-target formats.
	// Use a separate temporary swapchain: never replace the display swapchain.
	struct wlr_swapchain *primary = NULL;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	bool configured = wlr_output_configure_primary_swapchain(output, &state, &primary);
	wlr_output_state_finish(&state);
	if (!configured || !primary) {
		wlr_swapchain_destroy(primary);
		return false;
	}
	struct wlr_swapchain *swapchain = wlr_swapchain_create(output->allocator,
		pixel_width, pixel_height, &primary->format);
	wlr_swapchain_destroy(primary);
	if (!swapchain) {
		return false;
	}
	wlr_swapchain_destroy(source->swapchain);
	source->swapchain = swapchain;
	source->renderer = output->renderer;
	source->constraints_ready = wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(
		&source->base, swapchain, output->renderer);
	return source->constraints_ready;
}

static bool add_container(struct sway_workspace_image_capture *source,
		struct sway_container *con) {
	if (con->view) {
		struct sway_view *view = con->view;
		if (!view->surface) {
			return true;
		}
		struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(
			source->content, view->surface);
		if (!tree) {
			return false;
		}
		struct sway_container_state *state = &con->current;
		int x = state->content_x - source->workspace->current.output->lx;
		int y = state->content_y - source->workspace->current.output->ly;
		if (container_is_floating(con) || state->fullscreen_mode != FULLSCREEN_NONE) {
			x += fmax(0, (state->content_width - view->geometry.width) / 2);
			y += fmax(0, (state->content_height - view->geometry.height) / 2);
		}
		wlr_scene_node_set_position(&tree->node,
			x - view->geometry.x, y - view->geometry.y);
		if (!view->using_csd || (!container_is_floating(con) &&
				state->fullscreen_mode == FULLSCREEN_NONE)) {
			struct wlr_box clip = {
				.x = view->geometry.x, .y = view->geometry.y,
				.width = state->content_width, .height = state->content_height,
			};
			wlr_scene_subsurface_tree_set_clip(&tree->node, &clip);
		}
		return true;
	}
	list_t *children = con->current.children;
	for (int i = 0; i < children->length; i++) {
		struct sway_container *child = children->items[i];
		if ((con->current.layout == L_TABBED || con->current.layout == L_STACKED) &&
				child != con->current.focused_inactive_child) {
			continue;
		}
		if (!add_container(source, child)) {
			return false;
		}
	}
	return true;
}

static bool source_compose(struct sway_workspace_image_capture *source) {
	if (source->content) {
		wlr_scene_node_destroy(&source->content->node);
	}
	source->content = wlr_scene_tree_create(&source->scene->tree);
	if (!source->content) {
		return false;
	}
	// A locked or abandoned lock never exposes workspace contents.
	if (server.session_lock.lock) {
		return true;
	}
	struct sway_workspace *ws = source->workspace;
	struct sway_container *fs = ws->current.fullscreen;
	if (root->fullscreen_global && root->fullscreen_global->current.workspace == ws) {
		fs = root->fullscreen_global;
	}
	if (fs) {
		if (!add_container(source, fs)) {
			return false;
		}
	} else {
		for (int i = 0; i < ws->current.tiling->length; i++) {
			struct sway_container *con = ws->current.tiling->items[i];
			if ((ws->current.layout == L_TABBED || ws->current.layout == L_STACKED) &&
					con != ws->current.focused_inactive_child) {
				continue;
			}
			if (!add_container(source, con)) {
				return false;
			}
		}
	}
	for (int i = 0; i < ws->current.floating->length; i++) {
		struct sway_container *con = ws->current.floating->items[i];
		if (con == fs || (fs && !container_is_transient_for(con, fs))) {
			continue;
		}
		if (!add_container(source, con)) {
			return false;
		}
	}
	return true;
}

static void send_frame_done(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(buffer);
	if (surface) {
		wlr_surface_send_frame_done(surface->surface, data);
	}
}

static void source_deliver(struct sway_workspace_image_capture *source) {
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, source->base.width, source->base.height);
	struct wlr_ext_image_capture_source_v1_frame_event event = { .damage = &damage };
	wl_signal_emit_mutable(&source->base.events.frame, &event);
	if (source->valid_frame && source->content && !server.session_lock.lock) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_scene_node_for_each_buffer(&source->content->node, send_frame_done, &now);
	}
	pixman_region32_fini(&damage);
	wlr_output_state_finish(&source->state);
	wlr_output_state_init(&source->state);
	source->valid_frame = false;
}

static void source_ready(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct sway_workspace_image_capture *source = wl_container_of(waiter, source, waiter);
	wlr_drm_syncobj_timeline_waiter_finish(waiter);
	source->waiting = false;
	source_deliver(source);
}

static int source_tick(void *data) {
	struct sway_workspace_image_capture *source = data;
	if (!source->started) {
		return 0;
	}
	// Bounded polling also picks up commits and geometry changes on hidden trees.
	wl_event_source_timer_update(source->timer, 33);
	if (source->waiting) {
		return 0;
	}
	bool ok = source_configure(source) && source_compose(source);
	if (ok) {
		wlr_scene_tree_capture_add_damage(source->capture, NULL);
		struct wlr_scene_output_state_options options = { .swapchain = source->swapchain };
		ok = wlr_scene_tree_capture_render(source->capture, source_output(source),
			&source->state, &options);
	}
	source->valid_frame = ok;
	if (ok && source->state.wait_timeline) {
		source->waiting = wlr_drm_syncobj_timeline_waiter_init(&source->waiter,
			source->state.wait_timeline, source->state.wait_point, 0,
			server.wl_event_loop, source_ready);
		if (source->waiting) {
			return 0;
		}
		source->valid_frame = false;
	}
	source_deliver(source);
	return 0;
}

static void source_start(struct wlr_ext_image_capture_source_v1 *base, bool with_cursors) {
	struct sway_workspace_image_capture *source = wl_container_of(base, source, base);
	if (++source->started == 1) {
		wl_event_source_timer_update(source->timer, 1);
	}
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	struct sway_workspace_image_capture *source = wl_container_of(base, source, base);
	if (--source->started == 0) {
		wl_event_source_timer_update(source->timer, 0);
	}
}

static void source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct sway_workspace_image_capture *source = wl_container_of(base, source, base);
	if (source->started && schedule_frame && !source->waiting) {
		wl_event_source_timer_update(source->timer, 1);
	}
}

static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *event) {
	struct sway_workspace_image_capture *source = wl_container_of(base, source, base);
	struct wlr_output *output = source_output(source);
	if (!source->valid_frame || server.session_lock.lock || !output ||
			output->renderer != source->renderer) {
		wlr_ext_image_copy_capture_frame_v1_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return;
	}
	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame,
			source->state.buffer, output->renderer)) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &now);
	}
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
	.start = source_start,
	.stop = source_stop,
	.request_frame = source_request_frame,
	.copy_frame = source_copy_frame,
};

static void source_destroy(struct sway_workspace_image_capture *source) {
	wlr_ext_image_capture_source_v1_finish(&source->base);
	if (source->waiting) {
		wlr_drm_syncobj_timeline_waiter_finish(&source->waiter);
	}
	if (source->timer) {
		wl_event_source_remove(source->timer);
	}
	wlr_output_state_finish(&source->state);
	wlr_swapchain_destroy(source->swapchain);
	if (source->scene) {
		wlr_scene_node_destroy(&source->scene->tree.node);
	}
	wl_list_remove(&source->workspace_destroy.link);
	wl_list_remove(&source->display_destroy.link);
	source->workspace->image_capture = NULL;
	free(source);
}

static void handle_workspace_destroy(struct wl_listener *listener, void *data) {
	struct sway_workspace_image_capture *source = wl_container_of(listener, source, workspace_destroy);
	source_destroy(source);
}

static void handle_source_display_destroy(struct wl_listener *listener, void *data) {
	struct sway_workspace_image_capture *source = wl_container_of(listener, source, display_destroy);
	source_destroy(source);
}

static struct sway_workspace_image_capture *source_create(struct sway_workspace *ws) {
	struct sway_workspace_image_capture *source = calloc(1, sizeof(*source));
	if (!source) {
		return NULL;
	}
	source->workspace = ws;
	ws->image_capture = source;
	wlr_ext_image_capture_source_v1_init(&source->base, &source_impl);
	wlr_output_state_init(&source->state);
	wl_list_init(&source->workspace_destroy.link);
	wl_list_init(&source->display_destroy.link);
	source->scene = wlr_scene_create();
	if (!source->scene) {
		goto error;
	}
	source->scene->restack_xwayland_surfaces = false;
	source->content = wlr_scene_tree_create(&source->scene->tree);
	source->capture = wlr_scene_tree_capture_create(&source->scene->tree, 1, 1);
	source->timer = wl_event_loop_add_timer(server.wl_event_loop, source_tick, source);
	if (!source->content || !source->capture || !source->timer || !source_configure(source)) {
		goto error;
	}
	source->workspace_destroy.notify = handle_workspace_destroy;
	wl_signal_add(&ws->node.events.destroy, &source->workspace_destroy);
	source->display_destroy.notify = handle_source_display_destroy;
	wl_display_add_destroy_listener(server.wl_display, &source->display_destroy);
	return source;
error:
	source_destroy(source);
	return NULL;
}

static void manager_create_source(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *workspace_resource) {
	struct wlr_ext_workspace_handle_v1 *handle =
		wlr_ext_workspace_handle_v1_from_resource(workspace_resource);
	struct sway_workspace *ws = handle ? handle->data : NULL;
	struct sway_workspace_image_capture *source = ws ? ws->image_capture : NULL;
	if (ws && !source) {
		source = source_create(ws);
		if (!source) {
			sway_log(SWAY_ERROR, "Failed to initialize workspace capture source for '%s'", ws->name);
		}
	}
	wlr_ext_image_capture_source_v1_create_resource(source ? &source->base : NULL, client, id);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct ext_workspace_image_capture_source_v1_interface manager_impl = {
	.create_source = manager_create_source,
	.destroy = manager_destroy,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&ext_workspace_image_capture_source_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void handle_manager_display_destroy(struct wl_listener *listener, void *data) {
	wl_list_remove(&server.workspace_image_capture_display_destroy.link);
	wl_global_destroy(server.workspace_image_capture_global);
	server.workspace_image_capture_global = NULL;
}

bool sway_workspace_image_capture_init(void) {
	server.workspace_image_capture_global = wl_global_create(server.wl_display,
		&ext_workspace_image_capture_source_v1_interface, 1, NULL, manager_bind);
	if (!server.workspace_image_capture_global) {
		return false;
	}
	server.workspace_image_capture_display_destroy.notify = handle_manager_display_destroy;
	wl_display_add_destroy_listener(server.wl_display,
		&server.workspace_image_capture_display_destroy);
	return true;
}
