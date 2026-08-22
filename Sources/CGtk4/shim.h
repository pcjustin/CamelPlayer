#include <gtk/gtk.h>

/* GTK cast macros are unavailable from Swift; these wrappers keep the Swift
   side on plain GtkWidget pointer / gpointer handles. */

static inline gpointer cp_application_new(const char *id) {
    return gtk_application_new(id, G_APPLICATION_DEFAULT_FLAGS);
}

static inline int cp_application_run(gpointer app) {
    return g_application_run(G_APPLICATION(app), 0, NULL);
}

static inline GtkWidget *cp_app_window_new(gpointer app) {
    return gtk_application_window_new(GTK_APPLICATION(app));
}

static inline unsigned long cp_signal_connect(gpointer instance, const char *signal,
                                              GCallback handler, gpointer data,
                                              GClosureNotify destroy) {
    return g_signal_connect_data(instance, signal, handler, data, destroy, 0);
}

static inline void cp_window_set_title(GtkWidget *w, const char *title) {
    gtk_window_set_title(GTK_WINDOW(w), title);
}

static inline void cp_window_set_default_size(GtkWidget *w, int width, int height) {
    gtk_window_set_default_size(GTK_WINDOW(w), width, height);
}

static inline void cp_window_set_child(GtkWidget *w, GtkWidget *child) {
    gtk_window_set_child(GTK_WINDOW(w), child);
}

static inline void cp_window_present(GtkWidget *w) {
    gtk_window_present(GTK_WINDOW(w));
}

static inline void cp_box_append(GtkWidget *box, GtkWidget *child) {
    gtk_box_append(GTK_BOX(box), child);
}

static inline void cp_label_set_text(GtkWidget *label, const char *text) {
    gtk_label_set_text(GTK_LABEL(label), text);
}

static inline void cp_button_set_label(GtkWidget *button, const char *text) {
    gtk_button_set_label(GTK_BUTTON(button), text);
}

static inline gpointer cp_string_list_new(void) {
    return gtk_string_list_new(NULL);
}

static inline void cp_string_list_append(gpointer list, const char *s) {
    gtk_string_list_append(GTK_STRING_LIST(list), s);
}

static inline GtkWidget *cp_drop_down_new(void) {
    return gtk_drop_down_new(NULL, NULL);
}

static inline void cp_drop_down_set_model(GtkWidget *dd, gpointer model) {
    gtk_drop_down_set_model(GTK_DROP_DOWN(dd), G_LIST_MODEL(model));
}

static inline void cp_drop_down_set_selected(GtkWidget *dd, unsigned int i) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), i);
}

static inline unsigned int cp_drop_down_get_selected(GtkWidget *dd) {
    return gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
}

static inline GtkWidget *cp_scale_new(double min, double max, double step) {
    return gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
}

static inline double cp_range_get_value(GtkWidget *w) {
    return gtk_range_get_value(GTK_RANGE(w));
}

static inline void cp_range_set_value(GtkWidget *w, double v) {
    gtk_range_set_value(GTK_RANGE(w), v);
}

static inline void cp_range_set_range(GtkWidget *w, double min, double max) {
    gtk_range_set_range(GTK_RANGE(w), min, max);
}

static inline GtkWidget *cp_scrolled_window(GtkWidget *child) {
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
    return sw;
}

static inline void cp_list_box_append_label(GtkWidget *list, const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_list_box_append(GTK_LIST_BOX(list), label);
}

static inline void cp_list_box_remove_all(GtkWidget *list) {
    gtk_list_box_remove_all(GTK_LIST_BOX(list));
}

static inline void cp_list_box_select_index(GtkWidget *list, int i) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i);
    if (row) gtk_list_box_select_row(GTK_LIST_BOX(list), row);
}

static inline int cp_list_box_row_index(gpointer row) {
    return gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row));
}

static inline GtkWidget *cp_stack_new(void) {
    return gtk_stack_new();
}

static inline void cp_stack_add(GtkWidget *stack, GtkWidget *child,
                                const char *name, const char *title) {
    if (title) {
        gtk_stack_add_titled(GTK_STACK(stack), child, name, title);
    } else {
        gtk_stack_add_named(GTK_STACK(stack), child, name);
    }
}

static inline GtkWidget *cp_stack_switcher_new(GtkWidget *stack) {
    GtkWidget *sw = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(sw), GTK_STACK(stack));
    return sw;
}

static inline const char *cp_stack_visible_name(GtkWidget *stack) {
    return gtk_stack_get_visible_child_name(GTK_STACK(stack));
}

static inline void cp_stack_set_visible(GtkWidget *stack, GtkWidget *child) {
    gtk_stack_set_visible_child(GTK_STACK(stack), child);
}

static inline GtkWidget *cp_flow_box_new(void) {
    GtkWidget *fb = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fb), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(fb), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fb), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fb), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fb), 12);
    gtk_widget_set_valign(fb, GTK_ALIGN_START);
    gtk_widget_set_halign(fb, GTK_ALIGN_START);
    return fb;
}

static inline void cp_flow_box_append(GtkWidget *fb, GtkWidget *child) {
    gtk_flow_box_append(GTK_FLOW_BOX(fb), child);
}

static inline void cp_flow_box_remove_all(GtkWidget *fb) {
    gtk_flow_box_remove_all(GTK_FLOW_BOX(fb));
}

static inline int cp_flow_box_child_index(gpointer child) {
    return gtk_flow_box_child_get_index(GTK_FLOW_BOX_CHILD(child));
}

static inline GtkWidget *cp_picture_new(void) {
    GtkWidget *p = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(p), GTK_CONTENT_FIT_COVER);
    return p;
}

/* Fixed-size square cover: an icon placeholder with a clipped picture overlay.
   The overlay ignores the picture's natural size, so large textures cannot
   stretch the cell. */
static inline GtkWidget *cp_cover_new(int size, const char *icon, GtkWidget **out_picture) {
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *base = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(base), size / 2);
    gtk_widget_set_size_request(base, size, size);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), base);
    GtkWidget *pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), pic);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay), pic, TRUE);
    gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(overlay, GTK_ALIGN_CENTER);
    *out_picture = pic;
    return overlay;
}

static inline GdkTexture *cp_texture_from_data(const unsigned char *data, unsigned long len) {
    GBytes *bytes = g_bytes_new(data, len);
    GdkTexture *t = gdk_texture_new_from_bytes(bytes, NULL);
    g_bytes_unref(bytes);
    return t;
}

static inline void cp_picture_set_texture(GtkWidget *p, GdkTexture *t) {
    gtk_picture_set_paintable(GTK_PICTURE(p), t ? GDK_PAINTABLE(t) : NULL);
}

/* Ties a Swift object's lifetime to the widget: destroy runs on finalize. */
static inline void cp_widget_bind_object(GtkWidget *w, gpointer data, GDestroyNotify destroy) {
    g_object_set_data_full(G_OBJECT(w), "cp-swift-object", data, destroy);
}

/* Registers a D-Bus object with a heap vtable (GDBus keeps a reference to the
   vtable for the lifetime of the registration). */
static inline guint cp_dbus_register_object(GDBusConnection *conn, const char *path,
                                            GDBusInterfaceInfo *info,
                                            GDBusInterfaceMethodCallFunc method_call,
                                            GDBusInterfaceGetPropertyFunc get_property,
                                            GDBusInterfaceSetPropertyFunc set_property,
                                            gpointer user_data) {
    GDBusInterfaceVTable *vtable = g_new0(GDBusInterfaceVTable, 1);
    vtable->method_call = method_call;
    vtable->get_property = get_property;
    vtable->set_property = set_property;
    return g_dbus_connection_register_object(conn, path, info, vtable, user_data, NULL, NULL);
}

static inline void cp_widget_add_file_drop(GtkWidget *w, GCallback cb, gpointer data) {
    GtkDropTarget *target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect_data(target, "drop", cb, data, NULL, 0);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(target));
}

/* Returns a g_strfreev-able NULL-terminated path array, or NULL. */
static inline char **cp_drop_value_paths(GValue *value) {
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return NULL;
    GdkFileList *file_list = g_value_get_boxed(value);
    GSList *files = gdk_file_list_get_files(file_list);
    guint n = g_slist_length(files);
    char **paths = g_new0(char *, n + 1);
    guint i = 0;
    for (GSList *l = files; l; l = l->next) {
        char *path = g_file_get_path(G_FILE(l->data));
        if (path) paths[i++] = path;
    }
    return paths;
}

static inline void cp_widget_add_key_handler(GtkWidget *w, GCallback cb, gpointer data) {
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect_data(key, "key-pressed", cb, data, NULL, 0);
    gtk_widget_add_controller(w, key);
}

static inline int cp_window_focus_is_text(GtkWidget *window) {
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));
    return focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT(focus));
}

static inline GtkWidget *cp_paned_new(GtkWidget *start, GtkWidget *end) {
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), start);
    gtk_paned_set_end_child(GTK_PANED(paned), end);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    return paned;
}

static inline int cp_paned_get_position(GtkWidget *paned) {
    return gtk_paned_get_position(GTK_PANED(paned));
}

static inline void cp_paned_set_position(GtkWidget *paned, int pos) {
    gtk_paned_set_position(GTK_PANED(paned), pos);
}

/* Centers the row at the given index in the list box's visible area. */
static inline void cp_list_box_scroll_to_index(GtkWidget *list, int i) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i);
    if (!row) return;
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(row), list, &bounds)) return;
    GtkAdjustment *adj = gtk_list_box_get_adjustment(GTK_LIST_BOX(list));
    if (!adj) return;
    double target = bounds.origin.y - (gtk_adjustment_get_page_size(adj) - bounds.size.height) / 2;
    gtk_adjustment_set_value(adj, target);
}

/* Debug helper: renders a realized widget into a PNG file. */
static inline int cp_snapshot_widget_to_png(GtkWidget *widget, const char *path) {
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    if (width <= 0 || height <= 0) return 0;
    GdkPaintable *paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_object_unref(paintable);
    if (!node) return 0;
    GskRenderer *renderer = gtk_native_get_renderer(gtk_widget_get_native(widget));
    graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height);
    GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &viewport);
    gsk_render_node_unref(node);
    if (!texture) return 0;
    int ok = gdk_texture_save_to_png(texture, path);
    g_object_unref(texture);
    return ok;
}

static inline void cp_label_set_markup(GtkWidget *label, const char *markup) {
    gtk_label_set_markup(GTK_LABEL(label), markup);
}

static inline void cp_label_set_xalign(GtkWidget *label, float xalign) {
    gtk_label_set_xalign(GTK_LABEL(label), xalign);
}

static inline void cp_label_set_max_width_chars(GtkWidget *label, int n) {
    gtk_label_set_max_width_chars(GTK_LABEL(label), n);
}

static inline void cp_label_justify_center(GtkWidget *label) {
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
}

static inline void cp_image_set_pixel_size(GtkWidget *image, int size) {
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
}

static inline void cp_image_set_icon(GtkWidget *image, const char *icon) {
    gtk_image_set_from_icon_name(GTK_IMAGE(image), icon);
}

static inline void cp_button_set_has_frame(GtkWidget *button, int has_frame) {
    gtk_button_set_has_frame(GTK_BUTTON(button), has_frame);
}

static inline void cp_alert(GtkWidget *parent, const char *message) {
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_show(d, GTK_WINDOW(parent));
    g_object_unref(d);
}

static inline void cp_list_box_single_click(GtkWidget *list, int single) {
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), single);
}

static inline void cp_list_box_append(GtkWidget *list, GtkWidget *child) {
    gtk_list_box_append(GTK_LIST_BOX(list), child);
}

static inline GtkWidget *cp_toggle_new_icon(const char *icon) {
    GtkWidget *b = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(b), icon);
    return b;
}

static inline int cp_toggle_get_active(GtkWidget *b) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b));
}

static inline void cp_toggle_set_active(GtkWidget *b, int active) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), active);
}

static inline void cp_button_set_icon_name(GtkWidget *b, const char *icon) {
    gtk_button_set_icon_name(GTK_BUTTON(b), icon);
}

static inline void cp_file_dialog_select_folder(GtkWidget *parent,
                                                GAsyncReadyCallback cb, gpointer data) {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_select_folder(d, GTK_WINDOW(parent), NULL, cb, data);
}

/* Consumes the dialog; returns a g_free-able path or NULL when cancelled. */
static inline char *cp_file_dialog_folder_finish(GObject *source, GAsyncResult *res) {
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, NULL);
    g_object_unref(source);
    if (!f) return NULL;
    char *path = g_file_get_path(f);
    g_object_unref(f);
    return path;
}

/* Save dialog for playlist export. */
static inline void cp_file_dialog_save(GtkWidget *parent, const char *name,
                                       GAsyncReadyCallback cb, gpointer data) {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_initial_name(d, name);
    gtk_file_dialog_save(d, GTK_WINDOW(parent), NULL, cb, data);
}

static inline char *cp_file_dialog_save_finish(GObject *source, GAsyncResult *res) {
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, NULL);
    g_object_unref(source);
    if (!f) return NULL;
    char *path = g_file_get_path(f);
    g_object_unref(f);
    return path;
}

/* Single-file open dialog for playlist import. */
static inline void cp_file_dialog_open(GtkWidget *parent,
                                       GAsyncReadyCallback cb, gpointer data) {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_open(d, GTK_WINDOW(parent), NULL, cb, data);
}

static inline char *cp_file_dialog_open_finish(GObject *source, GAsyncResult *res) {
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, NULL);
    g_object_unref(source);
    if (!f) return NULL;
    char *path = g_file_get_path(f);
    g_object_unref(f);
    return path;
}

static inline const char *cp_editable_get_text(GtkWidget *w) {
    return gtk_editable_get_text(GTK_EDITABLE(w));
}

static inline void cp_editable_set_text(GtkWidget *w, const char *t) {
    gtk_editable_set_text(GTK_EDITABLE(w), t);
}

static inline void cp_label_ellipsize_end(GtkWidget *label) {
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
}

static inline void cp_file_dialog_set_audio_filter(GtkFileDialog *d) {
    GtkFileFilter *audio = gtk_file_filter_new();
    gtk_file_filter_set_name(audio, "Audio Files");
    const char *suffixes[] = {"mp3", "wav", "m4a", "flac", "aac", "aiff", NULL};
    for (const char **s = suffixes; *s; s++) gtk_file_filter_add_suffix(audio, *s);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All Files");
    gtk_file_filter_add_pattern(all, "*");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, audio);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(d, audio);
    g_object_unref(filters);
    g_object_unref(audio);
    g_object_unref(all);
}

static inline void cp_file_dialog_open_multiple(GtkWidget *parent,
                                                GAsyncReadyCallback cb, gpointer data) {
    GtkFileDialog *d = gtk_file_dialog_new();
    cp_file_dialog_set_audio_filter(d);
    gtk_file_dialog_open_multiple(d, GTK_WINDOW(parent), NULL, cb, data);
}

/* Consumes the dialog; returns NULL when the user cancelled. */
static inline gpointer cp_file_dialog_finish(GObject *source, GAsyncResult *res) {
    gpointer files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), res, NULL);
    g_object_unref(source);
    return files;
}

static inline unsigned int cp_list_model_n_items(gpointer model) {
    return g_list_model_get_n_items(G_LIST_MODEL(model));
}

/* Caller frees the returned path with g_free. */
static inline char *cp_file_path_at(gpointer files, unsigned int i) {
    GFile *f = g_list_model_get_item(G_LIST_MODEL(files), i);
    char *path = f ? g_file_get_path(f) : NULL;
    g_clear_object(&f);
    return path;
}
