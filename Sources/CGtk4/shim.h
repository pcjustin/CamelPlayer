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

static inline GtkWidget *cp_notebook_new(void) {
    return gtk_notebook_new();
}

static inline void cp_notebook_append(GtkWidget *nb, GtkWidget *child, const char *label) {
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), child, gtk_label_new(label));
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

static inline void cp_file_dialog_open_multiple(GtkWidget *parent,
                                                GAsyncReadyCallback cb, gpointer data) {
    GtkFileDialog *d = gtk_file_dialog_new();
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
