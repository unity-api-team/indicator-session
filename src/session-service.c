/*
A small wrapper utility to load indicators and put them as menu items
into the gnome-panel using it's applet interface.

Copyright 2009 Canonical Ltd.

Authors:
    Ted Gould <ted@canonical.com>
    Christoph Korn <c_korn@gmx.de>
    Cody Russell <crussell@canonical.com>
    Conor Curran <conor.curran@canonical.com>

This program is free software: you can redistribute it and/or modify it 
under the terms of the GNU General Public License version 3, as published 
by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but 
WITHOUT ANY WARRANTY; without even the implied warranties of 
MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR 
PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along 
with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>

#include <unistd.h>

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>

#include <libdbusmenu-glib/server.h>
#include <libdbusmenu-glib/menuitem.h>
#include <libdbusmenu-glib/client.h>
#include <libdbusmenu-gtk3/menuitem.h>

#include <libindicator/indicator-service.h>

#include "dbus-shared-names.h"
#include "dbusmenu-shared.h"
#include "users-service-dbus.h"
#include "user-menu-mgr.h"

#include "gconf-helper.h"

#include "session-dbus.h"
#include "lock-helper.h"
#include "upower-client.h"

#define UP_ADDRESS    "org.freedesktop.UPower"
#define UP_OBJECT     "/org/freedesktop/UPower"
#define UP_INTERFACE  "org.freedesktop.UPower"

#define EXTRA_LAUNCHER_DIR "/usr/share/indicators/session/applications"


typedef struct _ActivateData ActivateData;
struct _ActivateData
{
  UsersServiceDbus *service;
  UserData *user;
};

//static UsersServiceDbus  *dbus_interface = NULL;
static SessionDbus       *session_dbus = NULL;
static DbusmenuMenuitem  *lock_menuitem = NULL;
static DbusmenuMenuitem * session_root_menuitem = NULL;

static GMainLoop * mainloop = NULL;
static DBusGProxy * up_main_proxy = NULL;
static DBusGProxy * up_prop_proxy = NULL;

static DBusGProxyCall * suspend_call = NULL;
static DBusGProxyCall * hibernate_call = NULL;

static DbusmenuMenuitem * hibernate_mi = NULL;
static DbusmenuMenuitem * suspend_mi = NULL;
static DbusmenuMenuitem * logout_mi = NULL;
static DbusmenuMenuitem * restart_mi = NULL;
static DbusmenuMenuitem * shutdown_mi = NULL;

static gboolean can_hibernate = TRUE;
static gboolean can_suspend = TRUE;
static gboolean allow_hibernate = TRUE;
static gboolean allow_suspend = TRUE;

static GConfClient * gconf_client = NULL;

static void rebuild_session_items (DbusmenuMenuitem *root);

static void
lockdown_changed (GConfClient *client,
                  guint        cnxd_id,
                  GConfEntry  *entry,
                  gpointer     user_data)
{
	GConfValue  *value = gconf_entry_get_value (entry);
	const gchar *key   = gconf_entry_get_key (entry);

	if (value == NULL || key == NULL) {
		return;
	}

	if (g_strcmp0 (key, LOCKDOWN_KEY_USER) == 0 || g_strcmp0 (key, LOCKDOWN_KEY_SCREENSAVER) == 0) {
		rebuild_session_items(session_root_menuitem);
	}

	return;
}

static void
keybinding_changed (GConfClient *client,
                    guint        cnxd_id,
                    GConfEntry  *entry,
                    gpointer     user_data)
{
	GConfValue  *value = gconf_entry_get_value (entry);
	const gchar *key   = gconf_entry_get_key (entry);

	if (value == NULL || key == NULL) {
		return;
	}

	if (g_strcmp0 (key, KEY_LOCK_SCREEN) == 0) {
		g_debug("Keybinding changed to: %s", gconf_value_get_string(value));
		if (lock_menuitem != NULL) {
			dbusmenu_menuitem_property_set_shortcut_string(lock_menuitem, gconf_value_get_string(value));
		}
	}
	return;
}

/* Ensures that we have a GConf client and if we build one
   set up the signal handler. */
static void
ensure_gconf_client (void)
{
	if (!gconf_client) {
		gconf_client = gconf_client_get_default ();

		gconf_client_add_dir(gconf_client, LOCKDOWN_DIR, GCONF_CLIENT_PRELOAD_ONELEVEL, NULL);
		gconf_client_notify_add(gconf_client, LOCKDOWN_DIR, lockdown_changed, NULL, NULL, NULL);

		gconf_client_add_dir(gconf_client, KEYBINDING_DIR, GCONF_CLIENT_PRELOAD_ONELEVEL, NULL);
		gconf_client_notify_add(gconf_client, KEYBINDING_DIR, keybinding_changed, NULL, NULL, NULL);
	}
 	return;
}

/* Check to see if the lockdown key is protecting from
   locking the screen.  If not, lock it. */
static void
lock_if_possible (void) {
	ensure_gconf_client ();

	if (!gconf_client_get_bool (gconf_client, LOCKDOWN_KEY_SCREENSAVER, NULL)) {
		lock_screen (NULL, 0, NULL);
	}
	return;
}

/* A return from the command to sleep the system.  Make sure
   that we unthrottle the screensaver. */
static void
sleep_response (DBusGProxy * proxy, DBusGProxyCall * call, gpointer data)
{
	screensaver_unthrottle();
	return;
}

/* Let's put this machine to sleep, with some info on how
   it should sleep.  */
static void
machine_sleep (DbusmenuMenuitem * mi, guint timestamp, gpointer userdata)
{
	gchar * type = (gchar *)userdata;

	if (up_main_proxy == NULL) {
		g_warning("Can not %s as no upower proxy", type);
	}

	screensaver_throttle(type);
	lock_if_possible();

	dbus_g_proxy_begin_call(up_main_proxy,
	                        type,
	                        sleep_response,
	                        NULL,
	                        NULL,
	                        G_TYPE_INVALID);

	return;
}

/* A response to getting the suspend property */
static void
suspend_prop_cb (DBusGProxy * proxy, DBusGProxyCall * call, gpointer userdata)
{
	suspend_call = NULL;

	GValue candoit = {0};
	GError * error = NULL;
	dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_VALUE, &candoit, G_TYPE_INVALID);
	if (error != NULL) {
		g_warning("Unable to check suspend: %s", error->message);
		g_error_free(error);
		return;
	}
	g_debug("Got Suspend: %s", g_value_get_boolean(&candoit) ? "true" : "false");

	gboolean local_can_suspend = g_value_get_boolean(&candoit);
	if (local_can_suspend != can_suspend) {
		can_suspend = local_can_suspend;
		rebuild_session_items(session_root_menuitem);
	}

	return;
}

/* Response to getting the hibernate property */
static void
hibernate_prop_cb (DBusGProxy * proxy, DBusGProxyCall * call, gpointer userdata)
{
	hibernate_call = NULL;

	GValue candoit = {0};
	GError * error = NULL;
	dbus_g_proxy_end_call(proxy, call, &error, G_TYPE_VALUE, &candoit, G_TYPE_INVALID);
	if (error != NULL) {
		g_warning("Unable to check hibernate: %s", error->message);
		g_error_free(error);
		return;
	}
	g_debug("Got Hibernate: %s", g_value_get_boolean(&candoit) ? "true" : "false");

	gboolean local_can_hibernate = g_value_get_boolean(&candoit);
	if (local_can_hibernate != can_hibernate) {
		can_hibernate = local_can_hibernate;
		rebuild_session_items(session_root_menuitem);
	}

	return;
}

/* A signal that we need to recheck to ensure we can still
   hibernate and/or suspend */
static void
up_changed_cb (DBusGProxy * proxy, gpointer user_data)
{
	/* Start Async call to see if we can hibernate */
	if (suspend_call == NULL) {
		suspend_call = dbus_g_proxy_begin_call(up_prop_proxy,
		                                       "Get",
		                                       suspend_prop_cb,
		                                       NULL,
		                                       NULL,
		                                       G_TYPE_STRING,
		                                       UP_INTERFACE,
		                                       G_TYPE_STRING,
		                                       "CanSuspend",
		                                       G_TYPE_INVALID,
		                                       G_TYPE_VALUE,
		                                       G_TYPE_INVALID);
	}

	/* Start Async call to see if we can suspend */
	if (hibernate_call == NULL) {
		hibernate_call = dbus_g_proxy_begin_call(up_prop_proxy,
		                                         "Get",
		                                         hibernate_prop_cb,
		                                         NULL,
		                                         NULL,
		                                         G_TYPE_STRING,
		                                         UP_INTERFACE,
		                                         G_TYPE_STRING,
		                                         "CanHibernate",
		                                         G_TYPE_INVALID,
		                                         G_TYPE_VALUE,
		                                         G_TYPE_INVALID);
	}

	return;
}

/* Handle the callback from the allow functions to check and
   see if we're changing the value, and if so, rebuilding the
   menus based on that info. */
static void
allowed_cb (DBusGProxy *proxy, gboolean OUT_allowed, GError *error, gpointer userdata)
{
	if (error != NULL) {
		g_warning("Unable to get information on what is allowed from UPower: %s", error->message);
		return;
	}

	gboolean * can_do = (gboolean *)userdata;

	if (OUT_allowed != *can_do) {
		*can_do = OUT_allowed;
		rebuild_session_items (session_root_menuitem);
	}
}

/* This function goes through and sets up what we need for
   DKp checking.  We're even setting up the calls for the props
   we need */
static void
setup_up (void) {
	DBusGConnection * bus = dbus_g_bus_get(DBUS_BUS_SYSTEM, NULL);
	g_return_if_fail(bus != NULL);

	if (up_main_proxy == NULL) {
		up_main_proxy = dbus_g_proxy_new_for_name(bus,
		                                           UP_ADDRESS,
		                                           UP_OBJECT,
		                                           UP_INTERFACE);
	}
	g_return_if_fail(up_main_proxy != NULL);

	if (up_prop_proxy == NULL) {
		up_prop_proxy = dbus_g_proxy_new_for_name(bus,
		                                           UP_ADDRESS,
		                                           UP_OBJECT,
		                                           DBUS_INTERFACE_PROPERTIES);
		/* Connect to changed signal */
		dbus_g_proxy_add_signal(up_main_proxy,
		                        "Changed",
		                        G_TYPE_INVALID);

		dbus_g_proxy_connect_signal(up_main_proxy,
		                            "Changed",
		                            G_CALLBACK(up_changed_cb),
		                            NULL,
		                            NULL);
	}
	g_return_if_fail(up_prop_proxy != NULL);


	/* Force an original "changed" event */
	up_changed_cb(up_main_proxy, NULL);

	/* Check to see if these are getting blocked by PolicyKit */
	org_freedesktop_UPower_suspend_allowed_async(up_main_proxy,
	                                             allowed_cb,
	                                             &allow_suspend);
	org_freedesktop_UPower_hibernate_allowed_async(up_main_proxy,
	                                               allowed_cb,
	                                               &allow_hibernate);

	return;
}

/* This is the function to show a dialog on actions that
   can destroy data.  Currently it just calls the GTK version
   but it seems that in the future it should figure out
   what's going on and something better. */
static void
show_dialog (DbusmenuMenuitem * mi, guint timestamp, gchar * type)
{
	gchar * helper = g_build_filename(LIBEXECDIR, "gtk-logout-helper", NULL);
	gchar * dialog_line = g_strdup_printf("%s --%s", helper, type);
	g_free(helper);

	g_debug("Showing dialog '%s'", dialog_line);

	GError * error = NULL;
	if (!g_spawn_command_line_async(dialog_line, &error)) {
		g_warning("Unable to show dialog: %s", error->message);
		g_error_free(error);
	}

	g_free(dialog_line);

	return;
}


static void
rebuild_session_items (DbusmenuMenuitem *root)
                       
{
  gboolean can_lockscreen;

  /* Make sure we have a valid GConf client, and build one
     if needed */
  ensure_gconf_client ();

  can_lockscreen = !gconf_client_get_bool ( gconf_client,
                                            LOCKDOWN_KEY_SCREENSAVER,
                                            NULL);
  /* Lock screen item */
  if (can_lockscreen) {
	lock_menuitem = dbusmenu_menuitem_new();
	dbusmenu_menuitem_property_set (lock_menuitem,
                                  DBUSMENU_MENUITEM_PROP_LABEL,
                                  _("Lock Screen"));

	gchar * shortcut = gconf_client_get_string(gconf_client, KEY_LOCK_SCREEN, NULL);
	if (shortcut != NULL) {
		g_debug("Lock screen shortcut: %s", shortcut);
		dbusmenu_menuitem_property_set_shortcut_string(lock_menuitem, shortcut);
		g_free(shortcut);
	} else {
		g_debug("Unable to get lock screen shortcut.");
	}

	g_signal_connect (G_OBJECT(lock_menuitem),
                    DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                    G_CALLBACK(lock_screen), NULL);
	dbusmenu_menuitem_child_append(root, lock_menuitem);
  }

	/* Start going through the session based items. */

	logout_mi = dbusmenu_menuitem_new();
	if (supress_confirmations()) {
		dbusmenu_menuitem_property_set (logout_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Log Out"));
	} else {
		dbusmenu_menuitem_property_set (logout_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Log Out\342\200\246"));
	}
	dbusmenu_menuitem_property_set_bool (logout_mi,
                                       DBUSMENU_MENUITEM_PROP_VISIBLE,
                                       show_logout());
	dbusmenu_menuitem_child_append(root, logout_mi);
	g_signal_connect( G_OBJECT(logout_mi),
                    DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                    G_CALLBACK(show_dialog), "logout");

	if (can_suspend && allow_suspend) {
		suspend_mi = dbusmenu_menuitem_new();
		dbusmenu_menuitem_property_set (suspend_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Suspend"));
		dbusmenu_menuitem_child_append (root, suspend_mi);
		g_signal_connect( G_OBJECT(suspend_mi),
                      DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                      G_CALLBACK(machine_sleep),
                      "Suspend");
	}

	if (can_hibernate && allow_hibernate) {
		hibernate_mi = dbusmenu_menuitem_new();
		dbusmenu_menuitem_property_set (hibernate_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Hibernate"));
		dbusmenu_menuitem_child_append(root, hibernate_mi);
		g_signal_connect (G_OBJECT(hibernate_mi),
                      DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                      G_CALLBACK(machine_sleep), "Hibernate");
	}

	restart_mi = dbusmenu_menuitem_new();
	dbusmenu_menuitem_property_set (restart_mi,
                                  DBUSMENU_MENUITEM_PROP_TYPE,
                                  RESTART_ITEM_TYPE);
	if (supress_confirmations()) {
		dbusmenu_menuitem_property_set (restart_mi,
                                    RESTART_ITEM_LABEL,
                                    _("Restart"));
	} else {
		dbusmenu_menuitem_property_set (restart_mi,
                                    RESTART_ITEM_LABEL,
                                    _("Restart\342\200\246"));
	}
	dbusmenu_menuitem_property_set_bool (restart_mi,
                                       DBUSMENU_MENUITEM_PROP_VISIBLE,
                                       show_restart());
	dbusmenu_menuitem_child_append(root, restart_mi);
	g_signal_connect (G_OBJECT(restart_mi),
                    DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                    G_CALLBACK(show_dialog), "restart");

	shutdown_mi = dbusmenu_menuitem_new();
	if (supress_confirmations()) {
		dbusmenu_menuitem_property_set (shutdown_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Shut Down"));
	} else {
		dbusmenu_menuitem_property_set (shutdown_mi,
                                    DBUSMENU_MENUITEM_PROP_LABEL,
                                    _("Shut Down\342\200\246"));
	}
	dbusmenu_menuitem_property_set_bool (shutdown_mi,
                                       DBUSMENU_MENUITEM_PROP_VISIBLE,
                                       show_shutdown());
	dbusmenu_menuitem_child_append (root, shutdown_mi);
	g_signal_connect (G_OBJECT(shutdown_mi),
                    DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                    G_CALLBACK(show_dialog), "shutdown");

	RestartShutdownLogoutMenuItems * restart_shutdown_logout_mi = g_new0 (RestartShutdownLogoutMenuItems, 1);
	restart_shutdown_logout_mi->logout_mi = logout_mi;
	restart_shutdown_logout_mi->restart_mi = restart_mi;
	restart_shutdown_logout_mi->shutdown_mi = shutdown_mi;

	update_menu_entries(restart_shutdown_logout_mi);

	return;
}


/* When the service interface starts to shutdown, we
   should follow it. */
void
service_shutdown (IndicatorService * service, gpointer user_data)
{
	if (mainloop != NULL) {
		g_debug("Service shutdown");
		g_main_loop_quit(mainloop);
	}
	return;
}

/* When the directory changes we need to figure out how our menu
   item should look. */
static void
restart_dir_changed (void)
{
	gboolean restart_required = g_file_test("/var/run/reboot-required", G_FILE_TEST_EXISTS);

	if (restart_required) {
		if (supress_confirmations()) {
			dbusmenu_menuitem_property_set(restart_mi, RESTART_ITEM_LABEL, _("Restart to Complete Update"));
		} else {
			dbusmenu_menuitem_property_set(restart_mi, RESTART_ITEM_LABEL, _("Restart to Complete Update\342\200\246"));
		}
		dbusmenu_menuitem_property_set(restart_mi, RESTART_ITEM_ICON, "system-restart-panel");
		if (session_dbus != NULL) {
			session_dbus_set_name(session_dbus, ICON_RESTART);
		}
	} else {	
		if (supress_confirmations()) {
			dbusmenu_menuitem_property_set(restart_mi, RESTART_ITEM_LABEL, _("Restart"));
		} else {
			dbusmenu_menuitem_property_set(restart_mi, RESTART_ITEM_LABEL, _("Restart\342\200\246"));
		}
		dbusmenu_menuitem_property_remove(restart_mi, RESTART_ITEM_ICON);
		if (session_dbus != NULL) {
			session_dbus_set_name(session_dbus, ICON_DEFAULT);
		}
	}
	return;
}

/* Buids a file watcher for the directory so that when it
   changes we can check to see if our reboot-required is
   there. */
static void
setup_restart_watch (void)
{
	GFile * filedir = g_file_new_for_path("/var/run");
	GFileMonitor * filemon = g_file_monitor_directory(filedir, G_FILE_MONITOR_NONE, NULL, NULL);
	if (filemon != NULL) {
		g_signal_connect(G_OBJECT(filemon), "changed", G_CALLBACK(restart_dir_changed), NULL);
	}
	restart_dir_changed();
	return;
}

/* Main, is well, main.  It brings everything up and throws
   us into the mainloop of no return. */
int
main (int argc, char ** argv)
{
  g_type_init();

	/* Setting up i18n and gettext.  Apparently, we need
	   all of these. */
	setlocale (LC_ALL, "");
	bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
	textdomain (GETTEXT_PACKAGE);

	IndicatorService * service = indicator_service_new_version (INDICATOR_SESSION_DBUS_NAME,
      		                                                    INDICATOR_SESSION_DBUS_VERSION);
	g_signal_connect(G_OBJECT(service),
                   INDICATOR_SERVICE_SIGNAL_SHUTDOWN,
                   G_CALLBACK(service_shutdown), NULL);

	session_dbus = session_dbus_new();

	g_idle_add(lock_screen_setup, NULL);

  session_root_menuitem = dbusmenu_menuitem_new();
  rebuild_session_items (session_root_menuitem);

  DbusmenuServer * server = dbusmenu_server_new(INDICATOR_SESSION_DBUS_OBJECT);
  dbusmenu_server_set_root(server, session_root_menuitem);
    
  // Users
  UserMenuMgr* user_mgr = user_menu_mgr_new (session_dbus);
    
  setup_restart_watch();
	setup_up();

  DbusmenuServer* users_server = dbusmenu_server_new (INDICATOR_USERS_DBUS_OBJECT);
  
  dbusmenu_server_set_root (users_server, user_mgr_get_root_item (user_mgr));

  mainloop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(mainloop);
  
  return 0;
}

