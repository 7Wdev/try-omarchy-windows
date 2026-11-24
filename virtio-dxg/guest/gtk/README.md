# GTK 4 for the virtio-dxg guest

The guest runtime loads a hash-verified `libgtk-4.so.1` cohort built from
pinned GTK `4.24.1` sources with Arch's compiler and Meson configuration and
verified ABI-identical (`nm -D` symbols and `NEEDED`/`SONAME`) against the
guest's `/usr/lib/libgtk-4.so.1.2400.1`.

## Included patches (`versions/4.24.1/gtk.patch`)

- `gtk/gtkcssnode.c` & `gtk/gtkcssnodeprivate.h`:
  - skip valid children (`!child->visible || !child->invalid`) before adding ancestor bloom hashes during validation and timestamp invalidation;
  - track per-subtree dependency bitmasks (`subtree_change` / `subtree_valid`) to prune unaffected valid subtrees without descending on invalidation;
  - track cached-style dependency bitmasks (`cache_change`) so `GtkCssNodeStyleCache` trees (`cssnode->cache`) are preserved when an invalidation does not affect cacheable properties;
  - strip parent/sibling-irrelevant and animation/timestamp flags from sibling propagation masks and scope `gtk_css_node_invalidate_style` / `gtk_css_node_reposition` to the affected subtree.
- `gtk/gtkcsswidgetnode.c`: update `last_updated_style` when the style pointer changes with no visual delta so repeated validations do not re-diff against a stale reference.
- `gtk/gtkgridview.c`: cache the border spacing computed during `size_allocate` and reuse it when splitting grid tiles, avoiding repeated CSS style lookups while scrolling large grids such as GNOME Files.
- `gtk/gtklistbase.c`: record the exact visible range `[vis_first, vis_last]` across the viewport corners in `gtk_list_base_adjustment_value_changed_cb` and slide the fixed-budget live widget window `[win_start, win_end)` only when `[vis_first - above_below, vis_last + above_below + 1)` leaves it, eliminating upward-scroll `GTK_PACK_END` `align_along` sawtooth widget rebinds.
- `gtk/gtklistitemmanager.c`: fast path in `gtk_list_item_tracker_set_position` using `gtk_list_item_tracker_query_range` so unchanged tracked item ranges update the tracker in place without calling `gtk_list_item_manager_ensure_items` or `gtk_widget_queue_resize`.

## Building

```sh
bash virtio-dxg/guest/gtk/build-in-guest.sh gtk-4.24.1.tar.xz gtk4-virtio-dxg.tar
```

Boot-time override is active whenever `gtk4-virtio-dxg.tar` is staged in
`/run/virtio-dxg/payload` or `/mnt/vdxgshare/out` unless disabled with
`virtio_dxg.gtk=0` on the kernel command line.
