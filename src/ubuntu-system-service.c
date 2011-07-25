/*
Copyright 2011 Canonical Ltd.

Authors:
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

#include "ubuntu-system-service.h"
#include <gio/gio.h>

static guint uss_watcher_id;

struct _UbuntuSystemService
{
	GObject parent_instance;
	GCancellable * proxy_cancel;
	GDBusProxy * proxy;
  SessionDbus* session_dbus_interface;
};

static void ubuntu_system_service_on_name_appeared (GDBusConnection *connection,
                                                    const gchar     *name,
                                                    const gchar     *name_owner,
                                                    gpointer         user_data);
static void ubuntu_system_service_on_name_vanished (GDBusConnection *connection,
                                                    const gchar     *name,
                                                    gpointer         user_data);
static void ubuntu_system_service_fetch_proxy_cb (GObject * object,
                                                  GAsyncResult * res,
                                                  gpointer user_data);

G_DEFINE_TYPE (UbuntuSystemService, ubuntu_system_service, G_TYPE_OBJECT);

static void
ubuntu_system_service_init (UbuntuSystemService *self)
{
  self->proxy_cancel = g_cancellable_new();
  self->proxy = NULL;
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                            G_DBUS_PROXY_FLAGS_NONE,
                            NULL,
                            "org.debian.apt",
                            "/org/debian/apt",
                            "org.debian.apt",
                            self->proxy_cancel,
                            ubuntu_system_service_fetch_proxy_cb,
                            self);    
}

static void
ubuntu_system_service_finalize (GObject *object)
{
	G_OBJECT_CLASS (ubuntu_system_service_parent_class)->finalize (object);
}

static void
ubuntu_system_service_class_init (UbuntuSystemServiceClass *klass)
{
	GObjectClass* object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = ubuntu_system_service_finalize;
}

static void
ubuntu_system_service_fetch_proxy_cb (GObject * object,
                                     GAsyncResult * res,
                                     gpointer user_data)
{
	GError * error = NULL;

	UbuntuSystemService* self = UBUNTU_SYSTEM_SERVICE(user_data);
	g_return_if_fail(self != NULL);

	GDBusProxy * proxy = g_dbus_proxy_new_for_bus_finish(res, &error);

	if (self->proxy_cancel != NULL) {
		g_object_unref(self->proxy_cancel);
		self->proxy_cancel = NULL;
	}

	if (error != NULL) {
		g_warning("Could not grab DBus proxy for %s: %s",
               "com.ubuntu.systemservice", error->message);
		g_error_free(error);
		return;
	}

	self->proxy = proxy;
  // Set up the watch.
  uss_watcher_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                     "org.debian.apt",
                                     G_BUS_NAME_WATCHER_FLAGS_NONE,
                                     ubuntu_system_service_on_name_appeared,
                                     ubuntu_system_service_on_name_vanished,
                                     self,
                                     NULL);  
}

static void
ubuntu_system_service_on_name_appeared (GDBusConnection *connection,
                                        const gchar     *name,
                                        const gchar     *name_owner,
                                        gpointer         user_data)
{
  g_return_if_fail (UBUNTU_SYSTEM_SERVICE (user_data));
  //UbuntuSystemService* watcher = UBUNTU_SYSTEM_SERVICE (user_data);
  
  g_print ("Name %s on %s is owned by %s\n",
           name,
           "the system bus",
           name_owner);

  /*
  g_dbus_proxy_call (watcher->proxy,
                     "UpgradeSystem",
                     g_variant_new("(b)", TRUE),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,
                     apt_watcher_upgrade_system_cb,
                     user_data);
                     */
}

static void
ubuntu_system_service_on_name_vanished (GDBusConnection *connection,
                                        const gchar     *name,
                                        gpointer         user_data)
{
  g_debug ("Name %s does not exist or has just vanished",
           name);
  g_return_if_fail (UBUNTU_SYSTEM_SERVICE (user_data));
}



UbuntuSystemService* ubuntu_system_service_new (SessionDbus* session_dbus)
{
  UbuntuSystemService* ubuntu_mgr = g_object_new (UBUNTU_TYPE_SYSTEM_SERVICE, NULL);
  ubuntu_mgr->session_dbus_interface = session_dbus;
  return ubuntu_mgr;
}

