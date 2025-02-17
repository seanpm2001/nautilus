/* Nautilus
 *
 *  Copyright (C) 2008 Red Hat, Inc.
 *
 *  The Gnome Library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  The Gnome Library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with the Gnome Library; see the file COPYING.LIB.  If not,
 *  see <http://www.gnu.org/licenses/>.
 *
 *  Author: David Zeuthen <davidz@redhat.com>
 */


#include <config.h>

#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <gio/gio.h>

#include <glib/gi18n.h>

typedef struct
{
    GtkWidget *dialog;
    GMount *mount;
} AutorunSoftwareDialogData;

static void autorun_software_dialog_mount_unmounted (GMount                    *mount,
                                                     AutorunSoftwareDialogData *data);

static void
autorun_software_dialog_destroy (AutorunSoftwareDialogData *data)
{
    g_signal_handlers_disconnect_by_func (G_OBJECT (data->mount),
                                          G_CALLBACK (autorun_software_dialog_mount_unmounted),
                                          data);

    gtk_widget_destroy (GTK_WIDGET (data->dialog));
    g_object_unref (data->mount);
    g_free (data);
}

static void
autorun_software_dialog_mount_unmounted (GMount                    *mount,
                                         AutorunSoftwareDialogData *data)
{
    autorun_software_dialog_destroy (data);
}

static gboolean
_check_file (GFile      *mount_root,
             const char *file_path,
             gboolean    must_be_executable)
{
    g_autoptr (GFile) file = NULL;
    g_autoptr (GFileInfo) file_info = NULL;

    file = g_file_get_child (mount_root, file_path);
    file_info = g_file_query_info (file,
                                   G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
                                   G_FILE_QUERY_INFO_NONE,
                                   NULL,
                                   NULL);

    if (file_info == NULL)
    {
        return FALSE;
    }

    if (must_be_executable &&
        !g_file_info_get_attribute_boolean (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
    {
        return FALSE;
    }

    return TRUE;
}

static void
autorun (GMount *mount)
{
    g_autoptr (GFile) root = NULL;
    g_autoptr (GFile) program_to_spawn = NULL;
    g_autoptr (GFile) program_parameter_file = NULL;
    g_autofree char *error_string = NULL;
    g_autofree char *path_to_spawn = NULL;
    g_autofree char *cwd_for_program = NULL;
    g_autofree char *program_parameter = NULL;

    root = g_mount_get_root (mount);

    /* Careful here, according to
     *
     *  http://standards.freedesktop.org/autostart-spec/autostart-spec-latest.html
     *
     * the ordering does matter.
     */

    if (_check_file (root, ".autorun", TRUE))
    {
        program_to_spawn = g_file_get_child (root, ".autorun");
    }
    else if (_check_file (root, "autorun", TRUE))
    {
        program_to_spawn = g_file_get_child (root, "autorun");
    }
    else if (_check_file (root, "autorun.sh", TRUE))
    {
        program_to_spawn = g_file_new_for_path ("/bin/sh");
        program_parameter_file = g_file_get_child (root, "autorun.sh");
    }

    if (program_to_spawn != NULL)
    {
        path_to_spawn = g_file_get_path (program_to_spawn);
    }
    if (program_parameter_file != NULL)
    {
        program_parameter = g_file_get_path (program_parameter_file);
    }

    cwd_for_program = g_file_get_path (root);

    if (path_to_spawn != NULL && cwd_for_program != NULL)
    {
        if (chdir (cwd_for_program) == 0)
        {
            execl (path_to_spawn, path_to_spawn, program_parameter, NULL);
            error_string = g_strdup_printf (_("Unable to start the program:\n%s"), strerror (errno));
            goto out;
        }
        error_string = g_strdup_printf (_("Unable to start the program:\n%s"), strerror (errno));
        goto out;
    }
    error_string = g_strdup_printf (_("Unable to locate the program"));

out:
    if (error_string != NULL)
    {
        GtkWidget *dialog;
        dialog = gtk_message_dialog_new_with_markup (NULL,         /* TODO: parent window? */
                                                     0,
                                                     GTK_MESSAGE_ERROR,
                                                     GTK_BUTTONS_OK,
                                                     _("Oops! There was a problem running this software."));
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", error_string);
        /* This is required because we don't show dialogs in the
         *  window picker and if the window pops under another window
         *  there is no way to get it back. */
        gtk_window_set_keep_above (GTK_WINDOW (dialog), TRUE);

        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
    }
}

static void
present_autorun_for_software_dialog (GMount *mount)
{
    GIcon *icon;
    g_autofree char *mount_name = NULL;
    GtkWidget *dialog;
    AutorunSoftwareDialogData *data;

    mount_name = g_mount_get_name (mount);

    dialog = gtk_message_dialog_new (NULL,     /* TODO: parent window? */
                                     0,
                                     GTK_MESSAGE_OTHER,
                                     GTK_BUTTONS_CANCEL,
                                     _("“%s” contains software intended to be automatically started. Would you like to run it?"),
                                     mount_name);
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                              "%s",
                                              _("If you don’t trust this location or aren’t sure, press Cancel."));

    /* This is required because we don't show dialogs in the
     *  window picker and if the window pops under another window
     *  there is no way to get it back. */
    gtk_window_set_keep_above (GTK_WINDOW (dialog), TRUE);

    /* TODO: in a star trek future add support for verifying
     * software on media (e.g. if it has a certificate, check it
     * etc.)
     */


    icon = g_mount_get_icon (mount);
    if (G_IS_THEMED_ICON (icon))
    {
        const gchar * const *names;

        names = g_themed_icon_get_names (G_THEMED_ICON (icon));

        if (names != NULL)
        {
            gtk_window_set_icon_name (GTK_WINDOW (dialog), names[0]);
        }
    }

    data = g_new0 (AutorunSoftwareDialogData, 1);
    data->dialog = dialog;
    data->mount = g_object_ref (mount);

    g_signal_connect (G_OBJECT (mount),
                      "unmounted",
                      G_CALLBACK (autorun_software_dialog_mount_unmounted),
                      data);

    gtk_dialog_add_button (GTK_DIALOG (dialog),
                           _("_Run"),
                           GTK_RESPONSE_OK);

    gtk_widget_show_all (dialog);

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK)
    {
        gtk_widget_destroy (dialog);
        autorun (mount);
    }
}

int
main (int   argc,
      char *argv[])
{
    g_autoptr (GVolumeMonitor) monitor = NULL;
    g_autoptr (GFile) file = NULL;
    g_autoptr (GMount) mount = NULL;
    g_autoptr (GError) error = NULL;

    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    gtk_init (&argc, &argv);

    if (argc != 2)
    {
        g_print ("Usage: %s mount-uri\n", argv[0]);
        goto out;
    }

    /* instantiate monitor so we get the "unmounted" signal properly */
    monitor = g_volume_monitor_get ();
    if (monitor == NULL)
    {
        g_warning ("Unable to connect to the volume monitor");
        goto out;
    }

    file = g_file_new_for_commandline_arg (argv[1]);
    if (file == NULL)
    {
        g_warning ("Unable to parse mount URI");
        goto out;
    }

    mount = g_file_find_enclosing_mount (file, NULL, &error);
    if (mount == NULL)
    {
        g_warning ("Unable to find device for URI: %s", error->message);
        goto out;
    }

    present_autorun_for_software_dialog (mount);

out:
    return 0;
}
