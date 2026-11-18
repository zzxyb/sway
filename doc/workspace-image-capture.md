# Workspace image capture

Sway advertises `ext_workspace_image_capture_source_v1` version 1. Clients pass
an `ext_workspace_handle_v1` to `create_source`, then use the returned
`ext_image_capture_source_v1` with `ext_image_copy_capture_v1`.
The workspace protocol XML is vendored from the local wayland-protocols
`staging/ext-workspace-image-capture-source` implementation. Its interface name
is `ext_workspace_image_capture_source_v1` (there is no `manager` suffix).

## wlroots prerequisite

The existing `wlr_scene_tree_capture_*` API is used without changes. One extra
public API is required: `wlr_ext_workspace_handle_v1_from_resource()`. Apply
`doc/wlroots-workspace-handle-from-resource.patch` to wlroots and rebuild/reinstall
wlroots before configuring Sway. For example, from the wlroots checkout:

```sh
git apply /home/ut000903@uos/Documents/Code/Github/sway/doc/wlroots-workspace-handle-from-resource.patch
meson compile -C build
sudo meson install -C build
```

This accessor validates the resource implementation and returns NULL for an
inert handle. `wl_resource_get_user_data()` returns a private wrapper, not a
`wlr_ext_workspace_handle_v1`. The accessor must be implemented in wlroots, where
that wrapper is defined. The existing `wlr_*` symbol export rule covers it.
Sway's Meson checks require both this accessor and `wlr_scene_tree_capture_render`.
The wlroots checkout itself has not been modified by this Sway change.

## Behavior and current policy

Each requested workspace has an independent scene and fixed canvas matching its
output's logical dimensions. Window surface/subsurface trees are composed using
committed container geometry and workspace-local coordinates. Tabbed/stacked
layouts include the selected child; floating windows retain their stacking order;
fullscreen includes transient floating windows. The capture does not activate the
workspace or attach another scene output to the display scene.

The current capture policy includes window surfaces/subsurfaces on opaque black.
It omits compositor decorations, shell layers, backgrounds, popups outside the
window's subsurface tree, and cursors. It does not duplicate the transaction's
saved-buffer rendering during a window resize. These are deliberate limitations
of this first implementation; window contents use their latest committed surface.
A locked session fails capture requests, including an abandoned session lock.

Rendering polls at approximately 30 fps while sessions exist and conservatively
reports full damage. This handles hidden workspace commits without depending on
physical output damage or frame scheduling. Successful frames send frame-done to
the captured surfaces so hidden applications can animate. Idle sources do not
render. GPU timeline completion is awaited asynchronously before copying to the
client, and the offscreen output state is never committed to a physical output.
Workspace destruction stops sessions and makes the source resources inert.
Unavailable outputs or initialization failures create inert sources; transient
render failures fail the pending frame and allow subsequent retries.

## Validation on a development machine

```sh
meson setup --reconfigure build
meson compile -C build
```

Using an ext-image-copy-capture client which accepts workspace sources, check:

- Capture an inactive workspace; verify that the displayed workspace never changes.
- Compare overlapping floating windows and selected tabs with the activated view.
- Capture a fullscreen window and its transient dialogs.
- Animate a hidden client; verify frame callbacks continue and images update.
- Resize/move windows and move the workspace between differently scaled outputs.
- Destroy a workspace during capture; verify the session stops without a crash.
- Lock the session during a pending GPU frame; verify no workspace image is delivered.
- Destroy/recreate capture sessions and disconnect the output while recording.
- Test both SHM and DMA-BUF client buffers, including a timeline-capable renderer.

The execution environment used for this change does not expose `cc`, `meson`,
`ninja`, `git`, `wayland-scanner`, or the installed Wayland development headers.
Compilation and live compositor capture therefore still require verification on
the development machine; they have not been claimed as passing here.
