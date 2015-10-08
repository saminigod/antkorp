/* GtkIconTheme - a loader for icon themes
 * gtk-icon-theme.c Copyright (C) 2002, 2003 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <glib.h>
#include <glib/gstdio.h>

#ifdef G_OS_WIN32
#ifndef S_ISDIR
#define S_ISDIR(mode) ((mode)&_S_IFDIR)
#endif
#define WIN32_MEAN_AND_LEAN
#include <windows.h>
#include "win32/gdkwin32.h"
#endif /* G_OS_WIN32 */

#include "gtkicontheme.h"
#include "gtkdebug.h"
#include "deprecated/gtkiconfactory.h"
#include "gtkiconcache.h"
#include "gtkbuiltincache.h"
#include "gtkintl.h"
#include "gtkmain.h"
#include "gtknumerableiconprivate.h"
#include "gtksettings.h"
#include "gtkprivate.h"

#undef GDK_DEPRECATED
#undef GDK_DEPRECATED_FOR
#define GDK_DEPRECATED
#define GDK_DEPRECATED_FOR(f)

#include "deprecated/gtkstyle.h"

/* this is in case round() is not provided by the compiler, 
 * such as in the case of C89 compilers, like MSVC
 */
#include "fallback-c89.c"

/**
 * SECTION:gtkicontheme
 * @Short_description: Looking up icons by name
 * @Title: GtkIconTheme
 *
 * #GtkIconTheme provides a facility for looking up icons by name
 * and size. The main reason for using a name rather than simply
 * providing a filename is to allow different icons to be used
 * depending on what “icon theme” is selected
 * by the user. The operation of icon themes on Linux and Unix
 * follows the [Icon Theme Specification](http://www.freedesktop.org/Standards/icon-theme-spec)
 * There is a default icon theme,
 * named `hicolor` where applications should install
 * their icons, but more additional application themes can be
 * installed as operating system vendors and users choose.
 *
 * Named icons are similar to the deprecated [Stock Items][gtkstock],
 * and the distinction between the two may be a bit confusing.
 * A few things to keep in mind:
 * 
 * - Stock images usually are used in conjunction with
 *   [Stock Items][gtkstock], such as %GTK_STOCK_OK or
 *   %GTK_STOCK_OPEN. Named icons are easier to set up and therefore
 *   are more useful for new icons that an application wants to
 *   add, such as application icons or window icons.
 * 
 * - Stock images can only be loaded at the symbolic sizes defined
 *   by the #GtkIconSize enumeration, or by custom sizes defined
 *   by gtk_icon_size_register(), while named icons are more flexible
 *   and any pixel size can be specified.
 * 
 * - Because stock images are closely tied to stock items, and thus
 *   to actions in the user interface, stock images may come in
 *   multiple variants for different widget states or writing
 *   directions.
 *
 * A good rule of thumb is that if there is a stock image for what
 * you want to use, use it, otherwise use a named icon. It turns
 * out that internally stock images are generally defined in
 * terms of one or more named icons. (An example of the
 * more than one case is icons that depend on writing direction;
 * %GTK_STOCK_GO_FORWARD uses the two themed icons
 * “gtk-stock-go-forward-ltr” and “gtk-stock-go-forward-rtl”.)
 *
 * In many cases, named themes are used indirectly, via #GtkImage
 * or stock items, rather than directly, but looking up icons
 * directly is also simple. The #GtkIconTheme object acts
 * as a database of all the icons in the current theme. You
 * can create new #GtkIconTheme objects, but it’s much more
 * efficient to use the standard icon theme for the #GdkScreen
 * so that the icon information is shared with other people
 * looking up icons. In the case where the default screen is
 * being used, looking up an icon can be as simple as:
 * |[<!-- language="C" -->
 * GError *error = NULL;
 * GtkIconTheme *icon_theme;
 * GdkPixbuf *pixbuf;
 *
 * icon_theme = gtk_icon_theme_get_default ();
 * pixbuf = gtk_icon_theme_load_icon (icon_theme,
 *                                    "my-icon-name", // icon name
 *                                    48, // icon size
 *                                    0,  // flags
 *                                    &error);
 * if (!pixbuf)
 *   {
 *     g_warning ("Couldn’t load icon: %s", error->message);
 *     g_error_free (error);
 *   }
 * else
 *   {
 *     // Use the pixbuf
 *     g_object_unref (pixbuf);
 *   }
 * ]|
 */


#define DEFAULT_THEME_NAME "hicolor"

typedef enum
{
  ICON_THEME_DIR_FIXED,  
  ICON_THEME_DIR_SCALABLE,  
  ICON_THEME_DIR_THRESHOLD,
  ICON_THEME_DIR_UNTHEMED
} IconThemeDirType;

/* In reverse search order: */
typedef enum
{
  ICON_SUFFIX_NONE = 0,
  ICON_SUFFIX_XPM = 1 << 0,
  ICON_SUFFIX_SVG = 1 << 1,
  ICON_SUFFIX_PNG = 1 << 2,
  HAS_ICON_FILE = 1 << 3
} IconSuffix;

#define INFO_CACHE_LRU_SIZE 32
#if 0
#define DEBUG_CACHE(args) g_print args
#else
#define DEBUG_CACHE(args)
#endif

struct _GtkIconThemePrivate
{
  GHashTable *info_cache;
  GList *info_cache_lru;

  gchar *current_theme;
  gchar **search_path;
  gint search_path_len;

  guint custom_theme        : 1;
  guint is_screen_singleton : 1;
  guint pixbuf_supports_svg : 1;
  guint themes_valid        : 1;
  guint loading_themes      : 1;

  /* A list of all the themes needed to look up icons.
   * In search order, without duplicates
   */
  GList *themes;
  GHashTable *unthemed_icons;

  /* Note: The keys of this hashtable are owned by the
   * themedir and unthemed hashtables.
   */
  GHashTable *all_icons;

  /* GdkScreen for the icon theme (may be NULL)
   */
  GdkScreen *screen;

  /* time when we last stat:ed for theme changes */
  glong last_stat_time;
  GList *dir_mtimes;

  gulong theme_changed_idle;
};

typedef struct {
  gchar **icon_names;
  gint size;
  gint scale;
  GtkIconLookupFlags flags;
} IconInfoKey;

typedef struct _SymbolicPixbufCache SymbolicPixbufCache;

struct _SymbolicPixbufCache {
  GdkPixbuf *pixbuf;
  GdkPixbuf *proxy_pixbuf;
  GdkRGBA  fg;
  GdkRGBA  success_color;
  GdkRGBA  warning_color;
  GdkRGBA  error_color;
  SymbolicPixbufCache *next;
};

struct _GtkIconInfoClass
{
  GObjectClass parent_class;
};

struct _GtkIconInfo
{
  GObject parent_instance;

  /* Information about the source
   */
  IconInfoKey key;
  GtkIconTheme *in_cache;

  gchar *filename;
  GFile *icon_file;
  GLoadableIcon *loadable;
  GSList *emblem_infos;

  /* Cache pixbuf (if there is any) */
  GdkPixbuf *cache_pixbuf;

  GtkIconData *data;

  /* Information about the directory where
   * the source was found
   */
  IconThemeDirType dir_type;
  gint dir_size;
  gint dir_scale;
  gdouble unscaled_scale;
  gint threshold;

  /* Parameters influencing the scaled icon
   */
  gint desired_size;
  gint desired_scale;
  guint raw_coordinates : 1;
  guint forced_size     : 1;
  guint emblems_applied : 1;

  /* Cached information if we go ahead and try to load
   * the icon.
   */
  GdkPixbuf *pixbuf;
  GdkPixbuf *proxy_pixbuf;
  GError *load_error;
  gdouble scale;

  SymbolicPixbufCache *symbolic_pixbuf_cache;

  GtkRequisition *symbolic_pixbuf_size;
};

typedef struct
{
  char *name;
  char *display_name;
  char *comment;
  char *example;

  /* In search order */
  GList *dirs;
} IconTheme;

typedef struct
{
  IconThemeDirType type;
  GQuark context;

  int size;
  int min_size;
  int max_size;
  int threshold;
  int scale;

  char *dir;
  char *subdir;
  int subdir_index;
  
  GtkIconCache *cache;
  
  GHashTable *icons;
  GHashTable *icon_data;
} IconThemeDir;

typedef struct
{
  char *svg_filename;
  char *no_svg_filename;
} UnthemedIcon;

typedef struct
{
  gint size;
  GdkPixbuf *pixbuf;
} BuiltinIcon;

typedef struct 
{
  char *dir;
  time_t mtime; /* 0 == not existing or not a dir */

  GtkIconCache *cache;
} IconThemeDirMtime;

static void  gtk_icon_theme_finalize   (GObject              *object);
static void  theme_dir_destroy         (IconThemeDir         *dir);

static void         theme_destroy     (IconTheme        *theme);
static GtkIconInfo *theme_lookup_icon (IconTheme        *theme,
				       const char       *icon_name,
				       int               size,
                                       gint              scale,
				       gboolean          allow_svg,
				       gboolean          use_default_icons);
static void         theme_list_icons  (IconTheme        *theme,
				       GHashTable       *icons,
				       GQuark            context);
static void         theme_list_contexts  (IconTheme        *theme,
					  GHashTable       *contexts);
static void         theme_subdir_load (GtkIconTheme     *icon_theme,
				       IconTheme        *theme,
				       GKeyFile         *theme_file,
				       char             *subdir);
static void         do_theme_change   (GtkIconTheme     *icon_theme);

static void     blow_themes               (GtkIconTheme    *icon_themes);
static gboolean rescan_themes             (GtkIconTheme    *icon_themes);

static GtkIconData *icon_data_dup      (GtkIconData     *icon_data);
static GtkIconData *icon_data_ref      (GtkIconData     *icon_data);
static void  icon_data_unref           (GtkIconData     *icon_data);
static void load_icon_data             (IconThemeDir    *dir,
			                const char      *path,
			                const char      *name);

static IconSuffix theme_dir_get_icon_suffix (IconThemeDir *dir,
					     const gchar  *icon_name,
					     gboolean     *has_icon_file);


static GtkIconInfo *icon_info_new             (IconThemeDirType type, int dir_size, int dir_scale);
static GtkIconInfo *icon_info_new_builtin     (BuiltinIcon *icon);

static IconSuffix suffix_from_name (const char *name);

static BuiltinIcon *find_builtin_icon (const gchar *icon_name,
				       gint        size,
				       gint        scale,
				       gint        *min_difference_p);
static void remove_from_lru_cache (GtkIconTheme *icon_theme,
				   GtkIconInfo *icon_info);

static guint signal_changed = 0;

static GHashTable *icon_theme_builtin_icons;

/* also used in gtkiconfactory.c */
GtkIconCache *_builtin_cache = NULL;
static GList *builtin_dirs = NULL;

static guint
icon_info_key_hash (gconstpointer _key)
{
  const IconInfoKey *key = _key;
  guint h = 0;
  int i;
  for (i = 0; key->icon_names[i] != NULL; i++)
    h ^= g_str_hash (key->icon_names[i]);

  h ^= key->size * 0x10001;
  h ^= key->scale * 0x1000010;
  h ^= key->flags * 0x100000100;

  return h;
}

static gboolean
icon_info_key_equal (gconstpointer  _a,
		     gconstpointer  _b)
{
  const IconInfoKey *a = _a;
  const IconInfoKey *b = _b;
  int i;

  if (a->size != b->size)
    return FALSE;

  if (a->scale != b->scale)
    return FALSE;

  if (a->flags != b->flags)
    return FALSE;

  for (i = 0;
       a->icon_names[i] != NULL &&
       b->icon_names[i] != NULL; i++)
    {
      if (strcmp (a->icon_names[i], b->icon_names[i]) != 0)
	return FALSE;
    }

  return a->icon_names[i] == NULL && b->icon_names[i] == NULL;
}

G_DEFINE_TYPE_WITH_PRIVATE (GtkIconTheme, gtk_icon_theme, G_TYPE_OBJECT)

/**
 * gtk_icon_theme_new:
 * 
 * Creates a new icon theme object. Icon theme objects are used
 * to lookup up an icon by name in a particular icon theme.
 * Usually, you’ll want to use gtk_icon_theme_get_default()
 * or gtk_icon_theme_get_for_screen() rather than creating
 * a new icon theme object for scratch.
 * 
 * Returns: the newly created #GtkIconTheme object.
 *
 * Since: 2.4
 **/
GtkIconTheme *
gtk_icon_theme_new (void)
{
  return g_object_new (GTK_TYPE_ICON_THEME, NULL);
}

/**
 * gtk_icon_theme_get_default:
 * 
 * Gets the icon theme for the default screen. See
 * gtk_icon_theme_get_for_screen().
 *
 * Returns: (transfer none): A unique #GtkIconTheme associated with
 *  the default screen. This icon theme is associated with
 *  the screen and can be used as long as the screen
 *  is open. Do not ref or unref it.
 *
 * Since: 2.4
 **/
GtkIconTheme *
gtk_icon_theme_get_default (void)
{
  return gtk_icon_theme_get_for_screen (gdk_screen_get_default ());
}

/**
 * gtk_icon_theme_get_for_screen:
 * @screen: a #GdkScreen
 * 
 * Gets the icon theme object associated with @screen; if this
 * function has not previously been called for the given
 * screen, a new icon theme object will be created and
 * associated with the screen. Icon theme objects are
 * fairly expensive to create, so using this function
 * is usually a better choice than calling than gtk_icon_theme_new()
 * and setting the screen yourself; by using this function
 * a single icon theme object will be shared between users.
 *
 * Returns: (transfer none): A unique #GtkIconTheme associated with
 *  the given screen. This icon theme is associated with
 *  the screen and can be used as long as the screen
 *  is open. Do not ref or unref it.
 *
 * Since: 2.4
 **/
GtkIconTheme *
gtk_icon_theme_get_for_screen (GdkScreen *screen)
{
  GtkIconTheme *icon_theme;

  g_return_val_if_fail (GDK_IS_SCREEN (screen), NULL);

  icon_theme = g_object_get_data (G_OBJECT (screen), "gtk-icon-theme");
  if (!icon_theme)
    {
      GtkIconThemePrivate *priv;

      icon_theme = gtk_icon_theme_new ();
      gtk_icon_theme_set_screen (icon_theme, screen);

      priv = icon_theme->priv;
      priv->is_screen_singleton = TRUE;

      g_object_set_data (G_OBJECT (screen), I_("gtk-icon-theme"), icon_theme);
    }

  return icon_theme;
}

static void
gtk_icon_theme_class_init (GtkIconThemeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gtk_icon_theme_finalize;

/**
 * GtkIconTheme::changed:
 * @icon_theme: the icon theme
 * 
 * Emitted when the current icon theme is switched or GTK+ detects
 * that a change has occurred in the contents of the current
 * icon theme.
 **/
  signal_changed = g_signal_new (I_("changed"),
				 G_TYPE_FROM_CLASS (klass),
				 G_SIGNAL_RUN_LAST,
				 G_STRUCT_OFFSET (GtkIconThemeClass, changed),
				 NULL, NULL,
				 g_cclosure_marshal_VOID__VOID,
				 G_TYPE_NONE, 0);
}


/* Callback when the display that the icon theme is attached to
 * is closed; unset the screen, and if it’s the unique theme
 * for the screen, drop the reference
 */
static void
display_closed (GdkDisplay   *display,
		gboolean      is_error,
		GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv = icon_theme->priv;
  GdkScreen *screen = priv->screen;
  gboolean was_screen_singleton = priv->is_screen_singleton;

  if (was_screen_singleton)
    {
      g_object_set_data (G_OBJECT (screen), I_("gtk-icon-theme"), NULL);
      priv->is_screen_singleton = FALSE;
    }

  gtk_icon_theme_set_screen (icon_theme, NULL);

  if (was_screen_singleton)
    {
      g_object_unref (icon_theme);
    }
}

static void
update_current_theme (GtkIconTheme *icon_theme)
{
#define theme_changed(_old, _new) \
  ((_old && !_new) || (!_old && _new) || \
   (_old && _new && strcmp (_old, _new) != 0))
  GtkIconThemePrivate *priv = icon_theme->priv;

  if (!priv->custom_theme)
    {
      gchar *theme = NULL;
      gboolean changed = FALSE;

      if (priv->screen)
	{
	  GtkSettings *settings = gtk_settings_get_for_screen (priv->screen);
	  g_object_get (settings, "gtk-icon-theme-name", &theme, NULL);
	}

      if (theme_changed (priv->current_theme, theme))
	{
	  g_free (priv->current_theme);
	  priv->current_theme = theme;
	  changed = TRUE;
	}
      else
	g_free (theme);

      if (changed)
	do_theme_change (icon_theme);
    }
#undef theme_changed
}

/* Callback when the icon theme GtkSetting changes
 */
static void
theme_changed (GtkSettings  *settings,
	       GParamSpec   *pspec,
	       GtkIconTheme *icon_theme)
{
  update_current_theme (icon_theme);
}

static void
unset_screen (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv = icon_theme->priv;
  GtkSettings *settings;
  GdkDisplay *display;
  
  if (priv->screen)
    {
      settings = gtk_settings_get_for_screen (priv->screen);
      display = gdk_screen_get_display (priv->screen);
      
      g_signal_handlers_disconnect_by_func (display,
					    (gpointer) display_closed,
					    icon_theme);
      g_signal_handlers_disconnect_by_func (settings,
					    (gpointer) theme_changed,
					    icon_theme);

      priv->screen = NULL;
    }
}

/**
 * gtk_icon_theme_set_screen:
 * @icon_theme: a #GtkIconTheme
 * @screen: a #GdkScreen
 * 
 * Sets the screen for an icon theme; the screen is used
 * to track the user’s currently configured icon theme,
 * which might be different for different screens.
 *
 * Since: 2.4
 **/
void
gtk_icon_theme_set_screen (GtkIconTheme *icon_theme,
			   GdkScreen    *screen)
{
  GtkIconThemePrivate *priv;
  GtkSettings *settings;
  GdkDisplay *display;

  g_return_if_fail (GTK_ICON_THEME (icon_theme));
  g_return_if_fail (screen == NULL || GDK_IS_SCREEN (screen));

  priv = icon_theme->priv;

  unset_screen (icon_theme);
  
  if (screen)
    {
      display = gdk_screen_get_display (screen);
      settings = gtk_settings_get_for_screen (screen);
      
      priv->screen = screen;
      
      g_signal_connect (display, "closed",
			G_CALLBACK (display_closed), icon_theme);
      g_signal_connect (settings, "notify::gtk-icon-theme-name",
			G_CALLBACK (theme_changed), icon_theme);
    }

  update_current_theme (icon_theme);
}

/* Checks whether a loader for SVG files has been registered
 * with GdkPixbuf.
 */
static gboolean
pixbuf_supports_svg (void)
{
  GSList *formats;
  GSList *tmp_list;
  static gint found_svg = -1;

  if (found_svg != -1)
    return found_svg;

  formats = gdk_pixbuf_get_formats ();

  found_svg = FALSE; 
  for (tmp_list = formats; tmp_list && !found_svg; tmp_list = tmp_list->next)
    {
      gchar **mime_types = gdk_pixbuf_format_get_mime_types (tmp_list->data);
      gchar **mime_type;
      
      for (mime_type = mime_types; *mime_type && !found_svg; mime_type++)
	{
	  if (strcmp (*mime_type, "image/svg") == 0)
	    found_svg = TRUE;
	}

      g_strfreev (mime_types);
    }

  g_slist_free (formats);
  
  return found_svg;
}

/* The icon info was removed from the icon_info_hash hash table */
static void
icon_info_uncached (GtkIconInfo *icon_info)
{
  GtkIconTheme *icon_theme = icon_info->in_cache;

  DEBUG_CACHE (("removing %p (%s %d 0x%x) from cache (icon_them: %p)  (cache size %d)\n",
		icon_info,
		g_strjoinv (",", icon_info->key.icon_names),
		icon_info->key.size, icon_info->key.flags,
		icon_theme,
		icon_theme != NULL ? g_hash_table_size (icon_theme->priv->info_cache) : 0));

  icon_info->in_cache = NULL;

  if (icon_theme != NULL)
    remove_from_lru_cache (icon_theme, icon_info);
}

static void
gtk_icon_theme_init (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv;
  const gchar * const *xdg_data_dirs;
  int i, j;

  priv = gtk_icon_theme_get_instance_private (icon_theme);
  icon_theme->priv = priv;

  priv->info_cache = g_hash_table_new_full (icon_info_key_hash, icon_info_key_equal, NULL,
					    (GDestroyNotify)icon_info_uncached);

  priv->custom_theme = FALSE;

  xdg_data_dirs = g_get_system_data_dirs ();
  for (i = 0; xdg_data_dirs[i]; i++) ;

  priv->search_path_len = 2 * i + 2;
  
  priv->search_path = g_new (char *, priv->search_path_len);
  
  i = 0;
  priv->search_path[i++] = g_build_filename (g_get_user_data_dir (), "icons", NULL);
  priv->search_path[i++] = g_build_filename (g_get_home_dir (), ".icons", NULL);
  
  for (j = 0; xdg_data_dirs[j]; j++) 
    priv->search_path[i++] = g_build_filename (xdg_data_dirs[j], "icons", NULL);

  for (j = 0; xdg_data_dirs[j]; j++) 
    priv->search_path[i++] = g_build_filename (xdg_data_dirs[j], "pixmaps", NULL);

  priv->themes_valid = FALSE;
  priv->themes = NULL;
  priv->unthemed_icons = NULL;
  
  priv->pixbuf_supports_svg = pixbuf_supports_svg ();
}

static void
free_dir_mtime (IconThemeDirMtime *dir_mtime)
{
  if (dir_mtime->cache)
    _gtk_icon_cache_unref (dir_mtime->cache);

  g_free (dir_mtime->dir);
  g_slice_free (IconThemeDirMtime, dir_mtime);

}

static gboolean
theme_changed_idle (gpointer user_data)
{
  GtkIconTheme *icon_theme;
  GtkIconThemePrivate *priv;

  icon_theme = GTK_ICON_THEME (user_data);
  priv = icon_theme->priv;

  g_signal_emit (icon_theme, signal_changed, 0);

  if (priv->screen && priv->is_screen_singleton)
    gtk_style_context_reset_widgets (priv->screen);

  priv->theme_changed_idle = 0;

  return FALSE;
}

static void
queue_theme_changed (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv = icon_theme->priv;

  if (!priv->theme_changed_idle)
    {
      priv->theme_changed_idle =
        gdk_threads_add_idle_full (GTK_PRIORITY_RESIZE - 2,
                                   theme_changed_idle, icon_theme, NULL);
      g_source_set_name_by_id (priv->theme_changed_idle, "[gtk+] theme_changed_idle");
    }
}

static void
do_theme_change (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv = icon_theme->priv;

  g_hash_table_remove_all (priv->info_cache);

  if (!priv->themes_valid)
    return;

  GTK_NOTE (ICONTHEME, 
	    g_print ("change to icon theme \"%s\"\n", priv->current_theme));
  blow_themes (icon_theme);

  queue_theme_changed (icon_theme);

}

static void
blow_themes (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv = icon_theme->priv;
  
  if (priv->themes_valid)
    {
      g_hash_table_destroy (priv->all_icons);
      g_list_free_full (priv->themes, (GDestroyNotify) theme_destroy);
      g_list_free_full (priv->dir_mtimes, (GDestroyNotify) free_dir_mtime);
      g_hash_table_destroy (priv->unthemed_icons);
    }
  priv->themes = NULL;
  priv->unthemed_icons = NULL;
  priv->dir_mtimes = NULL;
  priv->all_icons = NULL;
  priv->themes_valid = FALSE;
}

static void
gtk_icon_theme_finalize (GObject *object)
{
  GtkIconTheme *icon_theme;
  GtkIconThemePrivate *priv;
  int i;

  icon_theme = GTK_ICON_THEME (object);
  priv = icon_theme->priv;

  g_hash_table_destroy (priv->info_cache);
  g_assert (priv->info_cache_lru == NULL);

  if (priv->theme_changed_idle)
    {
      g_source_remove (priv->theme_changed_idle);
      priv->theme_changed_idle = 0;
    }

  unset_screen (icon_theme);

  g_free (priv->current_theme);
  priv->current_theme = NULL;

  for (i = 0; i < priv->search_path_len; i++)
    g_free (priv->search_path[i]);

  g_free (priv->search_path);
  priv->search_path = NULL;

  blow_themes (icon_theme);

  G_OBJECT_CLASS (gtk_icon_theme_parent_class)->finalize (object);  
}

/**
 * gtk_icon_theme_set_search_path:
 * @icon_theme: a #GtkIconTheme
 * @path: (array length=n_elements) (element-type filename): array of
 *     directories that are searched for icon themes
 * @n_elements: number of elements in @path.
 * 
 * Sets the search path for the icon theme object. When looking
 * for an icon theme, GTK+ will search for a subdirectory of
 * one or more of the directories in @path with the same name
 * as the icon theme. (Themes from multiple of the path elements
 * are combined to allow themes to be extended by adding icons
 * in the user’s home directory.)
 *
 * In addition if an icon found isn’t found either in the current
 * icon theme or the default icon theme, and an image file with
 * the right name is found directly in one of the elements of
 * @path, then that image will be used for the icon name.
 * (This is legacy feature, and new icons should be put
 * into the default icon theme, which is called hicolor,
 * rather than directly on the icon path.)
 *
 * Since: 2.4
 **/
void
gtk_icon_theme_set_search_path (GtkIconTheme *icon_theme,
				const gchar  *path[],
				gint          n_elements)
{
  GtkIconThemePrivate *priv;
  gint i;

  g_return_if_fail (GTK_IS_ICON_THEME (icon_theme));

  priv = icon_theme->priv;
  for (i = 0; i < priv->search_path_len; i++)
    g_free (priv->search_path[i]);

  g_free (priv->search_path);

  priv->search_path = g_new (gchar *, n_elements);
  priv->search_path_len = n_elements;

  for (i = 0; i < priv->search_path_len; i++)
    priv->search_path[i] = g_strdup (path[i]);

  do_theme_change (icon_theme);
}


/**
 * gtk_icon_theme_get_search_path:
 * @icon_theme: a #GtkIconTheme
 * @path: (allow-none) (array length=n_elements) (element-type filename) (out):
 *     location to store a list of icon theme path directories or %NULL.
 *     The stored value should be freed with g_strfreev().
 * @n_elements: location to store number of elements in @path, or %NULL
 *
 * Gets the current search path. See gtk_icon_theme_set_search_path().
 *
 * Since: 2.4
 */
void
gtk_icon_theme_get_search_path (GtkIconTheme  *icon_theme,
                                gchar        **path[],
                                gint          *n_elements)
{
  GtkIconThemePrivate *priv;
  int i;

  g_return_if_fail (GTK_IS_ICON_THEME (icon_theme));

  priv = icon_theme->priv;

  if (n_elements)
    *n_elements = priv->search_path_len;
  
  if (path)
    {
      *path = g_new (gchar *, priv->search_path_len + 1);
      for (i = 0; i < priv->search_path_len; i++)
	(*path)[i] = g_strdup (priv->search_path[i]);
      (*path)[i] = NULL;
    }
}

/**
 * gtk_icon_theme_append_search_path:
 * @icon_theme: a #GtkIconTheme
 * @path: (type filename): directory name to append to the icon path
 * 
 * Appends a directory to the search path. 
 * See gtk_icon_theme_set_search_path(). 
 *
 * Since: 2.4
 **/
void
gtk_icon_theme_append_search_path (GtkIconTheme *icon_theme,
				   const gchar  *path)
{
  GtkIconThemePrivate *priv;

  g_return_if_fail (GTK_IS_ICON_THEME (icon_theme));
  g_return_if_fail (path != NULL);

  priv = icon_theme->priv;
  
  priv->search_path_len++;

  priv->search_path = g_renew (gchar *, priv->search_path, priv->search_path_len);
  priv->search_path[priv->search_path_len-1] = g_strdup (path);

  do_theme_change (icon_theme);
}

/**
 * gtk_icon_theme_prepend_search_path:
 * @icon_theme: a #GtkIconTheme
 * @path: (type filename): directory name to prepend to the icon path
 * 
 * Prepends a directory to the search path. 
 * See gtk_icon_theme_set_search_path().
 *
 * Since: 2.4
 **/
void
gtk_icon_theme_prepend_search_path (GtkIconTheme *icon_theme,
				    const gchar  *path)
{
  GtkIconThemePrivate *priv;
  int i;

  g_return_if_fail (GTK_IS_ICON_THEME (icon_theme));
  g_return_if_fail (path != NULL);

  priv = icon_theme->priv;
  
  priv->search_path_len++;
  priv->search_path = g_renew (gchar *, priv->search_path, priv->search_path_len);

  for (i = priv->search_path_len - 1; i > 0; i--)
    priv->search_path[i] = priv->search_path[i - 1];
  
  priv->search_path[0] = g_strdup (path);

  do_theme_change (icon_theme);
}

/**
 * gtk_icon_theme_set_custom_theme:
 * @icon_theme: a #GtkIconTheme
 * @theme_name: (allow-none): name of icon theme to use instead of
 *   configured theme, or %NULL to unset a previously set custom theme
 * 
 * Sets the name of the icon theme that the #GtkIconTheme object uses
 * overriding system configuration. This function cannot be called
 * on the icon theme objects returned from gtk_icon_theme_get_default()
 * and gtk_icon_theme_get_for_screen().
 *
 * Since: 2.4
 **/
void
gtk_icon_theme_set_custom_theme (GtkIconTheme *icon_theme,
				 const gchar  *theme_name)
{
  GtkIconThemePrivate *priv;

  g_return_if_fail (GTK_IS_ICON_THEME (icon_theme));

  priv = icon_theme->priv;

  g_return_if_fail (!priv->is_screen_singleton);
  
  if (theme_name != NULL)
    {
      priv->custom_theme = TRUE;
      if (!priv->current_theme || strcmp (theme_name, priv->current_theme) != 0)
	{
	  g_free (priv->current_theme);
	  priv->current_theme = g_strdup (theme_name);

	  do_theme_change (icon_theme);
	}
    }
  else
    {
      if (priv->custom_theme)
	{
	  priv->custom_theme = FALSE;

	  update_current_theme (icon_theme);
	}
    }
}

static void
insert_theme (GtkIconTheme *icon_theme, const char *theme_name)
{
  int i;
  GList *l;
  char **dirs;
  char **scaled_dirs;
  char **themes;
  GtkIconThemePrivate *priv;
  IconTheme *theme = NULL;
  char *path;
  GKeyFile *theme_file;
  GError *error = NULL;
  IconThemeDirMtime *dir_mtime;
  GStatBuf stat_buf;
  
  priv = icon_theme->priv;

  for (l = priv->themes; l != NULL; l = l->next)
    {
      theme = l->data;
      if (strcmp (theme->name, theme_name) == 0)
	return;
    }
  
  for (i = 0; i < priv->search_path_len; i++)
    {
      path = g_build_filename (priv->search_path[i],
			       theme_name,
			       NULL);
      dir_mtime = g_slice_new (IconThemeDirMtime);
      dir_mtime->cache = NULL;
      dir_mtime->dir = path;
      if (g_stat (path, &stat_buf) == 0 && S_ISDIR (stat_buf.st_mode))
	dir_mtime->mtime = stat_buf.st_mtime;
      else
	dir_mtime->mtime = 0;

      priv->dir_mtimes = g_list_prepend (priv->dir_mtimes, dir_mtime);
    }
  priv->dir_mtimes = g_list_reverse (priv->dir_mtimes);

  theme_file = NULL;
  for (i = 0; i < priv->search_path_len && !theme_file; i++)
    {
      path = g_build_filename (priv->search_path[i],
			       theme_name,
			       "index.theme",
			       NULL);
      if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) 
	{
	  theme_file = g_key_file_new ();
	  g_key_file_set_list_separator (theme_file, ',');
	  g_key_file_load_from_file (theme_file, path, 0, &error);
	  if (error)
	    {
	      g_key_file_free (theme_file);
	      theme_file = NULL;
	      g_error_free (error);
	      error = NULL;
	    }
	}
      g_free (path);
    }

  if (theme_file || strcmp (theme_name, DEFAULT_THEME_NAME) == 0)
    {
      theme = g_new0 (IconTheme, 1);
      theme->name = g_strdup (theme_name);
      priv->themes = g_list_prepend (priv->themes, theme);
    }

  if (theme_file == NULL)
    return;

  theme->display_name = 
    g_key_file_get_locale_string (theme_file, "Icon Theme", "Name", NULL, NULL);
  if (!theme->display_name)
    g_warning ("Theme file for %s has no name\n", theme_name);

  dirs = g_key_file_get_string_list (theme_file, "Icon Theme", "Directories", NULL, NULL);
  if (!dirs)
    {
      g_warning ("Theme file for %s has no directories\n", theme_name);
      priv->themes = g_list_remove (priv->themes, theme);
      g_free (theme->name);
      g_free (theme->display_name);
      g_free (theme);
      g_key_file_free (theme_file);
      return;
    }

  scaled_dirs = g_key_file_get_string_list (theme_file, "Icon Theme", "ScaledDirectories", NULL, NULL);

  theme->comment = 
    g_key_file_get_locale_string (theme_file, 
				  "Icon Theme", "Comment",
				  NULL, NULL);
  theme->example = 
    g_key_file_get_string (theme_file, 
			   "Icon Theme", "Example",
			   NULL);

  theme->dirs = NULL;
  for (i = 0; dirs[i] != NULL; i++)
    theme_subdir_load (icon_theme, theme, theme_file, dirs[i]);

  if (scaled_dirs)
    {
      for (i = 0; scaled_dirs[i] != NULL; i++)
        theme_subdir_load (icon_theme, theme, theme_file, scaled_dirs[i]);
    }
  g_strfreev (dirs);
  g_strfreev (scaled_dirs);

  theme->dirs = g_list_reverse (theme->dirs);

  themes = g_key_file_get_string_list (theme_file,
				       "Icon Theme",
				       "Inherits",
				       NULL,
				       NULL);
  if (themes)
    {
      for (i = 0; themes[i] != NULL; i++)
	insert_theme (icon_theme, themes[i]);
      
      g_strfreev (themes);
    }

  g_key_file_free (theme_file);
}

static void
free_unthemed_icon (UnthemedIcon *unthemed_icon)
{
  g_free (unthemed_icon->svg_filename);
  g_free (unthemed_icon->no_svg_filename);
  g_slice_free (UnthemedIcon, unthemed_icon);
}

static char *
strip_suffix (const char *filename)
{
  const char *dot;

  dot = strrchr (filename, '.');

  if (dot == NULL)
    return g_strdup (filename);

  return g_strndup (filename, dot - filename);
}

static void
load_themes (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv;
  GDir *gdir;
  int base;
  char *dir;
  const char *file;
  UnthemedIcon *unthemed_icon;
  IconSuffix old_suffix, new_suffix;
  GTimeVal tv;
  IconThemeDirMtime *dir_mtime;
  GStatBuf stat_buf;
  
  priv = icon_theme->priv;

  priv->all_icons = g_hash_table_new (g_str_hash, g_str_equal);
  
  if (priv->current_theme)
    insert_theme (icon_theme, priv->current_theme);

  /* Always look in the gnome and hicolor icon themes.
   * Looking in hicolor is mandated by the spec, looking
   * in gnome is a pragmatic solution to prevent missing
   * icons in GTK+ applications when run under, e.g. KDE.
   */
  insert_theme (icon_theme, "gnome");
  insert_theme (icon_theme, DEFAULT_THEME_NAME);
  priv->themes = g_list_reverse (priv->themes);


  priv->unthemed_icons = g_hash_table_new_full (g_str_hash, g_str_equal,
						g_free, (GDestroyNotify)free_unthemed_icon);

  for (base = 0; base < icon_theme->priv->search_path_len; base++)
    {
      dir = icon_theme->priv->search_path[base];

      dir_mtime = g_slice_new (IconThemeDirMtime);
      priv->dir_mtimes = g_list_append (priv->dir_mtimes, dir_mtime);
      
      dir_mtime->dir = g_strdup (dir);
      dir_mtime->mtime = 0;
      dir_mtime->cache = NULL;

      if (g_stat (dir, &stat_buf) != 0 || !S_ISDIR (stat_buf.st_mode))
	continue;
      dir_mtime->mtime = stat_buf.st_mtime;

      dir_mtime->cache = _gtk_icon_cache_new_for_path (dir);
      if (dir_mtime->cache != NULL)
	continue;

      gdir = g_dir_open (dir, 0, NULL);
      if (gdir == NULL)
	continue;

      while ((file = g_dir_read_name (gdir)))
	{
	  new_suffix = suffix_from_name (file);
	  
	  if (new_suffix != ICON_SUFFIX_NONE)
	    {
	      char *abs_file;
	      char *base_name;

	      abs_file = g_build_filename (dir, file, NULL);
	      base_name = strip_suffix (file);

	      if ((unthemed_icon = g_hash_table_lookup (priv->unthemed_icons,
							base_name)))
		{
		  if (new_suffix == ICON_SUFFIX_SVG)
		    {
		      if (unthemed_icon->svg_filename)
			g_free (abs_file);
		      else
			unthemed_icon->svg_filename = abs_file;
		    }
		  else
		    {
		      if (unthemed_icon->no_svg_filename)
			{
			  old_suffix = suffix_from_name (unthemed_icon->no_svg_filename);
			  if (new_suffix > old_suffix)
			    {
			      g_free (unthemed_icon->no_svg_filename);
			      unthemed_icon->no_svg_filename = abs_file;			      
			    }
			  else
			    g_free (abs_file);
			}
		      else
			unthemed_icon->no_svg_filename = abs_file;			      
		    }

		  g_free (base_name);
		}
	      else
		{
		  unthemed_icon = g_slice_new0 (UnthemedIcon);
		  
		  if (new_suffix == ICON_SUFFIX_SVG)
		    unthemed_icon->svg_filename = abs_file;
		  else
		    unthemed_icon->no_svg_filename = abs_file;

		  /* takes ownership of base_name */
		  g_hash_table_replace (priv->unthemed_icons,
					base_name,
					unthemed_icon);
		  g_hash_table_insert (priv->all_icons,
				       base_name, NULL);
		}
	    }
	}
      g_dir_close (gdir);
    }

  priv->themes_valid = TRUE;
  
  g_get_current_time(&tv);
  priv->last_stat_time = tv.tv_sec;
}

void
_gtk_icon_theme_ensure_builtin_cache (void)
{
  static gboolean initialized = FALSE;
  IconThemeDir *dir;
  static IconThemeDir dirs[5] = 
    {
      { ICON_THEME_DIR_THRESHOLD, 0, 16, 16, 16, 2, 1, NULL, "16", -1, NULL, NULL, NULL },
      { ICON_THEME_DIR_THRESHOLD, 0, 20, 20, 20, 2, 1, NULL, "20", -1,  NULL, NULL, NULL },
      { ICON_THEME_DIR_THRESHOLD, 0, 24, 24, 24, 2, 1, NULL, "24", -1, NULL, NULL, NULL },
      { ICON_THEME_DIR_THRESHOLD, 0, 32, 32, 32, 2, 1, NULL, "32", -1, NULL, NULL, NULL },
      { ICON_THEME_DIR_THRESHOLD, 0, 48, 48, 48, 2, 1, NULL, "48", -1, NULL, NULL, NULL }
    };
  gint i;

  if (!initialized)
    {
      initialized = TRUE;

      _builtin_cache = _gtk_icon_cache_new ((gchar *)builtin_icons);

      for (i = 0; i < G_N_ELEMENTS (dirs); i++)
	{
	  dir = &(dirs[i]);
	  dir->cache = _gtk_icon_cache_ref (_builtin_cache);
          dir->subdir_index = _gtk_icon_cache_get_directory_index (dir->cache, dir->subdir);

	  builtin_dirs = g_list_append (builtin_dirs, dir);
	}
    }
}

static void
ensure_valid_themes (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv = icon_theme->priv;
  GTimeVal tv;
  gboolean was_valid = priv->themes_valid;

  if (priv->loading_themes)
    return;
  priv->loading_themes = TRUE;

  _gtk_icon_theme_ensure_builtin_cache ();

  if (priv->themes_valid)
    {
      g_get_current_time (&tv);

      if (ABS (tv.tv_sec - priv->last_stat_time) > 5 &&
	  rescan_themes (icon_theme))
        {
          g_hash_table_remove_all (priv->info_cache);
          blow_themes (icon_theme);
        }
    }
  
  if (!priv->themes_valid)
    {
      load_themes (icon_theme);

      if (was_valid)
        queue_theme_changed (icon_theme);
    }

  priv->loading_themes = FALSE;
}

/* The LRU cache is a short list of IconInfos that are kept
   alive even though their IconInfo would otherwise have
   been freed, so that we can avoid reloading these
   constantly.
   We put infos on the lru list when nothing otherwise
   references the info. So, when we get a cache hit
   we remove it from the list, and when the proxy
   pixmap is released we put it on the list.
*/

static void
ensure_lru_cache_space (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv = icon_theme->priv;
  GList *l;

  /* Remove last item if LRU full */
  l = g_list_nth (priv->info_cache_lru, INFO_CACHE_LRU_SIZE - 1);
  if (l)
    {
      GtkIconInfo *icon_info = l->data;

      DEBUG_CACHE (("removing (due to out of space) %p (%s %d 0x%x) from LRU cache (cache size %d)\n",
		    icon_info,
		    g_strjoinv (",", icon_info->key.icon_names),
		    icon_info->key.size, icon_info->key.flags,
		    g_list_length (priv->info_cache_lru)));

      priv->info_cache_lru = g_list_delete_link (priv->info_cache_lru, l);
      g_object_unref (icon_info);
    }
}

static void
add_to_lru_cache (GtkIconTheme *icon_theme,
		  GtkIconInfo *icon_info)
{
  GtkIconThemePrivate *priv = icon_theme->priv;

  DEBUG_CACHE (("adding  %p (%s %d 0x%x) to LRU cache (cache size %d)\n",
		icon_info,
		g_strjoinv (",", icon_info->key.icon_names),
		icon_info->key.size, icon_info->key.flags,
		g_list_length (priv->info_cache_lru)));

  g_assert (g_list_find (priv->info_cache_lru, icon_info) == NULL);

  ensure_lru_cache_space (icon_theme);
  /* prepend new info to LRU */
  priv->info_cache_lru = g_list_prepend (priv->info_cache_lru,
					 g_object_ref (icon_info));
}

static void
ensure_in_lru_cache (GtkIconTheme *icon_theme,
		     GtkIconInfo *icon_info)
{
  GtkIconThemePrivate *priv = icon_theme->priv;
  GList *l;

  l = g_list_find (priv->info_cache_lru, icon_info);
  if (l)
    {
      /* Move to front of LRU if already in it */
      priv->info_cache_lru = g_list_remove_link (priv->info_cache_lru, l);
      priv->info_cache_lru = g_list_concat (l, priv->info_cache_lru);
    }
  else
    add_to_lru_cache (icon_theme, icon_info);
}

static void
remove_from_lru_cache (GtkIconTheme *icon_theme,
		       GtkIconInfo *icon_info)
{
  GtkIconThemePrivate *priv = icon_theme->priv;
  if (g_list_find (priv->info_cache_lru, icon_info))
    {
      DEBUG_CACHE (("removing %p (%s %d 0x%x) from LRU cache (cache size %d)\n",
		    icon_info,
		    g_strjoinv (",", icon_info->key.icon_names),
		    icon_info->key.size, icon_info->key.flags,
		    g_list_length (priv->info_cache_lru)));

      priv->info_cache_lru = g_list_remove (priv->info_cache_lru, icon_info);
      g_object_unref (icon_info);
    }
}

static SymbolicPixbufCache *
symbolic_pixbuf_cache_new (GdkPixbuf *pixbuf,
			   const GdkRGBA  *fg,
			   const GdkRGBA  *success_color,
			   const GdkRGBA  *warning_color,
			   const GdkRGBA  *error_color,
			   SymbolicPixbufCache *next)
{
  SymbolicPixbufCache *cache;

  cache = g_new0 (SymbolicPixbufCache, 1);
  cache->pixbuf = g_object_ref (pixbuf);
  if (fg)
    cache->fg = *fg;
  if (success_color)
    cache->success_color = *success_color;
  if (warning_color)
    cache->warning_color = *warning_color;
  if (error_color)
    cache->error_color = *error_color;
  cache->next = next;
  return cache;
}

static gboolean
rgba_matches (const GdkRGBA  *a, const GdkRGBA  *b)
{
  GdkRGBA transparent = { 0 };

  /* For matching we treat unset colors as transparent rather
     than default, which works as well, because transparent
     will never be used for real symbolic icon colors */
  if (a == NULL)
    a = &transparent;

  return
    fabs(a->red - b->red) < 0.0001 &&
    fabs(a->green - b->green) < 0.0001 &&
    fabs(a->blue - b->blue) < 0.0001 &&
    fabs(a->alpha - b->alpha) < 0.0001;
}

static SymbolicPixbufCache *
symbolic_pixbuf_cache_matches (SymbolicPixbufCache *cache,
			       const GdkRGBA  *fg,
			       const GdkRGBA  *success_color,
			       const GdkRGBA  *warning_color,
			       const GdkRGBA  *error_color)
{
  while (cache != NULL)
    {
      if (rgba_matches (fg, &cache->fg) &&
	  rgba_matches (success_color, &cache->success_color) &&
	  rgba_matches (warning_color, &cache->warning_color) &&
	  rgba_matches (error_color, &cache->error_color))
	return cache;

      cache = cache->next;
    }

  return NULL;
}

static void
symbolic_pixbuf_cache_free (SymbolicPixbufCache *cache)
{
  SymbolicPixbufCache *next;

  while (cache != NULL)
    {
      next = cache->next;
      g_object_unref (cache->pixbuf);
      g_free (cache);

      cache = next;
    }
}

static GtkIconInfo *
choose_icon (GtkIconTheme       *icon_theme,
	     const gchar        *icon_names[],
	     gint                size,
             gint                scale,
	     GtkIconLookupFlags  flags)
{
  GtkIconThemePrivate *priv;
  GList *l;
  GtkIconInfo *icon_info = NULL;
  GtkIconInfo *unscaled_icon_info;
  UnthemedIcon *unthemed_icon = NULL;
  const char *icon_name = NULL;
  gboolean allow_svg;
  gboolean use_builtin;
  IconTheme *theme = NULL;
  gint i;
  IconInfoKey key;

  priv = icon_theme->priv;

  ensure_valid_themes (icon_theme);

  key.icon_names = (char **)icon_names;
  key.size = size;
  key.scale = scale;
  key.flags = flags;

  icon_info = g_hash_table_lookup (priv->info_cache, &key);
  if (icon_info != NULL)
    {
      DEBUG_CACHE (("cache hit %p (%s %d 0x%x) (cache size %d)\n",
		    icon_info,
		    g_strjoinv (",", icon_info->key.icon_names),
		    icon_info->key.size, icon_info->key.flags,
		    g_hash_table_size (priv->info_cache)));

      icon_info = g_object_ref (icon_info);
      remove_from_lru_cache (icon_theme, icon_info);

      return icon_info;
    }

  if (flags & GTK_ICON_LOOKUP_NO_SVG)
    allow_svg = FALSE;
  else if (flags & GTK_ICON_LOOKUP_FORCE_SVG)
    allow_svg = TRUE;
  else
    allow_svg = priv->pixbuf_supports_svg;

  use_builtin = flags & GTK_ICON_LOOKUP_USE_BUILTIN;

  /* for symbolic icons, do a search in all registered themes first;
   * a theme that inherits them from a parent theme might provide
   * an alternative highcolor version, but still expect the symbolic icon
   * to show up instead.
   */
  if (icon_names[0] &&
      g_str_has_suffix (icon_names[0], "-symbolic"))
    {
      for (l = priv->themes; l; l = l->next)
        {
          theme = l->data;
          icon_name = icon_names[0];
          icon_info = theme_lookup_icon (theme, icon_name, size, scale, allow_svg, use_builtin);
          if (icon_info)
            goto out;
        }
    }

  for (l = priv->themes; l; l = l->next)
    {
      theme = l->data;

      for (i = 0; icon_names[i]; i++)
        {
          icon_name = icon_names[i];
          icon_info = theme_lookup_icon (theme, icon_name, size, scale, allow_svg, use_builtin);
          if (icon_info)
            goto out;
        }
    }

  theme = NULL;

  for (i = 0; icon_names[i]; i++)
    {
      unthemed_icon = g_hash_table_lookup (priv->unthemed_icons, icon_names[i]);
      if (unthemed_icon)
        break;
    }
#ifdef G_OS_WIN32
  /* Still not found an icon, check if reference to a Win32 resource */
  if (!unthemed_icon)
    {
      gchar **resources;
      HICON hIcon = NULL;
      
      resources = g_strsplit (icon_names[0], ",", 0);
      if (resources[0])
	{
	  wchar_t *wfile = g_utf8_to_utf16 (resources[0], -1, NULL, NULL, NULL);
	  ExtractIconExW (wfile, resources[1] ? atoi (resources[1]) : 0, &hIcon, NULL, 1);
	  g_free (wfile);
	}
      
      if (hIcon)
	{
	  icon_info = icon_info_new (ICON_THEME_DIR_UNTHEMED, size, 1);
	  icon_info->cache_pixbuf = gdk_win32_icon_to_pixbuf_libgtk_only (hIcon, NULL, NULL);
	  DestroyIcon (hIcon);
	}
      g_strfreev (resources);
    }
#endif

  if (unthemed_icon)
    {
      icon_info = icon_info_new (ICON_THEME_DIR_UNTHEMED, size, 1);

      /* A SVG icon, when allowed, beats out a XPM icon, but not
       * a PNG icon
       */
      if (allow_svg &&
	  unthemed_icon->svg_filename &&
	  (!unthemed_icon->no_svg_filename ||
	   suffix_from_name (unthemed_icon->no_svg_filename) != ICON_SUFFIX_PNG))
	icon_info->filename = g_strdup (unthemed_icon->svg_filename);
      else if (unthemed_icon->no_svg_filename)
	icon_info->filename = g_strdup (unthemed_icon->no_svg_filename);

      icon_info->icon_file = g_file_new_for_path (icon_info->filename);
    }

 out:
  if (icon_info)
    {
      icon_info->desired_size = size;
      icon_info->desired_scale = scale;
      icon_info->forced_size = (flags & GTK_ICON_LOOKUP_FORCE_SIZE) != 0;

      /* In case we're not scaling the icon we want to reuse the exact same size
       * as a scale==1 lookup would be, rather than not scaling at all and
       * causing a different layout */
      icon_info->unscaled_scale = 1.0;
      if (scale != 1 && !icon_info->forced_size && theme != NULL)
        {
          unscaled_icon_info = theme_lookup_icon (theme, icon_name, size, 1, allow_svg, use_builtin);
          if (unscaled_icon_info)
            {
              icon_info->unscaled_scale =
                (gdouble) unscaled_icon_info->dir_size * scale / (icon_info->dir_size * icon_info->dir_scale);
              g_object_unref (unscaled_icon_info);
            }
        }

      icon_info->key.icon_names = g_strdupv ((char **)icon_names);
      icon_info->key.size = size;
      icon_info->key.scale = scale;
      icon_info->key.flags = flags;
      icon_info->in_cache = icon_theme;
      DEBUG_CACHE (("adding %p (%s %d 0x%x) to cache (cache size %d)\n",
		    icon_info,
		    g_strjoinv (",", icon_info->key.icon_names),
		    icon_info->key.size, icon_info->key.flags,
		    g_hash_table_size (priv->info_cache)));
     g_hash_table_insert (priv->info_cache, &icon_info->key, icon_info);
    }
  else
    {
      static gboolean check_for_default_theme = TRUE;
      char *default_theme_path;
      gboolean found = FALSE;
      unsigned i;

      if (check_for_default_theme)
	{
	  check_for_default_theme = FALSE;

	  for (i = 0; !found && i < priv->search_path_len; i++)
	    {
	      default_theme_path = g_build_filename (priv->search_path[i],
						     DEFAULT_THEME_NAME,
						     "index.theme",
						     NULL);
	      found = g_file_test (default_theme_path, G_FILE_TEST_IS_REGULAR);
	      g_free (default_theme_path);
	    }

	  if (!found)
	    {
	      g_warning ("Could not find the icon '%s'. The '%s' theme\n"
			 "was not found either, perhaps you need to install it.\n"
			 "You can get a copy from:\n"
			 "\t%s",
			 icon_names[0], DEFAULT_THEME_NAME, "http://icon-theme.freedesktop.org/releases");
	    }
	}
    }

  return icon_info;
}


/**
 * gtk_icon_theme_lookup_icon:
 * @icon_theme: a #GtkIconTheme
 * @icon_name: the name of the icon to lookup
 * @size: desired icon size
 * @flags: flags modifying the behavior of the icon lookup
 * 
 * Looks up a named icon and returns a #GtkIconInfo containing
 * information such as the filename of the icon. The icon
 * can then be rendered into a pixbuf using
 * gtk_icon_info_load_icon(). (gtk_icon_theme_load_icon()
 * combines these two steps if all you need is the pixbuf.)
 * 
 * Returns: (transfer full): a #GtkIconInfo object containing information
 * about the icon, or %NULL if the icon wasn’t found.
 *
 * Since: 2.4
 */
GtkIconInfo *
gtk_icon_theme_lookup_icon (GtkIconTheme       *icon_theme,
			    const gchar        *icon_name,
			    gint                size,
			    GtkIconLookupFlags  flags)
{
  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (icon_name != NULL, NULL);
  g_return_val_if_fail ((flags & GTK_ICON_LOOKUP_NO_SVG) == 0 ||
			(flags & GTK_ICON_LOOKUP_FORCE_SVG) == 0, NULL);

  GTK_NOTE (ICONTHEME, 
	    g_print ("gtk_icon_theme_lookup_icon %s\n", icon_name));

  return gtk_icon_theme_lookup_icon_for_scale (icon_theme, icon_name,
                                               size, 1, flags);
}

/**
 * gtk_icon_theme_lookup_icon_for_scale:
 * @icon_theme: a #GtkIconTheme
 * @icon_name: the name of the icon to lookup
 * @size: desired icon size
 * @scale: the desired scale
 * @flags: flags modifying the behavior of the icon lookup
 *
 * Looks up a named icon for a particular window scale and returns a
 * #GtkIconInfo containing information such as the filename of the
 * icon. The icon can then be rendered into a pixbuf using
 * gtk_icon_info_load_icon(). (gtk_icon_theme_load_icon() combines
 * these two steps if all you need is the pixbuf.)
 *
 * Returns: (transfer full): a #GtkIconInfo object containing
 * information about the icon, or %NULL if the icon wasn’t found.
 *
 * Since: 3.10
 */
GtkIconInfo *
gtk_icon_theme_lookup_icon_for_scale (GtkIconTheme       *icon_theme,
                                      const gchar        *icon_name,
                                      gint                size,
                                      gint                scale,
                                      GtkIconLookupFlags  flags)
{
  GtkIconInfo *info;

  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (icon_name != NULL, NULL);
  g_return_val_if_fail ((flags & GTK_ICON_LOOKUP_NO_SVG) == 0 ||
			(flags & GTK_ICON_LOOKUP_FORCE_SVG) == 0, NULL);
  g_return_val_if_fail (scale >= 1, NULL);

  GTK_NOTE (ICONTHEME, 
	    g_print ("gtk_icon_theme_lookup_icon %s\n", icon_name));

  if (flags & GTK_ICON_LOOKUP_GENERIC_FALLBACK)
    {
      gchar **names, **nonsymbolic_names;
      gint dashes, i;
      gchar *p, *nonsymbolic_icon_name;
      gboolean is_symbolic;

      is_symbolic = g_str_has_suffix (icon_name, "-symbolic");
      if (is_symbolic)
        nonsymbolic_icon_name = g_strndup (icon_name, strlen (icon_name) - 9);
      else
        nonsymbolic_icon_name = g_strdup (icon_name);
 
      dashes = 0;
      for (p = (gchar *) nonsymbolic_icon_name; *p; p++)
        if (*p == '-')
          dashes++;

      nonsymbolic_names = g_new (gchar *, dashes + 2);
      nonsymbolic_names[0] = nonsymbolic_icon_name;

      for (i = 1; i <= dashes; i++)
        {
          nonsymbolic_names[i] = g_strdup (nonsymbolic_names[i - 1]);
          p = strrchr (nonsymbolic_names[i], '-');
          *p = '\0';
        }
      nonsymbolic_names[dashes + 1] = NULL;

      if (is_symbolic)
        {
          names = g_new (gchar *, 2 * dashes + 3);
          for (i = 0; nonsymbolic_names[i] != NULL; i++)
            {
              names[i] = g_strconcat (nonsymbolic_names[i], "-symbolic", NULL);
              names[dashes + 1 + i] = nonsymbolic_names[i];
            }

          names[dashes + 1 + i] = NULL;
          g_free (nonsymbolic_names);
        }
      else
        {
          names = nonsymbolic_names;
        }

      info = choose_icon (icon_theme, (const gchar **) names, size, scale, flags);

      g_strfreev (names);
    }
  else
    {
      const gchar *names[2];

      names[0] = icon_name;
      names[1] = NULL;

      info = choose_icon (icon_theme, names, size, scale, flags);
    }

  return info;
}

/**
 * gtk_icon_theme_choose_icon:
 * @icon_theme: a #GtkIconTheme
 * @icon_names: (array zero-terminated=1): %NULL-terminated array of
 *     icon names to lookup
 * @size: desired icon size
 * @flags: flags modifying the behavior of the icon lookup
 * 
 * Looks up a named icon and returns a #GtkIconInfo containing
 * information such as the filename of the icon. The icon
 * can then be rendered into a pixbuf using
 * gtk_icon_info_load_icon(). (gtk_icon_theme_load_icon()
 * combines these two steps if all you need is the pixbuf.)
 *
 * If @icon_names contains more than one name, this function 
 * tries them all in the given order before falling back to 
 * inherited icon themes.
 * 
 * Returns: (transfer full): a #GtkIconInfo object containing information
 * about the icon, or %NULL if the icon wasn’t found.
 *
 * Since: 2.12
 */
GtkIconInfo *
gtk_icon_theme_choose_icon (GtkIconTheme       *icon_theme,
			    const gchar        *icon_names[],
			    gint                size,
			    GtkIconLookupFlags  flags)
{
  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (icon_names != NULL, NULL);
  g_return_val_if_fail ((flags & GTK_ICON_LOOKUP_NO_SVG) == 0 ||
			(flags & GTK_ICON_LOOKUP_FORCE_SVG) == 0, NULL);

  return choose_icon (icon_theme, icon_names, size, 1, flags);
}

/**
 * gtk_icon_theme_choose_icon_for_scale:
 * @icon_theme: a #GtkIconTheme
 * @icon_names: (array zero-terminated=1): %NULL-terminated array of
 *     icon names to lookup
 * @size: desired icon size
 * @scale: desired scale
 * @flags: flags modifying the behavior of the icon lookup
 * 
 * Looks up a named icon for a particular window scale and returns a
 * #GtkIconInfo containing information such as the filename of the
 * icon. The icon can then be rendered into a pixbuf using
 * gtk_icon_info_load_icon(). (gtk_icon_theme_load_icon() combines
 * these two steps if all you need is the pixbuf.)
 *
 * If @icon_names contains more than one name, this function 
 * tries them all in the given order before falling back to 
 * inherited icon themes.
 * 
 * Returns: (transfer full): a #GtkIconInfo object containing information
 * about the icon, or %NULL if the icon wasn’t found.
 *
 * Since: 3.10
 */
GtkIconInfo *
gtk_icon_theme_choose_icon_for_scale (GtkIconTheme       *icon_theme,
                                      const gchar        *icon_names[],
                                      gint                size,
                                      gint                scale,
                                      GtkIconLookupFlags  flags)
{
  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (icon_names != NULL, NULL);
  g_return_val_if_fail ((flags & GTK_ICON_LOOKUP_NO_SVG) == 0 ||
			(flags & GTK_ICON_LOOKUP_FORCE_SVG) == 0, NULL);
  g_return_val_if_fail (scale >= 1, NULL);

  return choose_icon (icon_theme, icon_names, size, scale, flags);
}


/* Error quark */
GQuark
gtk_icon_theme_error_quark (void)
{
  return g_quark_from_static_string ("gtk-icon-theme-error-quark");
}

/**
 * gtk_icon_theme_load_icon:
 * @icon_theme: a #GtkIconTheme
 * @icon_name: the name of the icon to lookup
 * @size: the desired icon size. The resulting icon may not be
 *     exactly this size; see gtk_icon_info_load_icon().
 * @flags: flags modifying the behavior of the icon lookup
 * @error: (allow-none): Location to store error information on failure,
 *     or %NULL.
 *
 * Looks up an icon in an icon theme, scales it to the given size
 * and renders it into a pixbuf. This is a convenience function;
 * if more details about the icon are needed, use
 * gtk_icon_theme_lookup_icon() followed by gtk_icon_info_load_icon().
 *
 * Note that you probably want to listen for icon theme changes and
 * update the icon. This is usually done by connecting to the
 * GtkWidget::style-set signal. If for some reason you do not want to
 * update the icon when the icon theme changes, you should consider
 * using gdk_pixbuf_copy() to make a private copy of the pixbuf
 * returned by this function. Otherwise GTK+ may need to keep the old
 * icon theme loaded, which would be a waste of memory.
 *
 * Returns: (transfer full): the rendered icon; this may be a
 *     newly created icon or a new reference to an internal icon, so
 *     you must not modify the icon. Use g_object_unref() to release
 *     your reference to the icon. %NULL if the icon isn’t found.
 *
 * Since: 2.4
 **/
GdkPixbuf *
gtk_icon_theme_load_icon (GtkIconTheme         *icon_theme,
			  const gchar          *icon_name,
			  gint                  size,
			  GtkIconLookupFlags    flags,
			  GError              **error)
{
  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (icon_name != NULL, NULL);
  g_return_val_if_fail ((flags & GTK_ICON_LOOKUP_NO_SVG) == 0 ||
			(flags & GTK_ICON_LOOKUP_FORCE_SVG) == 0, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return gtk_icon_theme_load_icon_for_scale (icon_theme, icon_name,
                                             size, 1, flags, error);
}

/**
 * gtk_icon_theme_load_icon_for_scale:
 * @icon_theme: a #GtkIconTheme
 * @icon_name: the name of the icon to lookup
 * @size: the desired icon size. The resulting icon may not be
 *     exactly this size; see gtk_icon_info_load_icon().
 * @scale: desired scale
 * @flags: flags modifying the behavior of the icon lookup
 * @error: (allow-none): Location to store error information on failure,
 *     or %NULL.
 *
 * Looks up an icon in an icon theme for a particular window scale,
 * scales it to the given size and renders it into a pixbuf. This is a
 * convenience function; if more details about the icon are needed,
 * use gtk_icon_theme_lookup_icon() followed by
 * gtk_icon_info_load_icon().
 *
 * Note that you probably want to listen for icon theme changes and
 * update the icon. This is usually done by connecting to the
 * GtkWidget::style-set signal. If for some reason you do not want to
 * update the icon when the icon theme changes, you should consider
 * using gdk_pixbuf_copy() to make a private copy of the pixbuf
 * returned by this function. Otherwise GTK+ may need to keep the old
 * icon theme loaded, which would be a waste of memory.
 *
 * Returns: (transfer full): the rendered icon; this may be a
 *     newly created icon or a new reference to an internal icon, so
 *     you must not modify the icon. Use g_object_unref() to release
 *     your reference to the icon. %NULL if the icon isn’t found.
 *
 * Since: 3.10
 **/
GdkPixbuf *
gtk_icon_theme_load_icon_for_scale (GtkIconTheme        *icon_theme,
                                    const gchar         *icon_name,
                                    gint                 size,
                                    gint                 scale,
                                    GtkIconLookupFlags   flags,
                                    GError             **error)
{
  GtkIconInfo *icon_info;
  GdkPixbuf *pixbuf = NULL;
  
  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (icon_name != NULL, NULL);
  g_return_val_if_fail ((flags & GTK_ICON_LOOKUP_NO_SVG) == 0 ||
			(flags & GTK_ICON_LOOKUP_FORCE_SVG) == 0, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (scale >= 1, NULL);

  icon_info = gtk_icon_theme_lookup_icon_for_scale (icon_theme, icon_name, size, scale,
                                                    flags | GTK_ICON_LOOKUP_USE_BUILTIN);
  if (!icon_info)
    {
      g_set_error (error, GTK_ICON_THEME_ERROR,  GTK_ICON_THEME_NOT_FOUND,
		   _("Icon '%s' not present in theme"), icon_name);
      return NULL;
    }

  pixbuf = gtk_icon_info_load_icon (icon_info, error);
  g_object_unref (icon_info);

  return pixbuf;
}

/**
 * gtk_icon_theme_load_surface:
 * @icon_theme: a #GtkIconTheme
 * @icon_name: the name of the icon to lookup
 * @size: the desired icon size. The resulting icon may not be
 *     exactly this size; see gtk_icon_info_load_icon().
 * @scale: desired scale
 * @for_window: (allow-none): #GdkWindow to optimize drawing for, or %NULL
 * @flags: flags modifying the behavior of the icon lookup
 * @error: (allow-none): Location to store error information on failure,
 *     or %NULL.
 *
 * Looks up an icon in an icon theme for a particular window scale,
 * scales it to the given size and renders it into a cairo surface. This is a
 * convenience function; if more details about the icon are needed,
 * use gtk_icon_theme_lookup_icon() followed by
 * gtk_icon_info_load_surface().
 *
 * Note that you probably want to listen for icon theme changes and
 * update the icon. This is usually done by connecting to the
 * GtkWidget::style-set signal.
 *
 * Returns: (transfer full): the rendered icon; this may be a
 *     newly created icon or a new reference to an internal icon, so
 *     you must not modify the icon. Use cairo_surface_destroy() to release
 *     your reference to the icon. %NULL if the icon isn’t found.
 *
 * Since: 3.10
 **/
cairo_surface_t *
gtk_icon_theme_load_surface (GtkIconTheme        *icon_theme,
			     const gchar         *icon_name,
			     gint                 size,
			     gint                 scale,
			     GdkWindow           *for_window,
			     GtkIconLookupFlags   flags,
			     GError             **error)
{
  GtkIconInfo *icon_info;
  cairo_surface_t *surface = NULL;
  
  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (icon_name != NULL, NULL);
  g_return_val_if_fail ((flags & GTK_ICON_LOOKUP_NO_SVG) == 0 ||
			(flags & GTK_ICON_LOOKUP_FORCE_SVG) == 0, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (scale >= 1, NULL);

  icon_info = gtk_icon_theme_lookup_icon_for_scale (icon_theme, icon_name, size, scale,
                                                    flags | GTK_ICON_LOOKUP_USE_BUILTIN);
  if (!icon_info)
    {
      g_set_error (error, GTK_ICON_THEME_ERROR,  GTK_ICON_THEME_NOT_FOUND,
		   _("Icon '%s' not present in theme"), icon_name);
      return NULL;
    }

  surface = gtk_icon_info_load_surface (icon_info, for_window, error);
  g_object_unref (icon_info);

  return surface;
}

/**
 * gtk_icon_theme_has_icon:
 * @icon_theme: a #GtkIconTheme
 * @icon_name: the name of an icon
 * 
 * Checks whether an icon theme includes an icon
 * for a particular name.
 * 
 * Returns: %TRUE if @icon_theme includes an
 *  icon for @icon_name.
 *
 * Since: 2.4
 **/
gboolean 
gtk_icon_theme_has_icon (GtkIconTheme *icon_theme,
			 const char   *icon_name)
{
  GtkIconThemePrivate *priv;
  GList *l;

  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), FALSE);
  g_return_val_if_fail (icon_name != NULL, FALSE);

  priv = icon_theme->priv;
  
  ensure_valid_themes (icon_theme);

  for (l = priv->dir_mtimes; l; l = l->next)
    {
      IconThemeDirMtime *dir_mtime = l->data;
      GtkIconCache *cache = dir_mtime->cache;
      
      if (cache && _gtk_icon_cache_has_icon (cache, icon_name))
	return TRUE;
    }

  if (g_hash_table_lookup_extended (priv->all_icons,
				    icon_name, NULL, NULL))
    return TRUE;

  if (_builtin_cache &&
      _gtk_icon_cache_has_icon (_builtin_cache, icon_name))
    return TRUE;

  if (icon_theme_builtin_icons &&
      g_hash_table_lookup_extended (icon_theme_builtin_icons,
				    icon_name, NULL, NULL))
    return TRUE;

  return FALSE;
}

static void
add_size (gpointer  key,
	  gpointer  value,
	  gpointer  user_data)
{
  gint **res_p = user_data;

  **res_p = GPOINTER_TO_INT (key);

  (*res_p)++;
}

/**
 * gtk_icon_theme_get_icon_sizes:
 * @icon_theme: a #GtkIconTheme
 * @icon_name: the name of an icon
 * 
 * Returns an array of integers describing the sizes at which
 * the icon is available without scaling. A size of -1 means 
 * that the icon is available in a scalable format. The array 
 * is zero-terminated.
 * 
 * Returns: (array zero-terminated=1): An newly allocated array
 * describing the sizes at which the icon is available. The array
 * should be freed with g_free() when it is no longer needed.
 *
 * Since: 2.6
 **/
gint *
gtk_icon_theme_get_icon_sizes (GtkIconTheme *icon_theme,
			       const char   *icon_name)
{
  GList *l, *d, *icons;
  GHashTable *sizes;
  gint *result, *r;
  guint suffix;  
  GtkIconThemePrivate *priv;

  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  
  priv = icon_theme->priv;

  ensure_valid_themes (icon_theme);

  sizes = g_hash_table_new (g_direct_hash, g_direct_equal);

  for (l = priv->themes; l; l = l->next)
    {
      IconTheme *theme = l->data;
      for (d = theme->dirs; d; d = d->next)
	{
	  IconThemeDir *dir = d->data;

          if (dir->type != ICON_THEME_DIR_SCALABLE && g_hash_table_lookup_extended (sizes, GINT_TO_POINTER (dir->size), NULL, NULL))
            continue;

	  suffix = theme_dir_get_icon_suffix (dir, icon_name, NULL);	  
	  if (suffix != ICON_SUFFIX_NONE)
	    {
	      if (suffix == ICON_SUFFIX_SVG)
		g_hash_table_insert (sizes, GINT_TO_POINTER (-1), NULL);
	      else
		g_hash_table_insert (sizes, GINT_TO_POINTER (dir->size), NULL);
	    }
	}
    }

  for (d = builtin_dirs; d; d = d->next)
    {
      IconThemeDir *dir = d->data;
      
      if (dir->type != ICON_THEME_DIR_SCALABLE && g_hash_table_lookup_extended (sizes, GINT_TO_POINTER (dir->size), NULL, NULL))
        continue;

      suffix = theme_dir_get_icon_suffix (dir, icon_name, NULL);	  
      if (suffix != ICON_SUFFIX_NONE)
	{
	  if (suffix == ICON_SUFFIX_SVG)
	    g_hash_table_insert (sizes, GINT_TO_POINTER (-1), NULL);
	  else
	    g_hash_table_insert (sizes, GINT_TO_POINTER (dir->size), NULL);
	}
    }

  if (icon_theme_builtin_icons)
    {
      icons = g_hash_table_lookup (icon_theme_builtin_icons, icon_name);
      
      while (icons)
        {
	  BuiltinIcon *icon = icons->data;
	
	  g_hash_table_insert (sizes, GINT_TO_POINTER (icon->size), NULL);
          icons = icons->next;
        }      
    }

  r = result = g_new0 (gint, g_hash_table_size (sizes) + 1);

  g_hash_table_foreach (sizes, add_size, &r);
  g_hash_table_destroy (sizes);
  
  return result;
}

static void
add_key_to_hash (gpointer  key,
		 gpointer  value,
		 gpointer  user_data)
{
  GHashTable *hash = user_data;

  g_hash_table_insert (hash, key, NULL);
}

static void
add_key_to_list (gpointer  key,
		 gpointer  value,
		 gpointer  user_data)
{
  GList **list = user_data;

  *list = g_list_prepend (*list, g_strdup (key));
}

/**
 * gtk_icon_theme_list_icons:
 * @icon_theme: a #GtkIconTheme
 * @context: (allow-none): a string identifying a particular type of
 *           icon, or %NULL to list all icons.
 * 
 * Lists the icons in the current icon theme. Only a subset
 * of the icons can be listed by providing a context string.
 * The set of values for the context string is system dependent,
 * but will typically include such values as “Applications” and
 * “MimeTypes”.
 *
 * Returns: (element-type utf8) (transfer full): a #GList list
 *  holding the names of all the icons in the theme. You must first
 *  free each element in the list with g_free(), then free the list
 *  itself with g_list_free().
 *
 * Since: 2.4
 **/
GList *
gtk_icon_theme_list_icons (GtkIconTheme *icon_theme,
			   const char   *context)
{
  GtkIconThemePrivate *priv;
  GHashTable *icons;
  GList *list, *l;
  GQuark context_quark;
  
  priv = icon_theme->priv;
  
  ensure_valid_themes (icon_theme);

  if (context)
    {
      context_quark = g_quark_try_string (context);

      if (!context_quark)
	return NULL;
    }
  else
    context_quark = 0;

  icons = g_hash_table_new (g_str_hash, g_str_equal);
  
  l = priv->themes;
  while (l != NULL)
    {
      theme_list_icons (l->data, icons, context_quark);
      l = l->next;
    }

  if (context_quark == 0)
    g_hash_table_foreach (priv->unthemed_icons,
			  add_key_to_hash,
			  icons);

  list = NULL;
  
  g_hash_table_foreach (icons,
			add_key_to_list,
			&list);

  g_hash_table_destroy (icons);
  
  return list;
}

/**
 * gtk_icon_theme_list_contexts:
 * @icon_theme: a #GtkIconTheme
 *
 * Gets the list of contexts available within the current
 * hierarchy of icon themes
 *
 * Returns: (element-type utf8) (transfer full): a #GList list
 *  holding the names of all the contexts in the theme. You must first
 *  free each element in the list with g_free(), then free the list
 *  itself with g_list_free().
 *
 * Since: 2.12
 **/
GList *
gtk_icon_theme_list_contexts (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv;
  GHashTable *contexts;
  GList *list, *l;

  priv = icon_theme->priv;
  
  ensure_valid_themes (icon_theme);

  contexts = g_hash_table_new (g_str_hash, g_str_equal);

  l = priv->themes;
  while (l != NULL)
    {
      theme_list_contexts (l->data, contexts);
      l = l->next;
    }

  list = NULL;

  g_hash_table_foreach (contexts,
			add_key_to_list,
			&list);

  g_hash_table_destroy (contexts);

  return list;
}

/**
 * gtk_icon_theme_get_example_icon_name:
 * @icon_theme: a #GtkIconTheme
 * 
 * Gets the name of an icon that is representative of the
 * current theme (for instance, to use when presenting
 * a list of themes to the user.)
 * 
 * Returns: the name of an example icon or %NULL.
 *  Free with g_free().
 *
 * Since: 2.4
 **/
char *
gtk_icon_theme_get_example_icon_name (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv;
  GList *l;
  IconTheme *theme;

  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  
  priv = icon_theme->priv;
  
  ensure_valid_themes (icon_theme);

  l = priv->themes;
  while (l != NULL)
    {
      theme = l->data;
      if (theme->example)
	return g_strdup (theme->example);
      
      l = l->next;
    }
  
  return NULL;
}


static gboolean
rescan_themes (GtkIconTheme *icon_theme)
{
  GtkIconThemePrivate *priv;
  IconThemeDirMtime *dir_mtime;
  GList *d;
  int stat_res;
  GStatBuf stat_buf;
  GTimeVal tv;

  priv = icon_theme->priv;

  for (d = priv->dir_mtimes; d != NULL; d = d->next)
    {
      dir_mtime = d->data;

      stat_res = g_stat (dir_mtime->dir, &stat_buf);

      /* dir mtime didn't change */
      if (stat_res == 0 &&
	  S_ISDIR (stat_buf.st_mode) &&
	  dir_mtime->mtime == stat_buf.st_mtime)
	continue;
      /* didn't exist before, and still doesn't */
      if (dir_mtime->mtime == 0 &&
	  (stat_res != 0 || !S_ISDIR (stat_buf.st_mode)))
	continue;

      return TRUE;
    }

  g_get_current_time (&tv);
  priv->last_stat_time = tv.tv_sec;

  return FALSE;
}

/**
 * gtk_icon_theme_rescan_if_needed:
 * @icon_theme: a #GtkIconTheme
 * 
 * Checks to see if the icon theme has changed; if it has, any
 * currently cached information is discarded and will be reloaded
 * next time @icon_theme is accessed.
 * 
 * Returns: %TRUE if the icon theme has changed and needed
 *   to be reloaded.
 *
 * Since: 2.4
 **/
gboolean
gtk_icon_theme_rescan_if_needed (GtkIconTheme *icon_theme)
{
  gboolean retval;

  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), FALSE);

  retval = rescan_themes (icon_theme);
  if (retval)
      do_theme_change (icon_theme);

  return retval;
}

static void
theme_destroy (IconTheme *theme)
{
  g_free (theme->display_name);
  g_free (theme->comment);
  g_free (theme->name);
  g_free (theme->example);

  g_list_free_full (theme->dirs, (GDestroyNotify) theme_dir_destroy);
  
  g_free (theme);
}

static void
theme_dir_destroy (IconThemeDir *dir)
{
  if (dir->cache)
      _gtk_icon_cache_unref (dir->cache);
  else
    g_hash_table_destroy (dir->icons);
  
  if (dir->icon_data)
    g_hash_table_destroy (dir->icon_data);
  g_free (dir->dir);
  g_free (dir->subdir);
  g_free (dir);
}

static int
theme_dir_size_difference (IconThemeDir *dir,
                           int           size,
                           gint          scale)
{
  int scaled_size, scaled_dir_size;
  int min, max;

  scaled_size = size * scale;
  scaled_dir_size = dir->size * dir->scale;

  switch (dir->type)
    {
    case ICON_THEME_DIR_FIXED:
      return abs (scaled_size - scaled_dir_size);
      break;
    case ICON_THEME_DIR_SCALABLE:
      if (scaled_size < (dir->min_size * dir->scale))
	return (dir->min_size * dir->scale) - scaled_size;
      if (size > (dir->max_size * dir->scale))
	return scaled_size - (dir->max_size * dir->scale);
      return 0;
      break;
    case ICON_THEME_DIR_THRESHOLD:
      min = (dir->size - dir->threshold) * dir->scale;
      max = (dir->size + dir->threshold) * dir->scale;
      if (scaled_size < min)
	return min - scaled_size;
      if (scaled_size > max)
	return scaled_size - max;
      return 0;
      break;
    case ICON_THEME_DIR_UNTHEMED:
      g_assert_not_reached ();
      break;
    }
  g_assert_not_reached ();
  return 1000;
}

static const char *
string_from_suffix (IconSuffix suffix)
{
  switch (suffix)
    {
    case ICON_SUFFIX_XPM:
      return ".xpm";
    case ICON_SUFFIX_SVG:
      return ".svg";
    case ICON_SUFFIX_PNG:
      return ".png";
    default:
      g_assert_not_reached();
    }
  return NULL;
}

static IconSuffix
suffix_from_name (const char *name)
{
  IconSuffix retval;

  if (g_str_has_suffix (name, ".png"))
    retval = ICON_SUFFIX_PNG;
  else if (g_str_has_suffix (name, ".svg"))
    retval = ICON_SUFFIX_SVG;
  else if (g_str_has_suffix (name, ".xpm"))
    retval = ICON_SUFFIX_XPM;
  else
    retval = ICON_SUFFIX_NONE;

  return retval;
}

static IconSuffix
best_suffix (IconSuffix suffix,
	     gboolean   allow_svg)
{
  if ((suffix & ICON_SUFFIX_PNG) != 0)
    return ICON_SUFFIX_PNG;
  else if (allow_svg && ((suffix & ICON_SUFFIX_SVG) != 0))
    return ICON_SUFFIX_SVG;
  else if ((suffix & ICON_SUFFIX_XPM) != 0)
    return ICON_SUFFIX_XPM;
  else
    return ICON_SUFFIX_NONE;
}


static IconSuffix
theme_dir_get_icon_suffix (IconThemeDir *dir,
			   const gchar  *icon_name,
			   gboolean     *has_icon_file)
{
  IconSuffix suffix;

  if (dir->cache)
    {
      suffix = (IconSuffix)_gtk_icon_cache_get_icon_flags (dir->cache,
							   icon_name,
							   dir->subdir_index);

      if (has_icon_file)
	*has_icon_file = suffix & HAS_ICON_FILE;

      suffix = suffix & ~HAS_ICON_FILE;
    }
  else
    suffix = GPOINTER_TO_UINT (g_hash_table_lookup (dir->icons, icon_name));

  GTK_NOTE (ICONTHEME, 
	    g_print ("get_icon_suffix%s %u\n", dir->cache ? " (cached)" : "", suffix));

  return suffix;
}

/* returns TRUE if dir_a is a better match */
static gboolean
compare_dir_matches (IconThemeDir *dir_a, int difference_a,
		     IconThemeDir *dir_b, int difference_b, 
		     int requested_size, int requested_scale)
{
  int diff_a;
  int diff_b;

  if (difference_a == 0)
    {
      if (difference_b != 0)
	return TRUE;
      
      /* a and b both exact matches */
    }
  else
    {
      /* If scaling, *always* prefer downscaling */
      if (dir_a->size >= requested_size &&
	  dir_b->size < requested_size)
	return TRUE;

      if (dir_a->size < requested_size &&
	  dir_b->size >= requested_size)
	return FALSE;
 
      /* Otherwise prefer the closest match */

      if (difference_a < difference_b)
	return TRUE;

      if (difference_a > difference_b)
	return FALSE;

      /* same pixel difference */
    }

  if (dir_a->scale == requested_scale &&
      dir_b->scale != requested_scale)
    return TRUE;

  if (dir_a->scale != requested_scale &&
      dir_b->scale == requested_scale)
    return FALSE;

  /* a and b both match the scale */

  if (dir_a->type != ICON_THEME_DIR_SCALABLE &&
      dir_b->type == ICON_THEME_DIR_SCALABLE)
    return TRUE;

  if (dir_a->type == ICON_THEME_DIR_SCALABLE &&
      dir_b->type != ICON_THEME_DIR_SCALABLE)
    return FALSE;
      
  /* a and b both are scalable */

  diff_a = abs (requested_size * requested_scale - dir_a->size * dir_a->scale);
  diff_b = abs (requested_size * requested_scale - dir_b->size * dir_b->scale);

  return diff_a <= diff_b;
}

static GtkIconInfo *
theme_lookup_icon (IconTheme          *theme,
		   const char         *icon_name,
		   int                 size,
                   gint                scale,
		   gboolean            allow_svg,
		   gboolean            use_builtin)
{
  GList *dirs, *l;
  IconThemeDir *dir, *min_dir;
  char *file;
  int min_difference, difference;
  BuiltinIcon *closest_builtin = NULL;
  IconSuffix suffix;

  min_difference = G_MAXINT;
  min_dir = NULL;

  /* Builtin icons are logically part of the default theme and
   * are searched before other subdirectories of the default theme.
   */
  if (use_builtin && strcmp (theme->name, DEFAULT_THEME_NAME) == 0)
    {
      closest_builtin = find_builtin_icon (icon_name, 
					   size, scale,
					   &min_difference);

      if (min_difference == 0)
	return icon_info_new_builtin (closest_builtin);

      dirs = builtin_dirs;
    }
  else
    dirs = theme->dirs;

  l = dirs;
  while (l != NULL)
    {
      dir = l->data;

      GTK_NOTE (ICONTHEME,
		g_print ("theme_lookup_icon dir %s\n", dir->dir));
      suffix = theme_dir_get_icon_suffix (dir, icon_name, NULL);
      if (best_suffix (suffix, allow_svg) != ICON_SUFFIX_NONE)
	{
	  difference = theme_dir_size_difference (dir, size, scale);
	  if (min_dir == NULL ||
	      compare_dir_matches (dir, difference,
				   min_dir, min_difference,
				   size, scale))
	    {
	      min_dir = dir;
	      min_difference = difference;
	    }
        }

      l = l->next;

      if (l == NULL && dirs == builtin_dirs)
	{
	  dirs = theme->dirs;
	  l = dirs;
	}
    }

  if (min_dir)
    {
      GtkIconInfo *icon_info = icon_info_new (min_dir->type, min_dir->size, min_dir->scale);
      gboolean has_icon_file = FALSE;

      icon_info->threshold = min_dir->threshold;

      suffix = theme_dir_get_icon_suffix (min_dir, icon_name, &has_icon_file);
      suffix = best_suffix (suffix, allow_svg);
      g_assert (suffix != ICON_SUFFIX_NONE);

      if (min_dir->dir)
        {
          file = g_strconcat (icon_name, string_from_suffix (suffix), NULL);
          icon_info->filename = g_build_filename (min_dir->dir, file, NULL);
          icon_info->icon_file = g_file_new_for_path (icon_info->filename);
          g_free (file);
        }
      else
        {
          icon_info->filename = NULL;
          icon_info->icon_file = NULL;
        }

      if (min_dir->icon_data != NULL)
        icon_info->data = icon_data_ref (g_hash_table_lookup (min_dir->icon_data, icon_name));

      if (icon_info->data == NULL && min_dir->cache != NULL)
	{
	  icon_info->data = _gtk_icon_cache_get_icon_data (min_dir->cache, icon_name, min_dir->subdir_index);
	  if (icon_info->data)
	    {
	      if (min_dir->icon_data == NULL)
		min_dir->icon_data = g_hash_table_new_full (g_str_hash, g_str_equal,
							    g_free, (GDestroyNotify)icon_data_unref);

	      g_hash_table_replace (min_dir->icon_data, g_strdup (icon_name), icon_data_ref (icon_info->data));
	    }
	}

      if (icon_info->data == NULL && has_icon_file)
	{
	  gchar *icon_file_name, *icon_file_path;

	  icon_file_name = g_strconcat (icon_name, ".icon", NULL);
	  icon_file_path = g_build_filename (min_dir->dir, icon_file_name, NULL);

	  if (g_file_test (icon_file_path, G_FILE_TEST_IS_REGULAR))
	    {
	      if (min_dir->icon_data == NULL)	
		min_dir->icon_data = g_hash_table_new_full (g_str_hash, g_str_equal,
							    g_free, (GDestroyNotify)icon_data_unref);
	      load_icon_data (min_dir, icon_file_path, icon_file_name);
	      
	      icon_info->data = icon_data_ref (g_hash_table_lookup (min_dir->icon_data, icon_name));
	    }
	  g_free (icon_file_name);
	  g_free (icon_file_path);
	}

      if (min_dir->cache)
	{
	  icon_info->cache_pixbuf = _gtk_icon_cache_get_icon (min_dir->cache, icon_name,
							      min_dir->subdir_index);
	}

      return icon_info;
    }

  if (closest_builtin)
    return icon_info_new_builtin (closest_builtin);
  
  return NULL;
}

static void
theme_list_icons (IconTheme  *theme, 
		  GHashTable *icons,
		  GQuark      context)
{
  GList *l = theme->dirs;
  IconThemeDir *dir;
  
  while (l != NULL)
    {
      dir = l->data;

      if (context == dir->context ||
	  context == 0)
	{
	  if (dir->cache)
	    {
	      _gtk_icon_cache_add_icons (dir->cache,
					 dir->subdir,
					 icons);
					 
	    }
	  else
	    {
	      g_hash_table_foreach (dir->icons,
				    add_key_to_hash,
				    icons);
	    }
	}
      l = l->next;
    }
}

static void
theme_list_contexts (IconTheme  *theme, 
		     GHashTable *contexts)
{
  GList *l = theme->dirs;
  IconThemeDir *dir;
  const char *context;

  while (l != NULL)
    {
      dir = l->data;

      context = g_quark_to_string (dir->context);
      g_hash_table_replace (contexts, (gpointer) context, NULL);

      l = l->next;
    }
}

static void
load_icon_data (IconThemeDir *dir, const char *path, const char *name)
{
  GKeyFile *icon_file;
  char *base_name;
  char **split;
  gsize length;
  char *str;
  char *split_point;
  int i;
  gint *ivalues;
  GError *error = NULL;
  GtkIconData *data;

  icon_file = g_key_file_new ();
  g_key_file_set_list_separator (icon_file, ',');
  g_key_file_load_from_file (icon_file, path, 0, &error);
  if (error)
    {
      g_error_free (error);
      g_key_file_free (icon_file);      
      return;
    }
  else
    {
      base_name = strip_suffix (name);

      data = g_slice_new0 (GtkIconData);
      data->ref = 1;

      /* takes ownership of base_name */
      g_hash_table_replace (dir->icon_data, base_name, data);
      
      ivalues = g_key_file_get_integer_list (icon_file, 
					     "Icon Data", "EmbeddedTextRectangle",
					      &length, NULL);
      if (ivalues)
	{
	  if (length == 4)
	    {
	      data->has_embedded_rect = TRUE;
	      data->x0 = ivalues[0];
	      data->y0 = ivalues[1];
	      data->x1 = ivalues[2];
	      data->y1 = ivalues[3];
	    }
	  
	  g_free (ivalues);
	}
      
      str = g_key_file_get_string (icon_file, "Icon Data", "AttachPoints", NULL);
      if (str)
	{
	  split = g_strsplit (str, "|", -1);
	  
	  data->n_attach_points = g_strv_length (split);
	  data->attach_points = g_new (GdkPoint, data->n_attach_points);

	  i = 0;
	  while (split[i] != NULL && i < data->n_attach_points)
	    {
	      split_point = strchr (split[i], ',');
	      if (split_point)
		{
		  *split_point = 0;
		  split_point++;
		  data->attach_points[i].x = atoi (split[i]);
		  data->attach_points[i].y = atoi (split_point);
		}
	      i++;
	    }
	  
	  g_strfreev (split);
	  g_free (str);
	}
      
      data->display_name = g_key_file_get_locale_string (icon_file, 
							 "Icon Data", "DisplayName",
							 NULL, NULL);
      g_key_file_free (icon_file);
    }
}

static void
scan_directory (GtkIconThemePrivate *icon_theme,
		IconThemeDir *dir, char *full_dir)
{
  GDir *gdir;
  const char *name;

  GTK_NOTE (ICONTHEME, 
	    g_print ("scanning directory %s\n", full_dir));
  dir->icons = g_hash_table_new_full (g_str_hash, g_str_equal,
				      g_free, NULL);
  
  gdir = g_dir_open (full_dir, 0, NULL);

  if (gdir == NULL)
    return;

  while ((name = g_dir_read_name (gdir)))
    {
      char *path;
      char *base_name;
      IconSuffix suffix, hash_suffix;

      if (g_str_has_suffix (name, ".icon"))
	{
	  if (dir->icon_data == NULL)
	    dir->icon_data = g_hash_table_new_full (g_str_hash, g_str_equal,
						    g_free, (GDestroyNotify)icon_data_unref);
	  
	  path = g_build_filename (full_dir, name, NULL);
	  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
	    load_icon_data (dir, path, name);
	  
	  g_free (path);
	  
	  continue;
	}

      suffix = suffix_from_name (name);
      if (suffix == ICON_SUFFIX_NONE)
	continue;

      base_name = strip_suffix (name);

      hash_suffix = GPOINTER_TO_INT (g_hash_table_lookup (dir->icons, base_name));
      g_hash_table_replace (icon_theme->all_icons, base_name, NULL);
      /* takes ownership of base_name */
      g_hash_table_replace (dir->icons, base_name, GUINT_TO_POINTER (hash_suffix| suffix));
    }
  
  g_dir_close (gdir);
}

static void
theme_subdir_load (GtkIconTheme *icon_theme,
		   IconTheme    *theme,
		   GKeyFile     *theme_file,
		   char         *subdir)
{
  GList *d;
  char *type_string;
  IconThemeDir *dir;
  IconThemeDirType type;
  char *context_string;
  GQuark context;
  int size;
  int min_size;
  int max_size;
  int threshold;
  char *full_dir;
  GError *error = NULL;
  IconThemeDirMtime *dir_mtime;
  int scale;

  size = g_key_file_get_integer (theme_file, subdir, "Size", &error);
  if (error)
    {
      g_error_free (error);
      g_warning ("Theme directory %s of theme %s has no size field\n", 
		 subdir, theme->name);
      return;
    }
  
  type = ICON_THEME_DIR_THRESHOLD;
  type_string = g_key_file_get_string (theme_file, subdir, "Type", NULL);
  if (type_string)
    {
      if (strcmp (type_string, "Fixed") == 0)
	type = ICON_THEME_DIR_FIXED;
      else if (strcmp (type_string, "Scalable") == 0)
	type = ICON_THEME_DIR_SCALABLE;
      else if (strcmp (type_string, "Threshold") == 0)
	type = ICON_THEME_DIR_THRESHOLD;

      g_free (type_string);
    }
  
  context = 0;
  context_string = g_key_file_get_string (theme_file, subdir, "Context", NULL);
  if (context_string)
    {
      context = g_quark_from_string (context_string);
      g_free (context_string);
    }

  if (g_key_file_has_key (theme_file, subdir, "MaxSize", NULL))
    max_size = g_key_file_get_integer (theme_file, subdir, "MaxSize", NULL);
  else
    max_size = size;

  if (g_key_file_has_key (theme_file, subdir, "MinSize", NULL))
    min_size = g_key_file_get_integer (theme_file, subdir, "MinSize", NULL);
  else
    min_size = size;

  if (g_key_file_has_key (theme_file, subdir, "Threshold", NULL))
    threshold = g_key_file_get_integer (theme_file, subdir, "Threshold", NULL);
  else
    threshold = 2;

  if (g_key_file_has_key (theme_file, subdir, "Scale", NULL))
    scale = g_key_file_get_integer (theme_file, subdir, "Scale", NULL);
  else
    scale = 1;

  for (d = icon_theme->priv->dir_mtimes; d; d = d->next)
    {
      dir_mtime = (IconThemeDirMtime *)d->data;

      if (dir_mtime->mtime == 0)
	continue; /* directory doesn't exist */

       full_dir = g_build_filename (dir_mtime->dir, subdir, NULL);

      /* First, see if we have a cache for the directory */
      if (dir_mtime->cache != NULL || g_file_test (full_dir, G_FILE_TEST_IS_DIR))
	{
	  if (dir_mtime->cache == NULL)
	    {
	      /* This will return NULL if the cache doesn't exist or is outdated */
	      dir_mtime->cache = _gtk_icon_cache_new_for_path (dir_mtime->dir);
	    }
	  
	  dir = g_new (IconThemeDir, 1);
	  dir->type = type;
	  dir->context = context;
	  dir->size = size;
	  dir->min_size = min_size;
	  dir->max_size = max_size;
	  dir->threshold = threshold;
	  dir->dir = full_dir;
	  dir->icon_data = NULL;
	  dir->subdir = g_strdup (subdir);
          dir->scale = scale;

	  if (dir_mtime->cache != NULL)
            {
	      dir->cache = _gtk_icon_cache_ref (dir_mtime->cache);
              dir->subdir_index = _gtk_icon_cache_get_directory_index (dir->cache, dir->subdir);
            }
	  else
	    {
	      dir->cache = NULL;
              dir->subdir_index = -1;
	      scan_directory (icon_theme->priv, dir, full_dir);
	    }

	  theme->dirs = g_list_prepend (theme->dirs, dir);
	}
      else
	g_free (full_dir);
    }
}

static GtkIconData *
icon_data_ref (GtkIconData *icon_data)
{
  if (icon_data)
    icon_data->ref++;
  return icon_data;
}

static void
icon_data_unref (GtkIconData *icon_data)
{
  if (icon_data)
    {
      icon_data->ref--;
      if (icon_data->ref == 0)
        {
          g_free (icon_data->attach_points);
          g_free (icon_data->display_name);
          g_slice_free (GtkIconData, icon_data);
        }
    }
}

static GtkIconData *
icon_data_dup (GtkIconData *icon_data)
{
  GtkIconData *dup = NULL;
  if (icon_data)
    {
      dup = g_slice_new0 (GtkIconData);
      dup->ref = 1;
      *dup = *icon_data;
      if (dup->n_attach_points > 0)
	{
	  dup->attach_points = g_memdup (dup->attach_points,
					 sizeof (GdkPoint) * dup->n_attach_points);
	}
      dup->display_name = g_strdup (dup->display_name);
    }

  return dup;
}


/*
 * GtkIconInfo
 */

static void gtk_icon_info_class_init (GtkIconInfoClass *klass);

G_DEFINE_TYPE (GtkIconInfo, gtk_icon_info, G_TYPE_OBJECT)

static void
gtk_icon_info_init (GtkIconInfo *icon_info)
{
  icon_info->scale = -1.;
}

static GtkIconInfo *
icon_info_new (IconThemeDirType type, int dir_size, int dir_scale)
{
  GtkIconInfo *icon_info;

  icon_info = g_object_new (GTK_TYPE_ICON_INFO, NULL);

  icon_info->dir_type = type;
  icon_info->dir_size = dir_size;
  icon_info->dir_scale = dir_scale;
  icon_info->unscaled_scale = 1.0;

  return icon_info;
}

/* This only copies whatever is needed to load the pixbuf, so that we can do
 * a load in a thread without affecting the original IconInfo from the thread.
 */
static GtkIconInfo *
icon_info_dup (GtkIconInfo *icon_info)
{
  GtkIconInfo *dup;
  GSList *l;

  dup = icon_info_new (icon_info->dir_type, icon_info->dir_size, icon_info->dir_scale);

  dup->filename = g_strdup (icon_info->filename);
  if (icon_info->icon_file)
    dup->icon_file = g_object_ref (icon_info->icon_file);
  if (icon_info->loadable)
    dup->loadable = g_object_ref (icon_info->loadable);
  if (icon_info->pixbuf)
    dup->pixbuf = g_object_ref (icon_info->pixbuf);

  for (l = icon_info->emblem_infos; l != NULL; l = l->next)
    {
      dup->emblem_infos =
	g_slist_append (dup->emblem_infos,
			icon_info_dup (l->data));
    }

  if (icon_info->cache_pixbuf)
    dup->cache_pixbuf = g_object_ref (icon_info->cache_pixbuf);

  dup->data = icon_data_dup (icon_info->data);
  dup->unscaled_scale = icon_info->unscaled_scale;
  dup->threshold = icon_info->threshold;
  dup->desired_size = icon_info->desired_size;
  dup->desired_scale = icon_info->desired_scale;
  dup->raw_coordinates = icon_info->raw_coordinates;
  dup->forced_size = icon_info->forced_size;
  dup->emblems_applied = icon_info->emblems_applied;

  return dup;
}

static GtkIconInfo *
icon_info_new_builtin (BuiltinIcon *icon)
{
  GtkIconInfo *icon_info = icon_info_new (ICON_THEME_DIR_THRESHOLD, icon->size, 1);

  icon_info->cache_pixbuf = g_object_ref (icon->pixbuf);
  icon_info->threshold = 2;

  return icon_info;
}

/**
 * gtk_icon_info_copy: (skip)
 * @icon_info: a #GtkIconInfo
 * 
 * Make a copy of a #GtkIconInfo.
 * 
 * Returns: the new GtkIconInfo
 *
 * Since: 2.4
 *
 * Deprecated: 3.8: Use g_object_ref()
 **/
GtkIconInfo *
gtk_icon_info_copy (GtkIconInfo *icon_info)
{
  
  g_return_val_if_fail (icon_info != NULL, NULL);

  return g_object_ref (icon_info);
}

/**
 * gtk_icon_info_free: (skip)
 * @icon_info: a #GtkIconInfo
 * 
 * Free a #GtkIconInfo and associated information
 *
 * Since: 2.4
 *
 * Deprecated: 3.8: Use g_object_unref()
 **/
void
gtk_icon_info_free (GtkIconInfo *icon_info)
{
  g_return_if_fail (icon_info != NULL);

  g_object_unref (icon_info);
}

static void
gtk_icon_info_finalize (GObject *object)
{
  GtkIconInfo *icon_info = (GtkIconInfo *) object;

  if (icon_info->in_cache)
    g_hash_table_remove (icon_info->in_cache->priv->info_cache, &icon_info->key);

  g_strfreev (icon_info->key.icon_names);

  g_free (icon_info->filename);
  g_clear_object (&icon_info->icon_file);

  g_clear_object (&icon_info->loadable);
  g_slist_free_full (icon_info->emblem_infos, (GDestroyNotify) g_object_unref);
  g_clear_object (&icon_info->pixbuf);
  g_clear_object (&icon_info->proxy_pixbuf);
  g_clear_object (&icon_info->cache_pixbuf);
  g_clear_error (&icon_info->load_error);
  g_clear_pointer (&icon_info->symbolic_pixbuf_size, gtk_requisition_free);
  icon_data_unref (icon_info->data);

  symbolic_pixbuf_cache_free (icon_info->symbolic_pixbuf_cache);

  G_OBJECT_CLASS (gtk_icon_info_parent_class)->finalize (object);
}

static void
gtk_icon_info_class_init (GtkIconInfoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gtk_icon_info_finalize;
}

/**
 * gtk_icon_info_get_base_size:
 * @icon_info: a #GtkIconInfo
 * 
 * Gets the base size for the icon. The base size
 * is a size for the icon that was specified by
 * the icon theme creator. This may be different
 * than the actual size of image; an example of
 * this is small emblem icons that can be attached
 * to a larger icon. These icons will be given
 * the same base size as the larger icons to which
 * they are attached.
 *
 * Note that for scaled icons the base size does
 * not include the base scale.
 *
 * Returns: the base size, or 0, if no base
 *  size is known for the icon.
 *
 * Since: 2.4
 **/
gint
gtk_icon_info_get_base_size (GtkIconInfo *icon_info)
{
  g_return_val_if_fail (icon_info != NULL, 0);

  return icon_info->dir_size;
}

/**
 * gtk_icon_info_get_base_scale:
 * @icon_info: a #GtkIconInfo
 *
 * Gets the base scale for the icon. The base scale is a scale for the
 * icon that was specified by the icon theme creator. For instance an
 * icon drawn for a high-dpi screen with window-scale 2 for a base
 * size of 32 will be 64 pixels tall and have a base_scale of 2.
 *
 * Returns: the base scale.
 *
 * Since: 3.10
 **/
gint
gtk_icon_info_get_base_scale (GtkIconInfo *icon_info)
{
  g_return_val_if_fail (icon_info != NULL, 0);

  return icon_info->dir_scale;
}

/**
 * gtk_icon_info_get_filename:
 * @icon_info: a #GtkIconInfo
 * 
 * Gets the filename for the icon. If the
 * %GTK_ICON_LOOKUP_USE_BUILTIN flag was passed
 * to gtk_icon_theme_lookup_icon(), there may be
 * no filename if a builtin icon is returned; in this
 * case, you should use gtk_icon_info_get_builtin_pixbuf().
 * 
 * Returns: (type filename): the filename for the icon, or %NULL
 *  if gtk_icon_info_get_builtin_pixbuf() should be used instead. The
 *  return value is owned by GTK+ and should not be modified or freed.
 *
 * Since: 2.4
 **/
const gchar *
gtk_icon_info_get_filename (GtkIconInfo *icon_info)
{
  g_return_val_if_fail (icon_info != NULL, NULL);

  return icon_info->filename;
}

/**
 * gtk_icon_info_get_builtin_pixbuf:
 * @icon_info: a #GtkIconInfo
 * 
 * Gets the built-in image for this icon, if any. To allow
 * GTK+ to use built in icon images, you must pass the
 * %GTK_ICON_LOOKUP_USE_BUILTIN to
 * gtk_icon_theme_lookup_icon().
 *
 * Returns: (transfer none): the built-in image pixbuf, or %NULL. No
 *  extra reference is added to the returned pixbuf, so if
 *  you want to keep it around, you must use g_object_ref().
 *  The returned image must not be modified.
 *
 * Since: 2.4
 **/
GdkPixbuf *
gtk_icon_info_get_builtin_pixbuf (GtkIconInfo *icon_info)
{
  g_return_val_if_fail (icon_info != NULL, NULL);

  if (icon_info->filename)
    return NULL;
  
  return icon_info->cache_pixbuf;
}

/**
 * gtk_icon_info_is_symbolic:
 * @icon_info: a #GtkIconInfo
 *
 * Checks if the icon is symbolic or not. This currently uses only
 * the file name and not the file contents for determining this.
 * This behaviour may change in the future.
 *
 * Returns: %TRUE if the icon is symbolic, %FALSE otherwise.
 *
 * Since: 3.12
 **/
gboolean
gtk_icon_info_is_symbolic (GtkIconInfo *icon_info)
{
  gchar *icon_uri;
  gboolean is_symbolic;

  g_return_val_if_fail (GTK_IS_ICON_INFO (icon_info), FALSE);

  icon_uri = NULL;
  if (icon_info->icon_file)
    icon_uri = g_file_get_uri (icon_info->icon_file);

  is_symbolic = (icon_uri != NULL) && (g_str_has_suffix (icon_uri, "-symbolic.svg"));
  g_free (icon_uri);

  return is_symbolic;
}

static gboolean icon_info_ensure_scale_and_pixbuf (GtkIconInfo*, gboolean);

static GdkPixbuf *
apply_emblems_to_pixbuf (GdkPixbuf *pixbuf,
                         GtkIconInfo *info)
{
  GdkPixbuf *icon = NULL;
  gint w, h, pos;
  GSList *l;

  if (info->emblem_infos == NULL)
    return NULL;

  w = gdk_pixbuf_get_width (pixbuf);
  h = gdk_pixbuf_get_height (pixbuf);

  for (l = info->emblem_infos, pos = 0; l; l = l->next, pos++)
    {
      GtkIconInfo *emblem_info = l->data;

      if (icon_info_ensure_scale_and_pixbuf (emblem_info, FALSE))
        {
          GdkPixbuf *emblem = emblem_info->pixbuf;
          gint ew, eh;
          gint x = 0, y = 0; /* silence compiler */
          gdouble scale;

          ew = gdk_pixbuf_get_width (emblem);
          eh = gdk_pixbuf_get_height (emblem);
          if (ew >= w)
            {
              scale = 0.75;
              ew = ew * 0.75;
              eh = eh * 0.75;
            }
          else
            scale = 1.0;

          switch (pos % 4)
            {
            case 0:
              x = w - ew;
              y = h - eh;
              break;
            case 1:
              x = w - ew;
              y = 0;
              break;
            case 2:
              x = 0;
              y = h - eh;
              break;
            case 3:
              x = 0;
              y = 0;
              break;
            }

          if (icon == NULL)
            {
              icon = gdk_pixbuf_copy (pixbuf);
              if (icon == NULL)
                break;
            }

          gdk_pixbuf_composite (emblem, icon, x, y, ew, eh, x, y,
                                scale, scale, GDK_INTERP_BILINEAR, 255);
       }
   }

  return icon;
}

/* Combine the icon with all emblems, the first emblem is placed 
 * in the southeast corner. Scale emblems to be at most 3/4 of the
 * size of the icon itself.
 */
static void 
apply_emblems (GtkIconInfo *info)
{
  GdkPixbuf *icon;

  if (info->emblems_applied)
    return;

  icon = apply_emblems_to_pixbuf (info->pixbuf, info);

  if (icon)
    {
      g_object_unref (info->pixbuf);
      info->pixbuf = icon;
      info->emblems_applied = TRUE;
    }
}

/* If this returns TRUE, its safe to call
   icon_info_ensure_scale_and_pixbuf without blocking */
static gboolean
icon_info_get_pixbuf_ready (GtkIconInfo *icon_info)
{
  if (icon_info->pixbuf &&
      (icon_info->emblem_infos == NULL || icon_info->emblems_applied))
    return TRUE;

  if (icon_info->load_error)
    return TRUE;

  return FALSE;
}

/* This function contains the complicated logic for deciding
 * on the size at which to load the icon and loading it at
 * that size.
 */
static gboolean
icon_info_ensure_scale_and_pixbuf (GtkIconInfo  *icon_info,
				   gboolean      scale_only)
{
  int image_width, image_height;
  int scaled_desired_size;
  GdkPixbuf *source_pixbuf;
  gboolean is_svg;

  /* First check if we already succeeded have the necessary
   * information (or failed earlier)
   */
  if (scale_only && icon_info->scale >= 0)
    return TRUE;

  if (icon_info->pixbuf)
    {
      apply_emblems (icon_info);
      return TRUE;
    }

  if (icon_info->load_error)
    return FALSE;

  /* SVG icons are a special case - we just immediately scale them
   * to the desired size
   */
  if (icon_info->icon_file && !icon_info->loadable)
    icon_info->loadable = G_LOADABLE_ICON (g_file_icon_new (icon_info->icon_file));

  scaled_desired_size = icon_info->desired_size * icon_info->desired_scale;

  is_svg = FALSE;
  if (G_IS_FILE_ICON (icon_info->loadable))
    {
      GFile *file;
      GFileInfo *file_info;
      const gchar *content_type;

      file = g_file_icon_get_file (G_FILE_ICON (icon_info->loadable));
      file_info = g_file_query_info (file, 
                                     G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                     G_FILE_QUERY_INFO_NONE,
                                     NULL, NULL);
      if (file_info) 
        {
          content_type = g_file_info_get_content_type (file_info);

          if (content_type && strcmp (content_type, "image/svg+xml") == 0)
            is_svg = TRUE;

          g_object_unref (file_info);
       }
    }

  if (is_svg)
    {
      GInputStream *stream;

      icon_info->scale = scaled_desired_size / 1000.;

      if (scale_only)
	return TRUE;

      /* TODO: We should have a load_at_scale */
      stream = g_loadable_icon_load (icon_info->loadable,
                                     scaled_desired_size,
                                     NULL, NULL,
                                     &icon_info->load_error);
      if (stream)
        {
          icon_info->pixbuf = gdk_pixbuf_new_from_stream_at_scale (stream,
                                                                   scaled_desired_size,
                                                                   scaled_desired_size,
                                                                   TRUE,
                                                                   NULL,
                                                                   &icon_info->load_error);
          g_object_unref (stream);
        }

      if (!icon_info->pixbuf)
        return FALSE;

      apply_emblems (icon_info);
        
      return TRUE;
    }

  /* In many cases, the scale can be determined without actual access
   * to the icon file. This is generally true when we have a size
   * for the directory where the icon is; the image size doesn't
   * matter in that case.
   */
  if (icon_info->forced_size)
    icon_info->scale = -1;
  else if (icon_info->dir_type == ICON_THEME_DIR_FIXED)
    icon_info->scale = icon_info->unscaled_scale;
  else if (icon_info->dir_type == ICON_THEME_DIR_THRESHOLD)
    {
      if (scaled_desired_size  >= (icon_info->dir_size - icon_info->threshold) * icon_info->dir_scale &&
	  scaled_desired_size <= (icon_info->dir_size + icon_info->threshold) * icon_info->dir_scale)
        icon_info->scale = icon_info->unscaled_scale;
      else if (icon_info->dir_size > 0)
	icon_info->scale =(gdouble) scaled_desired_size / (icon_info->dir_size * icon_info->dir_scale);
    }
  else if (icon_info->dir_type == ICON_THEME_DIR_SCALABLE)
    {
      if (icon_info->dir_size > 0)
	icon_info->scale = (gdouble) scaled_desired_size / (icon_info->dir_size * icon_info->dir_scale);
    }

  if (icon_info->scale >= 0. && scale_only)
    return TRUE;

  /* At this point, we need to actually get the icon; either from the
   * builtin image or by loading the file
   */
  source_pixbuf = NULL;
  if (icon_info->cache_pixbuf)
    source_pixbuf = g_object_ref (icon_info->cache_pixbuf);
  else
    {
      GInputStream *stream;

      /* TODO: We should have a load_at_scale */
      stream = g_loadable_icon_load (icon_info->loadable,
                                     scaled_desired_size,
                                     NULL, NULL,
                                     &icon_info->load_error);
      if (stream)
        {
          source_pixbuf = gdk_pixbuf_new_from_stream (stream,
                                                      NULL,
                                                      &icon_info->load_error);
          g_object_unref (stream);
        }
    }

  if (!source_pixbuf)
    return FALSE;

  /* Do scale calculations that depend on the image size
   */
  image_width = gdk_pixbuf_get_width (source_pixbuf);
  image_height = gdk_pixbuf_get_height (source_pixbuf);

  if (icon_info->scale < 0.0)
    {
      gint image_size = MAX (image_width, image_height);
      if (image_size > 0)
	icon_info->scale = (gdouble)scaled_desired_size / (gdouble)image_size;
      else
	icon_info->scale = 1.0;
      
      if (icon_info->dir_type == ICON_THEME_DIR_UNTHEMED && 
          !icon_info->forced_size)
	icon_info->scale = MIN (icon_info->scale, 1.0);
    }

  /* We don't short-circuit out here for scale_only, since, now
   * we've loaded the icon, we might as well go ahead and finish
   * the job. This is a bit of a waste when we scale here
   * and never get the final pixbuf; at the cost of a bit of
   * extra complexity, we could keep the source pixbuf around
   * but not actually scale it until needed.
   */
  if (icon_info->scale == 1.0)
    icon_info->pixbuf = source_pixbuf;
  else
    {
      icon_info->pixbuf = gdk_pixbuf_scale_simple (source_pixbuf,
						   0.5 + image_width * icon_info->scale,
						   0.5 + image_height * icon_info->scale,
						   GDK_INTERP_BILINEAR);
      g_object_unref (source_pixbuf);
    }

  apply_emblems (icon_info);

  return TRUE;
}

static void
proxy_pixbuf_destroy (guchar *pixels, gpointer data)
{
  GtkIconInfo *icon_info = data;
  GtkIconTheme *icon_theme = icon_info->in_cache;

  g_assert (icon_info->proxy_pixbuf != NULL);
  icon_info->proxy_pixbuf = NULL;

  /* Keep it alive a bit longer */
  if (icon_theme != NULL)
    ensure_in_lru_cache (icon_theme, icon_info);

  g_object_unref (icon_info);
}

/**
 * gtk_icon_info_load_icon:
 * @icon_info: a #GtkIconInfo from gtk_icon_theme_lookup_icon()
 * @error: (allow-none): location to store error information on failure,
 *     or %NULL.
 *
 * Renders an icon previously looked up in an icon theme using
 * gtk_icon_theme_lookup_icon(); the size will be based on the size
 * passed to gtk_icon_theme_lookup_icon(). Note that the resulting
 * pixbuf may not be exactly this size; an icon theme may have icons
 * that differ slightly from their nominal sizes, and in addition GTK+
 * will avoid scaling icons that it considers sufficiently close to the
 * requested size or for which the source image would have to be scaled
 * up too far. (This maintains sharpness.). This behaviour can be changed
 * by passing the %GTK_ICON_LOOKUP_FORCE_SIZE flag when obtaining
 * the #GtkIconInfo. If this flag has been specified, the pixbuf
 * returned by this function will be scaled to the exact size.
 *
 * Returns: (transfer full): the rendered icon; this may be a newly
 *     created icon or a new reference to an internal icon, so you must
 *     not modify the icon. Use g_object_unref() to release your reference
 *     to the icon.
 *
 * Since: 2.4
 **/
GdkPixbuf *
gtk_icon_info_load_icon (GtkIconInfo *icon_info,
			 GError     **error)
{
  g_return_val_if_fail (icon_info != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!icon_info_ensure_scale_and_pixbuf (icon_info, FALSE))
    {
      if (icon_info->load_error)
        {
          if (error)
            *error = g_error_copy (icon_info->load_error);
        }
      else
        {
          g_set_error_literal (error,  
                               GTK_ICON_THEME_ERROR,  
                               GTK_ICON_THEME_NOT_FOUND,
                               _("Failed to load icon"));
        }
 
      return NULL;
    }

  /* Instead of returning the pixbuf directly we
     return a proxy to it that we don't own (but that
     shares the data with the one we own). This way
     we can know when it is freed and ensure the
     IconInfo is alive (and thus cached) while
     the pixbuf is still alive. */

  if (icon_info->proxy_pixbuf != NULL)
    return g_object_ref (icon_info->proxy_pixbuf);

  icon_info->proxy_pixbuf =
    gdk_pixbuf_new_from_data (gdk_pixbuf_get_pixels (icon_info->pixbuf),
			      gdk_pixbuf_get_colorspace (icon_info->pixbuf),
			      gdk_pixbuf_get_has_alpha (icon_info->pixbuf),
			      gdk_pixbuf_get_bits_per_sample (icon_info->pixbuf),
			      gdk_pixbuf_get_width (icon_info->pixbuf),
			      gdk_pixbuf_get_height (icon_info->pixbuf),
			      gdk_pixbuf_get_rowstride (icon_info->pixbuf),
			      proxy_pixbuf_destroy,
			      g_object_ref (icon_info));

  return icon_info->proxy_pixbuf;
}

/**
 * gtk_icon_info_load_surface:
 * @icon_info: a #GtkIconInfo from gtk_icon_theme_lookup_icon()
 * @for_window: (allow-none): #GdkWindow to optimize drawing for, or %NULL
 * @error: (allow-none): location to store error information on failure,
 *     or %NULL.
 *
 * Renders an icon previously looked up in an icon theme using
 * gtk_icon_theme_lookup_icon(); the size will be based on the size
 * passed to gtk_icon_theme_lookup_icon(). Note that the resulting
 * surface may not be exactly this size; an icon theme may have icons
 * that differ slightly from their nominal sizes, and in addition GTK+
 * will avoid scaling icons that it considers sufficiently close to the
 * requested size or for which the source image would have to be scaled
 * up too far. (This maintains sharpness.). This behaviour can be changed
 * by passing the %GTK_ICON_LOOKUP_FORCE_SIZE flag when obtaining
 * the #GtkIconInfo. If this flag has been specified, the pixbuf
 * returned by this function will be scaled to the exact size.
 *
 * Returns: (transfer full): the rendered icon; this may be a newly
 *     created icon or a new reference to an internal icon, so you must
 *     not modify the icon. Use cairo_surface_destroy() to release your reference
 *     to the icon.
 *
 * Since: 3.10
 **/
cairo_surface_t *
gtk_icon_info_load_surface (GtkIconInfo *icon_info,
			    GdkWindow *for_window,
			    GError     **error)
{
  GdkPixbuf *pixbuf;
  cairo_surface_t *surface;

  g_return_val_if_fail (icon_info != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  pixbuf = gtk_icon_info_load_icon (icon_info, error);

  if (pixbuf == NULL)
    return NULL;

  surface = gdk_cairo_surface_create_from_pixbuf (pixbuf, icon_info->desired_scale, for_window);
  g_object_unref (pixbuf);

  return surface;
}

static void
load_icon_thread  (GTask           *task,
		   gpointer         source_object,
		   gpointer         task_data,
		   GCancellable    *cancellable)
{
  GtkIconInfo *dup = task_data;

  icon_info_ensure_scale_and_pixbuf (dup, FALSE);
  g_task_return_pointer (task, NULL, NULL);
}

/**
 * gtk_icon_info_load_icon_async:
 * @icon_info: a #GtkIconInfo from gtk_icon_theme_lookup_icon()
 * @cancellable: (allow-none): optional #GCancellable object,
 *     %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the
 *     request is satisfied
 * @user_data: (closure): the data to pass to callback function
 *
 * Asynchronously load, render and scale an icon previously looked up
 * from the icon theme using gtk_icon_theme_lookup_icon().
 *
 * For more details, see gtk_icon_info_load_icon() which is the synchronous
 * version of this call.
 *
 * Since: 3.8
 **/
void
gtk_icon_info_load_icon_async (GtkIconInfo          *icon_info,
			       GCancellable         *cancellable,
			       GAsyncReadyCallback   callback,
			       gpointer              user_data)
{
  GTask *task;
  GdkPixbuf *pixbuf;
  GtkIconInfo *dup;
  GError *error = NULL;

  task = g_task_new (icon_info, cancellable, callback, user_data);

  if (icon_info_get_pixbuf_ready (icon_info))
    {
      pixbuf = gtk_icon_info_load_icon (icon_info, &error);
      if (pixbuf == NULL)
	g_task_return_error (task, error);
      else
	g_task_return_pointer (task, pixbuf, g_object_unref);
      g_object_unref (task);
    }
  else
    {
      dup = icon_info_dup (icon_info);
      g_task_set_task_data (task, dup, g_object_unref);
      g_task_run_in_thread (task, load_icon_thread);
      g_object_unref (task);
    }
}

/**
 * gtk_icon_info_load_icon_finish:
 * @icon_info: a #GtkIconInfo from gtk_icon_theme_lookup_icon()
 * @res: a #GAsyncResult
 * @error: (allow-none): location to store error information on failure,
 *     or %NULL.
 *
 * Finishes an async icon load, see gtk_icon_info_load_icon_async().
 *
 * Returns: (transfer full): the rendered icon; this may be a newly
 *     created icon or a new reference to an internal icon, so you must
 *     not modify the icon. Use g_object_unref() to release your reference
 *     to the icon.
 *
 * Since: 3.8
 **/
GdkPixbuf *
gtk_icon_info_load_icon_finish (GtkIconInfo          *icon_info,
				GAsyncResult         *result,
				GError              **error)
{
  GTask *task = G_TASK (result);
  GtkIconInfo *dup;

  g_return_val_if_fail (g_task_is_valid (result, icon_info), NULL);

  dup = g_task_get_task_data (task);
  if (dup == NULL || g_task_had_error (task))
    return g_task_propagate_pointer (task, error);

  /* We ran the thread and it was not cancelled */

  /* Check if someone else updated the icon_info in between */
  if (!icon_info_get_pixbuf_ready (icon_info))
    {
      /* If not, copy results from dup back to icon_info */

      icon_info->emblems_applied = dup->emblems_applied;
      icon_info->scale = dup->scale;
      g_clear_object (&icon_info->pixbuf);
      if (dup->pixbuf)
	icon_info->pixbuf = g_object_ref (dup->pixbuf);
      g_clear_error (&icon_info->load_error);
      if (dup->load_error)
	icon_info->load_error = g_error_copy (dup->load_error);
    }

  g_assert (icon_info_get_pixbuf_ready (icon_info));

  /* This is now guaranteed to not block */
  return gtk_icon_info_load_icon (icon_info, error);
}

static gchar *
gdk_color_to_css (GdkColor *color)
{
  return g_strdup_printf ("rgb(%d,%d,%d)",
                          color->red >> 8,
                          color->green >> 8,
                          color->blue >> 8);
}

static void
proxy_symbolic_pixbuf_destroy (guchar *pixels, gpointer data)
{
  GtkIconInfo *icon_info = data;
  GtkIconTheme *icon_theme = icon_info->in_cache;
  SymbolicPixbufCache *symbolic_cache;

  for (symbolic_cache = icon_info->symbolic_pixbuf_cache;
       symbolic_cache != NULL;
       symbolic_cache = symbolic_cache->next)
    {
      if (symbolic_cache->proxy_pixbuf != NULL &&
	  gdk_pixbuf_get_pixels (symbolic_cache->proxy_pixbuf) == pixels)
	break;
    }

  g_assert (symbolic_cache != NULL);
  g_assert (symbolic_cache->proxy_pixbuf != NULL);

  symbolic_cache->proxy_pixbuf = NULL;

  /* Keep it alive a bit longer */
  if (icon_theme != NULL)
    ensure_in_lru_cache (icon_theme, icon_info);

  g_object_unref (icon_info);
}

static GdkPixbuf *
symbolic_cache_get_proxy (SymbolicPixbufCache *symbolic_cache,
			  GtkIconInfo  *icon_info)
{
  if (symbolic_cache->proxy_pixbuf)
    return g_object_ref (symbolic_cache->proxy_pixbuf);

  symbolic_cache->proxy_pixbuf =
    gdk_pixbuf_new_from_data (gdk_pixbuf_get_pixels (symbolic_cache->pixbuf),
			      gdk_pixbuf_get_colorspace (symbolic_cache->pixbuf),
			      gdk_pixbuf_get_has_alpha (symbolic_cache->pixbuf),
			      gdk_pixbuf_get_bits_per_sample (symbolic_cache->pixbuf),
			      gdk_pixbuf_get_width (symbolic_cache->pixbuf),
			      gdk_pixbuf_get_height (symbolic_cache->pixbuf),
			      gdk_pixbuf_get_rowstride (symbolic_cache->pixbuf),
			      proxy_symbolic_pixbuf_destroy,
			      g_object_ref (icon_info));

  return symbolic_cache->proxy_pixbuf;
}

static GdkPixbuf *
_gtk_icon_info_load_symbolic_internal (GtkIconInfo    *icon_info,
				       const GdkRGBA  *fg,
				       const GdkRGBA  *success_color,
				       const GdkRGBA  *warning_color,
				       const GdkRGBA  *error_color,
				       gboolean        use_cache,
                                       GError        **error)
{
  GInputStream *stream;
  GdkPixbuf *pixbuf;
  gchar *css_fg;
  gchar *css_success;
  gchar *css_warning;
  gchar *css_error;
  gchar *data;
  gchar *width, *height;
  gchar *file_data, *escaped_file_data;
  gsize file_len;
  SymbolicPixbufCache *symbolic_cache;

  if (use_cache)
    {
      symbolic_cache = symbolic_pixbuf_cache_matches (icon_info->symbolic_pixbuf_cache,
						      fg, success_color, warning_color, error_color);
      if (symbolic_cache)
	return symbolic_cache_get_proxy (symbolic_cache, icon_info);
    }

  /* css_fg can't possibly have failed, otherwise
   * that would mean we have a broken style */
  g_return_val_if_fail (fg != NULL, NULL);

  css_fg = gdk_rgba_to_string (fg);

  css_success = css_warning = css_error = NULL;

  if (warning_color)
    css_warning = gdk_rgba_to_string (warning_color);

  if (error_color)
    css_error = gdk_rgba_to_string (error_color);

  if (success_color)
    css_success = gdk_rgba_to_string (success_color);

  if (!css_success)
    {
      GdkColor success_default_color = { 0, 0x4e00, 0x9a00, 0x0600 };
      css_success = gdk_color_to_css (&success_default_color);
    }
  if (!css_warning)
    {
      GdkColor warning_default_color = { 0, 0xf500, 0x7900, 0x3e00 };
      css_warning = gdk_color_to_css (&warning_default_color);
    }
  if (!css_error)
    {
      GdkColor error_default_color = { 0, 0xcc00, 0x0000, 0x0000 };
      css_error = gdk_color_to_css (&error_default_color);
    }

  if (!g_file_load_contents (icon_info->icon_file, NULL, &file_data, &file_len, NULL, error))
    return NULL;

  if (!icon_info->symbolic_pixbuf_size)
    {
      /* Fetch size from the original icon */
      stream = g_memory_input_stream_new_from_data (file_data, file_len, NULL);
      pixbuf = gdk_pixbuf_new_from_stream (stream, NULL, error);
      g_object_unref (stream);

      if (!pixbuf)
        return NULL;

      icon_info->symbolic_pixbuf_size = gtk_requisition_new ();
      icon_info->symbolic_pixbuf_size->width = gdk_pixbuf_get_width (pixbuf);
      icon_info->symbolic_pixbuf_size->height = gdk_pixbuf_get_height (pixbuf);
      g_object_unref (pixbuf);
    }

  width = g_strdup_printf ("%d", icon_info->symbolic_pixbuf_size->width);
  height = g_strdup_printf ("%d", icon_info->symbolic_pixbuf_size->height);

  escaped_file_data = g_markup_escape_text (file_data, file_len);
  g_free (file_data);

  data = g_strconcat ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
                      "<svg version=\"1.1\"\n"
                      "     xmlns=\"http://www.w3.org/2000/svg\"\n"
                      "     xmlns:xi=\"http://www.w3.org/2001/XInclude\"\n"
                      "     width=\"", width, "\"\n"
                      "     height=\"", height, "\">\n"
                      "  <style type=\"text/css\">\n"
                      "    rect,path {\n"
                      "      fill: ", css_fg," !important;\n"
                      "    }\n"
                      "    .warning {\n"
                      "      fill: ", css_warning, " !important;\n"
                      "    }\n"
                      "    .error {\n"
                      "      fill: ", css_error ," !important;\n"
                      "    }\n"
                      "    .success {\n"
                      "      fill: ", css_success, " !important;\n"
                      "    }\n"
                      "  </style>\n"
                      "  <xi:include href=\"data:text/xml,", escaped_file_data, "\"/>\n"
                      "</svg>",
                      NULL);
  g_free (escaped_file_data);
  g_free (css_fg);
  g_free (css_warning);
  g_free (css_error);
  g_free (css_success);
  g_free (width);
  g_free (height);

  stream = g_memory_input_stream_new_from_data (data, -1, g_free);
  pixbuf = gdk_pixbuf_new_from_stream_at_scale (stream,
                                                icon_info->desired_size * icon_info->desired_scale,
                                                icon_info->desired_size * icon_info->desired_scale,
                                                TRUE,
                                                NULL,
                                                error);
  g_object_unref (stream);

  if (pixbuf != NULL)
    {
      GdkPixbuf *icon;

      icon = apply_emblems_to_pixbuf (pixbuf, icon_info);
      if (icon != NULL)
        {
          g_object_unref (pixbuf);
          pixbuf = icon;
        }

      if (use_cache)
	{
	  icon_info->symbolic_pixbuf_cache =
	    symbolic_pixbuf_cache_new (pixbuf, fg, success_color, warning_color, error_color,
				       icon_info->symbolic_pixbuf_cache);
	  g_object_unref (pixbuf);
	  return symbolic_cache_get_proxy (icon_info->symbolic_pixbuf_cache, icon_info);
	}
      else
	return pixbuf;
    }

  return NULL;
}

/**
 * gtk_icon_info_load_symbolic:
 * @icon_info: a #GtkIconInfo
 * @fg: a #GdkRGBA representing the foreground color of the icon
 * @success_color: (allow-none): a #GdkRGBA representing the warning color
 *     of the icon or %NULL to use the default color
 * @warning_color: (allow-none): a #GdkRGBA representing the warning color
 *     of the icon or %NULL to use the default color
 * @error_color: (allow-none): a #GdkRGBA representing the error color
 *     of the icon or %NULL to use the default color (allow-none)
 * @was_symbolic: (out) (allow-none): a #gboolean, returns whether the
 *     loaded icon was a symbolic one and whether the @fg color was
 *     applied to it.
 * @error: (allow-none): location to store error information on failure,
 *     or %NULL.
 *
 * Loads an icon, modifying it to match the system colours for the foreground,
 * success, warning and error colors provided. If the icon is not a symbolic
 * one, the function will return the result from gtk_icon_info_load_icon().
 *
 * This allows loading symbolic icons that will match the system theme.
 *
 * Unless you are implementing a widget, you will want to use
 * g_themed_icon_new_with_default_fallbacks() to load the icon.
 *
 * As implementation details, the icon loaded needs to be of SVG type,
 * contain the “symbolic” term as the last component of the icon name,
 * and use the “fg”, “success”, “warning” and “error” CSS styles in the
 * SVG file itself.
 *
 * See the [Symbolic Icons Specification](http://www.freedesktop.org/wiki/SymbolicIcons)
 * for more information about symbolic icons.
 *
 * Returns: (transfer full): a #GdkPixbuf representing the loaded icon
 *
 * Since: 3.0
 **/
GdkPixbuf *
gtk_icon_info_load_symbolic (GtkIconInfo    *icon_info,
                             const GdkRGBA  *fg,
                             const GdkRGBA  *success_color,
                             const GdkRGBA  *warning_color,
                             const GdkRGBA  *error_color,
                             gboolean       *was_symbolic,
                             GError        **error)
{
  gboolean is_symbolic;

  g_return_val_if_fail (icon_info != NULL, NULL);
  g_return_val_if_fail (fg != NULL, NULL);

  is_symbolic = gtk_icon_info_is_symbolic (icon_info);

  if (was_symbolic)
    *was_symbolic = is_symbolic;

  if (!is_symbolic)
    return gtk_icon_info_load_icon (icon_info, error);

  return _gtk_icon_info_load_symbolic_internal (icon_info,
						fg, success_color,
						warning_color, error_color,
						TRUE,
						error);
}

/**
 * gtk_icon_info_load_symbolic_for_context:
 * @icon_info: a #GtkIconInfo
 * @context: a #GtkStyleContext
 * @was_symbolic: (out) (allow-none): a #gboolean, returns whether the
 *     loaded icon was a symbolic one and whether the @fg color was
 *     applied to it.
 * @error: (allow-none): location to store error information on failure,
 *     or %NULL.
 *
 * Loads an icon, modifying it to match the system colors for the foreground,
 * success, warning and error colors provided. If the icon is not a symbolic
 * one, the function will return the result from gtk_icon_info_load_icon().
 * This function uses the regular foreground color and the symbolic colors
 * with the names “success_color”, “warning_color” and “error_color” from
 * the context.
 *
 * This allows loading symbolic icons that will match the system theme.
 *
 * See gtk_icon_info_load_symbolic() for more details.
 *
 * Returns: (transfer full): a #GdkPixbuf representing the loaded icon
 *
 * Since: 3.0
 **/
GdkPixbuf *
gtk_icon_info_load_symbolic_for_context (GtkIconInfo      *icon_info,
                                         GtkStyleContext  *context,
                                         gboolean         *was_symbolic,
                                         GError          **error)
{
  GdkRGBA *color = NULL;
  GdkRGBA fg;
  GdkRGBA *fgp;
  GdkRGBA success_color;
  GdkRGBA *success_colorp;
  GdkRGBA warning_color;
  GdkRGBA *warning_colorp;
  GdkRGBA error_color;
  GdkRGBA *error_colorp;
  GtkStateFlags state;
  gboolean is_symbolic;

  g_return_val_if_fail (icon_info != NULL, NULL);
  g_return_val_if_fail (context != NULL, NULL);

  is_symbolic = gtk_icon_info_is_symbolic (icon_info);

  if (was_symbolic)
    *was_symbolic = is_symbolic;

  if (!is_symbolic)
    return gtk_icon_info_load_icon (icon_info, error);

  fgp = success_colorp = warning_colorp = error_colorp = NULL;

  state = gtk_style_context_get_state (context);
  gtk_style_context_get (context, state, "color", &color, NULL);
  if (color)
    {
      fg = *color;
      fgp = &fg;
      gdk_rgba_free (color);
    }

  if (gtk_style_context_lookup_color (context, "success_color", &success_color))
    success_colorp = &success_color;

  if (gtk_style_context_lookup_color (context, "warning_color", &warning_color))
    warning_colorp = &warning_color;

  if (gtk_style_context_lookup_color (context, "error_color", &error_color))
    error_colorp = &error_color;

  return _gtk_icon_info_load_symbolic_internal (icon_info,
						fgp, success_colorp,
						warning_colorp, error_colorp,
						TRUE,
						error);
}

typedef struct {
  gboolean is_symbolic;
  GtkIconInfo *dup;
  GdkRGBA fg;
  gboolean fg_set;
  GdkRGBA success_color;
  gboolean success_color_set;
  GdkRGBA warning_color;
  gboolean warning_color_set;
  GdkRGBA error_color;
  gboolean error_color_set;
} AsyncSymbolicData;

static void
async_symbolic_data_free (AsyncSymbolicData *data)
{
  if (data->dup)
    g_object_unref (data->dup);
  g_slice_free (AsyncSymbolicData, data);
}

static void
async_load_no_symbolic_cb (GObject *source_object,
			   GAsyncResult *res,
			   gpointer user_data)
{
  GtkIconInfo *icon_info = GTK_ICON_INFO (source_object);
  GTask *task = user_data;
  GError *error = NULL;
  GdkPixbuf *pixbuf;

  pixbuf = gtk_icon_info_load_icon_finish (icon_info, res, &error);
  if (pixbuf == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, pixbuf, g_object_unref);
  g_object_unref (task);
}

static void
load_symbolic_icon_thread  (GTask           *task,
			    gpointer         source_object,
			    gpointer         task_data,
			    GCancellable    *cancellable)
{
  AsyncSymbolicData *data = task_data;
  GError *error;
  GdkPixbuf *pixbuf;

  error = NULL;
  pixbuf =
    _gtk_icon_info_load_symbolic_internal (data->dup,
					   data->fg_set ? &data->fg : NULL,
					   data->success_color_set ? &data->success_color : NULL,
					   data->warning_color_set ? &data->warning_color : NULL,
					   data->error_color_set ? &data->error_color : NULL,
					   FALSE,
					   &error);
  if (pixbuf == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, pixbuf, g_object_unref);
}

/**
 * gtk_icon_info_load_symbolic_async:
 * @icon_info: a #GtkIconInfo from gtk_icon_theme_lookup_icon()
 * @fg: a #GdkRGBA representing the foreground color of the icon
 * @success_color: (allow-none): a #GdkRGBA representing the warning color
 *     of the icon or %NULL to use the default color
 * @warning_color: (allow-none): a #GdkRGBA representing the warning color
 *     of the icon or %NULL to use the default color
 * @error_color: (allow-none): a #GdkRGBA representing the error color
 *     of the icon or %NULL to use the default color (allow-none)
 * @cancellable: (allow-none): optional #GCancellable object,
 *     %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the
 *     request is satisfied
 * @user_data: (closure): the data to pass to callback function
 *
 * Asynchronously load, render and scale a symbolic icon previously looked up
 * from the icon theme using gtk_icon_theme_lookup_icon().
 *
 * For more details, see gtk_icon_info_load_symbolic() which is the synchronous
 * version of this call.
 *
 * Since: 3.8
 **/
void
gtk_icon_info_load_symbolic_async (GtkIconInfo   *icon_info,
				   const GdkRGBA *fg,
				   const GdkRGBA *success_color,
				   const GdkRGBA *warning_color,
				   const GdkRGBA *error_color,
				   GCancellable         *cancellable,
				   GAsyncReadyCallback   callback,
				   gpointer              user_data)
{
  GTask *task;
  AsyncSymbolicData *data;
  SymbolicPixbufCache *symbolic_cache;
  GdkPixbuf *pixbuf;

  g_return_if_fail (icon_info != NULL);
  g_return_if_fail (fg != NULL);

  task = g_task_new (icon_info, cancellable, callback, user_data);

  data = g_slice_new0 (AsyncSymbolicData);
  g_task_set_task_data (task, data, (GDestroyNotify) async_symbolic_data_free);

  data->is_symbolic = gtk_icon_info_is_symbolic (icon_info);

  if (!data->is_symbolic)
    {
      gtk_icon_info_load_icon_async (icon_info, cancellable, async_load_no_symbolic_cb, g_object_ref (task));
    }
  else
    {
      symbolic_cache = symbolic_pixbuf_cache_matches (icon_info->symbolic_pixbuf_cache,
						      fg, success_color, warning_color, error_color);
      if (symbolic_cache)
	{
	  pixbuf = symbolic_cache_get_proxy (symbolic_cache, icon_info);
	  g_task_return_pointer (task, pixbuf, g_object_unref);
	}
      else
	{
	  if (fg)
	    {
	      data->fg = *fg;
	      data->fg_set = TRUE;
	    }

	  if (success_color)
	    {
	      data->success_color = *success_color;
	      data->success_color_set = TRUE;
	    }

	  if (warning_color)
	    {
	      data->warning_color = *warning_color;
	      data->warning_color_set = TRUE;
	    }

	  if (error_color)
	    {
	      data->error_color = *error_color;
	      data->error_color_set = TRUE;
	    }

	  data->dup = icon_info_dup (icon_info);
	  g_task_run_in_thread (task, load_symbolic_icon_thread);
	}
    }
  g_object_unref (task);
}

/**
 * gtk_icon_info_load_symbolic_finish:
 * @icon_info: a #GtkIconInfo from gtk_icon_theme_lookup_icon()
 * @res: a #GAsyncResult
 * @was_symbolic: (out) (allow-none): a #gboolean, returns whether the
 *     loaded icon was a symbolic one and whether the @fg color was
 *     applied to it.
 * @error: (allow-none): location to store error information on failure,
 *     or %NULL.
 *
 * Finishes an async icon load, see gtk_icon_info_load_symbolic_async().
 *
 * Returns: (transfer full): the rendered icon; this may be a newly
 *     created icon or a new reference to an internal icon, so you must
 *     not modify the icon. Use g_object_unref() to release your reference
 *     to the icon.
 *
 * Since: 3.8
 **/
GdkPixbuf *
gtk_icon_info_load_symbolic_finish (GtkIconInfo   *icon_info,
				    GAsyncResult  *result,
				    gboolean      *was_symbolic,
				    GError       **error)
{
  GTask *task = G_TASK (result);
  AsyncSymbolicData *data = g_task_get_task_data (task);
  SymbolicPixbufCache *symbolic_cache;
  GdkPixbuf *pixbuf;

  if (was_symbolic)
    *was_symbolic = data->is_symbolic;

  if (data->dup && !g_task_had_error (task))
    {
      pixbuf = g_task_propagate_pointer (task, NULL);

      g_assert (pixbuf != NULL); /* we checked for !had_error above */

      symbolic_cache = symbolic_pixbuf_cache_matches (icon_info->symbolic_pixbuf_cache,
						      data->fg_set ? &data->fg : NULL,
						      data->success_color_set ? &data->success_color : NULL,
						      data->warning_color_set ? &data->warning_color : NULL,
						      data->error_color_set ? &data->error_color : NULL);

      if (symbolic_cache == NULL)
	{
	  symbolic_cache = icon_info->symbolic_pixbuf_cache =
	    symbolic_pixbuf_cache_new (pixbuf,
				       data->fg_set ? &data->fg : NULL,
				       data->success_color_set ? &data->success_color : NULL,
				       data->warning_color_set ? &data->warning_color : NULL,
				       data->error_color_set ? &data->error_color : NULL,
				       icon_info->symbolic_pixbuf_cache);
	}

      g_object_unref (pixbuf);

      return symbolic_cache_get_proxy (symbolic_cache, icon_info);
    }

  return g_task_propagate_pointer (task, error);
}

/**
 * gtk_icon_info_load_symbolic_for_context_async:
 * @icon_info: a #GtkIconInfo from gtk_icon_theme_lookup_icon()
 * @context: a #GtkStyleContext
 * @cancellable: (allow-none): optional #GCancellable object,
 *     %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the
 *     request is satisfied
 * @user_data: (closure): the data to pass to callback function
 *
 * Asynchronously load, render and scale a symbolic icon previously looked up
 * from the icon theme using gtk_icon_theme_lookup_icon().
 *
 * For more details, see gtk_icon_info_load_symbolic_for_context() which is the synchronous
 * version of this call.
 *
 * Since: 3.8
 **/
void
gtk_icon_info_load_symbolic_for_context_async (GtkIconInfo      *icon_info,
					       GtkStyleContext  *context,
					       GCancellable     *cancellable,
					       GAsyncReadyCallback callback,
					       gpointer          user_data)
{
  GdkRGBA *color = NULL;
  GdkRGBA fg;
  GdkRGBA *fgp;
  GdkRGBA success_color;
  GdkRGBA *success_colorp;
  GdkRGBA warning_color;
  GdkRGBA *warning_colorp;
  GdkRGBA error_color;
  GdkRGBA *error_colorp;
  GtkStateFlags state;

  g_return_if_fail (icon_info != NULL);
  g_return_if_fail (context != NULL);

  fgp = success_colorp = warning_colorp = error_colorp = NULL;

  state = gtk_style_context_get_state (context);
  gtk_style_context_get (context, state, "color", &color, NULL);
  if (color)
    {
      fg = *color;
      fgp = &fg;
      gdk_rgba_free (color);
    }

  if (gtk_style_context_lookup_color (context, "success_color", &success_color))
    success_colorp = &success_color;

  if (gtk_style_context_lookup_color (context, "warning_color", &warning_color))
    warning_colorp = &warning_color;

  if (gtk_style_context_lookup_color (context, "error_color", &error_color))
    error_colorp = &error_color;

  gtk_icon_info_load_symbolic_async (icon_info,
				     fgp, success_colorp,
				     warning_colorp, error_colorp,
				     cancellable, callback, user_data);
}

/**
 * gtk_icon_info_load_symbolic_for_context_finish:
 * @icon_info: a #GtkIconInfo from gtk_icon_theme_lookup_icon()
 * @res: a #GAsyncResult
 * @was_symbolic: (out) (allow-none): a #gboolean, returns whether the
 *     loaded icon was a symbolic one and whether the @fg color was
 *     applied to it.
 * @error: (allow-none): location to store error information on failure,
 *     or %NULL.
 *
 * Finishes an async icon load, see gtk_icon_info_load_symbolic_for_context_async().
 *
 * Returns: (transfer full): the rendered icon; this may be a newly
 *     created icon or a new reference to an internal icon, so you must
 *     not modify the icon. Use g_object_unref() to release your reference
 *     to the icon.
 *
 * Since: 3.8
 **/
GdkPixbuf *
gtk_icon_info_load_symbolic_for_context_finish (GtkIconInfo      *icon_info,
						GAsyncResult     *result,
						gboolean         *was_symbolic,
						GError          **error)
{
  return gtk_icon_info_load_symbolic_finish (icon_info, result, was_symbolic, error);
}

static GdkRGBA *
color_to_rgba (GdkColor *color, GdkRGBA *rgba)
{
  rgba->red = color->red / 65535.0;
  rgba->green = color->green / 65535.0;
  rgba->blue = color->blue / 65535.0;
  rgba->alpha = 1.0;
  return rgba;
}

/**
 * gtk_icon_info_load_symbolic_for_style:
 * @icon_info: a #GtkIconInfo
 * @style: a #GtkStyle to take the colors from
 * @state: the widget state to use for colors
 * @was_symbolic: (out) (allow-none): a #gboolean, returns whether the
 *     loaded icon was a symbolic one and whether the @fg color was
 *     applied to it.
 * @error: (allow-none): location to store error information on failure,
 *     or %NULL.
 *
 * Loads an icon, modifying it to match the system colours for the foreground,
 * success, warning and error colors provided. If the icon is not a symbolic
 * one, the function will return the result from gtk_icon_info_load_icon().
 *
 * This allows loading symbolic icons that will match the system theme.
 *
 * See gtk_icon_info_load_symbolic() for more details.
 *
 * Returns: (transfer full): a #GdkPixbuf representing the loaded icon
 *
 * Since: 3.0
 *
 * Deprecated: 3.0: Use gtk_icon_info_load_symbolic_for_context() instead
 **/
GdkPixbuf *
gtk_icon_info_load_symbolic_for_style (GtkIconInfo   *icon_info,
                                       GtkStyle      *style,
                                       GtkStateType   state,
                                       gboolean      *was_symbolic,
                                       GError       **error)
{
  GdkColor color;
  GdkRGBA fg;
  GdkRGBA success_color;
  GdkRGBA *success_colorp;
  GdkRGBA warning_color;
  GdkRGBA *warning_colorp;
  GdkRGBA error_color;
  GdkRGBA *error_colorp;
  gboolean is_symbolic;

  g_return_val_if_fail (icon_info != NULL, NULL);
  g_return_val_if_fail (style != NULL, NULL);

  is_symbolic = gtk_icon_info_is_symbolic (icon_info);

  if (was_symbolic)
    *was_symbolic = is_symbolic;

  if (!is_symbolic)
    return gtk_icon_info_load_icon (icon_info, error);

  color_to_rgba (&style->fg[state], &fg);

  success_colorp = warning_colorp = error_colorp = NULL;

  if (gtk_style_lookup_color (style, "success_color", &color))
    success_colorp = color_to_rgba (&color, &success_color);

  if (gtk_style_lookup_color (style, "warning_color", &color))
    warning_colorp = color_to_rgba (&color, &warning_color);

  if (gtk_style_lookup_color (style, "error_color", &color))
    error_colorp = color_to_rgba (&color, &error_color);

  return _gtk_icon_info_load_symbolic_internal (icon_info,
						&fg, success_colorp,
						warning_colorp, error_colorp,
						TRUE,
						error);
}

/**
 * gtk_icon_info_set_raw_coordinates:
 * @icon_info: a #GtkIconInfo
 * @raw_coordinates: whether the coordinates of embedded rectangles
 *   and attached points should be returned in their original
 *   (unscaled) form.
 * 
 * Sets whether the coordinates returned by gtk_icon_info_get_embedded_rect()
 * and gtk_icon_info_get_attach_points() should be returned in their
 * original form as specified in the icon theme, instead of scaled
 * appropriately for the pixbuf returned by gtk_icon_info_load_icon().
 *
 * Raw coordinates are somewhat strange; they are specified to be with
 * respect to the unscaled pixmap for PNG and XPM icons, but for SVG
 * icons, they are in a 1000x1000 coordinate space that is scaled
 * to the final size of the icon.  You can determine if the icon is an SVG
 * icon by using gtk_icon_info_get_filename(), and seeing if it is non-%NULL
 * and ends in “.svg”.
 *
 * This function is provided primarily to allow compatibility wrappers
 * for older API's, and is not expected to be useful for applications.
 *
 * Since: 2.4
 **/
void
gtk_icon_info_set_raw_coordinates (GtkIconInfo *icon_info,
				   gboolean     raw_coordinates)
{
  g_return_if_fail (icon_info != NULL);
  
  icon_info->raw_coordinates = raw_coordinates != FALSE;
}

/* Scale coordinates from the icon data prior to returning
 * them to the user.
 */
static gboolean
icon_info_scale_point (GtkIconInfo  *icon_info,
		       gint          x,
		       gint          y,
		       gint         *x_out,
		       gint         *y_out)
{
  if (icon_info->raw_coordinates)
    {
      *x_out = x;
      *y_out = y;
    }
  else
    {
      if (!icon_info_ensure_scale_and_pixbuf (icon_info, TRUE))
	return FALSE;

      *x_out = 0.5 + x * icon_info->scale;
      *y_out = 0.5 + y * icon_info->scale;
    }

  return TRUE;
}

/**
 * gtk_icon_info_get_embedded_rect:
 * @icon_info: a #GtkIconInfo
 * @rectangle: (out): #GdkRectangle in which to store embedded
 *   rectangle coordinates; coordinates are only stored
 *   when this function returns %TRUE.
 *
 * Gets the coordinates of a rectangle within the icon
 * that can be used for display of information such
 * as a preview of the contents of a text file.
 * See gtk_icon_info_set_raw_coordinates() for further
 * information about the coordinate system.
 * 
 * Returns: %TRUE if the icon has an embedded rectangle
 *
 * Since: 2.4
 **/
gboolean
gtk_icon_info_get_embedded_rect (GtkIconInfo  *icon_info,
				 GdkRectangle *rectangle)
{
  g_return_val_if_fail (icon_info != NULL, FALSE);

  if (icon_info->data && icon_info->data->has_embedded_rect &&
      icon_info_ensure_scale_and_pixbuf (icon_info, TRUE))
    {
      gint scaled_x0, scaled_y0;
      gint scaled_x1, scaled_y1;
      
      if (rectangle)
	{
	  icon_info_scale_point (icon_info,
				 icon_info->data->x0, icon_info->data->y0,
				 &scaled_x0, &scaled_y0);
	  icon_info_scale_point (icon_info,
				 icon_info->data->x1, icon_info->data->y1,
				 &scaled_x1, &scaled_y1);
	  
	  rectangle->x = scaled_x0;
	  rectangle->y = scaled_y0;
	  rectangle->width = scaled_x1 - rectangle->x;
	  rectangle->height = scaled_y1 - rectangle->y;
	}

      return TRUE;
    }
  else
    return FALSE;
}

/**
 * gtk_icon_info_get_attach_points:
 * @icon_info: a #GtkIconInfo
 * @points: (allow-none) (array length=n_points) (out): location to store pointer to an array of points, or %NULL
 *          free the array of points with g_free().
 * @n_points: (allow-none): location to store the number of points in @points, or %NULL
 * 
 * Fetches the set of attach points for an icon. An attach point
 * is a location in the icon that can be used as anchor points for attaching
 * emblems or overlays to the icon.
 * 
 * Returns: %TRUE if there are any attach points for the icon.
 *
 * Since: 2.4
 **/
gboolean
gtk_icon_info_get_attach_points (GtkIconInfo *icon_info,
				 GdkPoint   **points,
				 gint        *n_points)
{
  g_return_val_if_fail (icon_info != NULL, FALSE);
  
  if (icon_info->data && icon_info->data->n_attach_points &&
      icon_info_ensure_scale_and_pixbuf (icon_info, TRUE))
    {
      if (points)
	{
	  gint i;
	  
	  *points = g_new (GdkPoint, icon_info->data->n_attach_points);
	  for (i = 0; i < icon_info->data->n_attach_points; i++)
	    icon_info_scale_point (icon_info,
				   icon_info->data->attach_points[i].x,
				   icon_info->data->attach_points[i].y,
				   &(*points)[i].x,
				   &(*points)[i].y);
	}
	  
      if (n_points)
	*n_points = icon_info->data->n_attach_points;

      return TRUE;
    }
  else
    {
      if (points)
	*points = NULL;
      if (n_points)
	*n_points = 0;
      
      return FALSE;
    }
}

/**
 * gtk_icon_info_get_display_name:
 * @icon_info: a #GtkIconInfo
 * 
 * Gets the display name for an icon. A display name is a
 * string to be used in place of the icon name in a user
 * visible context like a list of icons.
 * 
 * Returns: the display name for the icon or %NULL, if
 *  the icon doesn’t have a specified display name. This value
 *  is owned @icon_info and must not be modified or free.
 *
 * Since: 2.4
 **/
const gchar *
gtk_icon_info_get_display_name (GtkIconInfo *icon_info)
{
  g_return_val_if_fail (icon_info != NULL, NULL);

  if (icon_info->data)
    return icon_info->data->display_name;
  else
    return NULL;
}

/*
 * Builtin icons
 */


/**
 * gtk_icon_theme_add_builtin_icon:
 * @icon_name: the name of the icon to register
 * @size: the size in pixels at which to register the icon (different
 *        images can be registered for the same icon name
 *        at different sizes.)
 * @pixbuf: #GdkPixbuf that contains the image to use
 *          for @icon_name.
 * 
 * Registers a built-in icon for icon theme lookups. The idea
 * of built-in icons is to allow an application or library
 * that uses themed icons to function requiring files to
 * be present in the file system. For instance, the default
 * images for all of GTK+’s stock icons are registered
 * as built-icons.
 *
 * In general, if you use gtk_icon_theme_add_builtin_icon()
 * you should also install the icon in the icon theme, so
 * that the icon is generally available.
 *
 * This function will generally be used with pixbufs loaded
 * via gdk_pixbuf_new_from_inline().
 *
 * Since: 2.4
 **/
void
gtk_icon_theme_add_builtin_icon (const gchar *icon_name,
				 gint         size,
				 GdkPixbuf   *pixbuf)
{
  BuiltinIcon *default_icon;
  GSList *icons;
  gpointer key;

  g_return_if_fail (icon_name != NULL);
  g_return_if_fail (GDK_IS_PIXBUF (pixbuf));
  
  if (!icon_theme_builtin_icons)
    icon_theme_builtin_icons = g_hash_table_new (g_str_hash, g_str_equal);

  icons = g_hash_table_lookup (icon_theme_builtin_icons, icon_name);
  if (!icons)
    key = g_strdup (icon_name);
  else
    key = (gpointer)icon_name;	/* Won't get stored */

  default_icon = g_new (BuiltinIcon, 1);
  default_icon->size = size;
  default_icon->pixbuf = g_object_ref (pixbuf);
  icons = g_slist_prepend (icons, default_icon);

  /* Replaces value, leaves key untouched
   */
  g_hash_table_insert (icon_theme_builtin_icons, key, icons);
}

/* Look up a builtin icon; the min_difference_p and
 * has_larger_p out parameters allow us to combine
 * this lookup with searching through the actual directories
 * of the “hicolor” icon theme. See theme_lookup_icon()
 * for how they are used.
 */
static BuiltinIcon *
find_builtin_icon (const gchar *icon_name,
		   gint         size,
		   gint         scale,
		   gint        *min_difference_p)
{
  GSList *icons = NULL;
  gint min_difference = G_MAXINT;
  gboolean has_larger = FALSE;
  BuiltinIcon *min_icon = NULL;
  
  if (!icon_theme_builtin_icons)
    return NULL;

  size *= scale;

  icons = g_hash_table_lookup (icon_theme_builtin_icons, icon_name);

  while (icons)
    {
      BuiltinIcon *default_icon = icons->data;
      int min, max, difference;
      gboolean smaller;
      
      min = default_icon->size - 2;
      max = default_icon->size + 2;
      smaller = size < min;
      if (size < min)
	difference = min - size;
      else if (size > max)
	difference = size - max;
      else
	difference = 0;
      
      if (difference == 0)
	{
	  min_icon = default_icon;
	  break;
	}
      
      if (!has_larger)
	{
	  if (difference < min_difference || smaller)
	    {
	      min_difference = difference;
	      min_icon = default_icon;
	      has_larger = smaller;
	    }
	}
      else
	{
	  if (difference < min_difference && smaller)
	    {
	      min_difference = difference;
	      min_icon = default_icon;
	    }
	}
      
      icons = icons->next;
    }

  if (min_difference_p)
    *min_difference_p = min_difference;

  return min_icon;
}

/**
 * gtk_icon_theme_lookup_by_gicon:
 * @icon_theme: a #GtkIconTheme
 * @icon: the #GIcon to look up
 * @size: desired icon size
 * @flags: flags modifying the behavior of the icon lookup
 * 
 * Looks up an icon and returns a #GtkIconInfo containing
 * information such as the filename of the icon. 
 * The icon can then be rendered into a pixbuf using
 * gtk_icon_info_load_icon().
 *
 * Returns: (transfer full): a #GtkIconInfo containing 
 *     information about the icon, or %NULL if the icon 
 *     wasn’t found. Unref with g_object_unref()
 *
 * Since: 2.14
 */
GtkIconInfo *
gtk_icon_theme_lookup_by_gicon (GtkIconTheme       *icon_theme,
                                GIcon              *icon,
                                gint                size,
                                GtkIconLookupFlags  flags)
{
  return gtk_icon_theme_lookup_by_gicon_for_scale (icon_theme, icon,
                                                   size, 1, flags);
}


/**
 * gtk_icon_theme_lookup_by_gicon_for_scale:
 * @icon_theme: a #GtkIconTheme
 * @icon: the #GIcon to look up
 * @size: desired icon size
 * @scale: the desired scale
 * @flags: flags modifying the behavior of the icon lookup
 *
 * Looks up an icon and returns a #GtkIconInfo containing
 * information such as the filename of the icon.
 * The icon can then be rendered into a pixbuf using
 * gtk_icon_info_load_icon().
 *
 * Returns: (transfer full): a #GtkIconInfo containing
 *     information about the icon, or %NULL if the icon
 *     wasn’t found. Unref with g_object_unref()
 *
 * Since: 3.10
 */
GtkIconInfo *
gtk_icon_theme_lookup_by_gicon_for_scale (GtkIconTheme       *icon_theme,
                                          GIcon              *icon,
                                          gint                size,
                                          gint                scale,
                                          GtkIconLookupFlags  flags)
{
  GtkIconInfo *info;

  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (G_IS_ICON (icon), NULL);

  if (GDK_IS_PIXBUF (icon))
    {
      GdkPixbuf *pixbuf;

      pixbuf = GDK_PIXBUF (icon);

      if ((flags & GTK_ICON_LOOKUP_FORCE_SIZE) != 0)
        {
          gint width, height, max;
          gdouble pixbuf_scale;
          GdkPixbuf *scaled;

          width = gdk_pixbuf_get_width (pixbuf);
          height = gdk_pixbuf_get_height (pixbuf);
          max = MAX (width, height);
          pixbuf_scale = (gdouble) size * scale / (gdouble) max;

          scaled = gdk_pixbuf_scale_simple (pixbuf,
                                            0.5 + width * pixbuf_scale,
                                            0.5 + height * pixbuf_scale,
                                            GDK_INTERP_BILINEAR);

          info = gtk_icon_info_new_for_pixbuf (icon_theme, scaled);

          g_object_unref (scaled);
        }
      else
        {
          info = gtk_icon_info_new_for_pixbuf (icon_theme, pixbuf);
        }

      return info;
    }
  else if (G_IS_LOADABLE_ICON (icon))
    {
      info = icon_info_new (ICON_THEME_DIR_UNTHEMED, size, 1);
      info->loadable = G_LOADABLE_ICON (g_object_ref (icon));

      if (G_IS_FILE_ICON (icon))
        {
          GFile *file = g_file_icon_get_file (G_FILE_ICON (icon));
          if (file != NULL)
            {
              info->icon_file = g_object_ref (file);
              info->filename = g_file_get_path (file);
            }
        }

      info->desired_size = size;
      info->desired_scale = scale;
      info->threshold = 2;
      info->forced_size = (flags & GTK_ICON_LOOKUP_FORCE_SIZE) != 0;

      return info;
    }
  else if (G_IS_THEMED_ICON (icon))
    {
      const gchar **names;

      names = (const gchar **)g_themed_icon_get_names (G_THEMED_ICON (icon));
      info = gtk_icon_theme_choose_icon_for_scale (icon_theme, names, size, scale, flags);

      return info;
    }
  else if (G_IS_EMBLEMED_ICON (icon))
    {
      GIcon *base, *emblem;
      GList *list, *l;
      GtkIconInfo *base_info, *emblem_info;

      if (GTK_IS_NUMERABLE_ICON (icon))
        _gtk_numerable_icon_set_background_icon_size (GTK_NUMERABLE_ICON (icon), size / 2);

      base = g_emblemed_icon_get_icon (G_EMBLEMED_ICON (icon));
      base_info = gtk_icon_theme_lookup_by_gicon_for_scale (icon_theme, base, size, scale, flags);
      if (base_info)
        {
          info = icon_info_dup (base_info);
          g_object_unref (base_info);

          list = g_emblemed_icon_get_emblems (G_EMBLEMED_ICON (icon));
          for (l = list; l; l = l->next)
            {
              emblem = g_emblem_get_icon (G_EMBLEM (l->data));
	      /* always force size for emblems */
              emblem_info = gtk_icon_theme_lookup_by_gicon_for_scale (icon_theme, emblem, size / 2, scale, flags | GTK_ICON_LOOKUP_FORCE_SIZE);
              if (emblem_info)
                info->emblem_infos = g_slist_prepend (info->emblem_infos, emblem_info);
            }

          return info;
        }
      else
        return NULL;
    }

  return NULL;
}

/**
 * gtk_icon_info_new_for_pixbuf:
 * @icon_theme: a #GtkIconTheme
 * @pixbuf: the pixbuf to wrap in a #GtkIconInfo
 *
 * Creates a #GtkIconInfo for a #GdkPixbuf.
 *
 * Returns: (transfer full): a #GtkIconInfo
 *
 * Since: 2.14
 */
GtkIconInfo *
gtk_icon_info_new_for_pixbuf (GtkIconTheme *icon_theme,
                              GdkPixbuf    *pixbuf)
{
  GtkIconInfo *info;

  g_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);
  g_return_val_if_fail (GDK_IS_PIXBUF (pixbuf), NULL);

  info = icon_info_new (ICON_THEME_DIR_UNTHEMED, 0, 1);
  info->pixbuf = g_object_ref (pixbuf);
  info->scale = 1.0;

  return info;
}