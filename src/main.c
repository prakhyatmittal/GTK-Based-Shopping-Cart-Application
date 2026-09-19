/*
 * main.c — entry point. Wires together db_init (with a resolved, portable
 * path instead of a bare "shopping_cart.db" relative to whatever directory
 * the app happened to be launched from) and the GTK UI.
 */
#include <gtk/gtk.h>
#include "ui.h"
#include "db.h"
#include "respath.h"
#include <stdio.h>

static void on_shutdown(GApplication *app, gpointer user_data)
{
    (void)app; (void)user_data;
    db_close();
}

int main(int argc, char **argv)
{
    char db_path[4096];
    respath_resolve("shopping_cart.db", db_path, sizeof(db_path));

    if (!db_init(db_path)) {
        fprintf(stderr, "Fatal: could not initialize database at %s\n", db_path);
        return 1;
    }

    GtkApplication *app = gtk_application_new("com.quickcart.app", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(ui_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}
