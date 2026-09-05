/* fw-theme.c — Owned dark/light theming (libadwaita replacement).
 *
 * Libadwaita used to hand Framework follow-system dark/light for free
 * via a portal-backed AdwStyleManager.  Having dropped libadwaita
 * (v0.80.0), we own that path here: read the freedesktop appearance
 * portal directly over D-Bus, map it to a Kanagawa Dragon (dark) /
 * Kanagawa Lotus (light) palette injected as GTK named colours, apply
 * the owned structural stylesheet on top, and re-apply live when the
 * portal reports a change.  No new dependency — Gio ships with GLib;
 * degrades to the dark default when no portal answers.
 *
 * Pattern ported from the Hermitage sibling (hermitage/theme.py).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-theme.h"
#include "fw-debug.h"

#include <gio/gio.h>

/* ── Palettes — Kanagawa Dragon (dark) and Kanagawa Lotus (light) ────── */

typedef struct {
  const char *window_bg,  *window_fg;
  const char *view_bg,    *view_fg;
  const char *card_bg,    *card_fg, *card_shade;
  const char *headerbar_bg, *headerbar_fg;
  const char *popover_bg, *popover_fg;
  const char *dialog_bg,  *dialog_fg;
  const char *sidebar_bg, *sidebar_fg;
  const char *accent,     *accent_fg;
  const char *success,    *warning, *error;
  const char *borders;
} Palette;

static const Palette DARK = {
  .window_bg = "#181616", .window_fg = "#c5c9c5",
  .view_bg = "#1d1c19",   .view_fg = "#c5c9c5",
  .card_bg = "#201f1e",   .card_fg = "#c5c9c5", .card_shade = "#141312",
  .headerbar_bg = "#1d1c19", .headerbar_fg = "#c5c9c5",
  .popover_bg = "#23211f", .popover_fg = "#c5c9c5",
  .dialog_bg = "#1d1c19",  .dialog_fg = "#c5c9c5",
  .sidebar_bg = "#1a1918", .sidebar_fg = "#c5c9c5",
  .accent = "#8ba4b0",     .accent_fg = "#181616",   /* dragonBlue2 */
  .success = "#8a9a7b",    .warning = "#c4b28a", .error = "#c4746e",
  .borders = "#393836",                             /* dragonBlack5 */
};

static const Palette LIGHT = {
  .window_bg = "#f2ecbc", .window_fg = "#545464",    /* lotusInk1 */
  .view_bg = "#e7dba0",   .view_fg = "#545464",
  .card_bg = "#e5ddb0",   .card_fg = "#545464", .card_shade = "#dcd5ac",
  .headerbar_bg = "#e5ddb0", .headerbar_fg = "#545464",
  .popover_bg = "#e7dba0", .popover_fg = "#545464",
  .dialog_bg = "#f2ecbc",  .dialog_fg = "#545464",
  .sidebar_bg = "#e5ddb0", .sidebar_fg = "#545464",
  .accent = "#4d699b",     .accent_fg = "#f2ecbc",   /* lotusBlue4 */
  .success = "#6f894e",    .warning = "#77713f", .error = "#c84053",
  .borders = "#cabf83",
};

/* Expand a palette into the full GTK named-colour block, so both the
 * owned structural sheet and the stock widget internals (buttons,
 * entries, menus, scrollbars) pick up the Kanagawa colours. */
static char *
palette_css (const Palette *p)
{
  return g_strdup_printf (
    "@define-color window_bg_color %s;\n"
    "@define-color window_fg_color %s;\n"
    "@define-color view_bg_color %s;\n"
    "@define-color view_fg_color %s;\n"
    "@define-color card_bg_color %s;\n"
    "@define-color card_fg_color %s;\n"
    "@define-color card_shade_color %s;\n"
    "@define-color headerbar_bg_color %s;\n"
    "@define-color headerbar_fg_color %s;\n"
    "@define-color headerbar_border_color %s;\n"
    "@define-color headerbar_backdrop_color %s;\n"
    "@define-color popover_bg_color %s;\n"
    "@define-color popover_fg_color %s;\n"
    "@define-color dialog_bg_color %s;\n"
    "@define-color dialog_fg_color %s;\n"
    "@define-color sidebar_bg_color %s;\n"
    "@define-color sidebar_fg_color %s;\n"
    "@define-color sidebar_backdrop_color %s;\n"
    "@define-color sidebar_border_color %s;\n"
    "@define-color accent_color %s;\n"
    "@define-color accent_bg_color %s;\n"
    "@define-color accent_fg_color %s;\n"
    "@define-color success_color %s;\n"
    "@define-color warning_color %s;\n"
    "@define-color error_color %s;\n"
    "@define-color destructive_color %s;\n"
    "@define-color borders %s;\n",
    p->window_bg, p->window_fg,
    p->view_bg, p->view_fg,
    p->card_bg, p->card_fg, p->card_shade,
    p->headerbar_bg, p->headerbar_fg, p->headerbar_fg, p->window_bg,
    p->popover_bg, p->popover_fg,
    p->dialog_bg, p->dialog_fg,
    p->sidebar_bg, p->sidebar_fg, p->window_bg, p->borders,
    p->accent, p->accent, p->accent_fg,
    p->success, p->warning, p->error, p->error,
    p->borders);
}

/* ── GObject ────────────────────────────────────────────────────────── */

struct _FwTheme {
  GObject          parent_instance;
  gboolean         dark;
  GtkCssProvider  *palette_provider;   /* swapped on scheme change */
  GtkCssProvider  *style_provider;     /* static structural sheet */
  GDBusProxy      *proxy;              /* portal Settings, or NULL */
};

enum { PROP_0, PROP_DARK, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE (FwTheme, fw_theme, G_TYPE_OBJECT)

static FwTheme *the_theme = NULL;

static void
fw_theme_get_property (GObject *object, guint prop_id,
                       GValue *value, GParamSpec *pspec)
{
  FwTheme *self = FW_THEME (object);
  if (prop_id == PROP_DARK)
    g_value_set_boolean (value, self->dark);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
fw_theme_class_init (FwThemeClass *klass)
{
  GObjectClass *o = G_OBJECT_CLASS (klass);
  o->get_property = fw_theme_get_property;
  props[PROP_DARK] = g_param_spec_boolean (
    "dark", NULL, NULL, TRUE,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (o, N_PROPS, props);
}

static void
fw_theme_init (FwTheme *self)
{
  self->dark = TRUE;   /* default until the portal is queried */
}

/* ── Structural stylesheet resolution ───────────────────────────────── */

/* Probe the same source order fw-fonts uses, returning the readable
 * style.css path or NULL. */
static char *
find_style_css (void)
{
  const char *candidates[] = {
    g_getenv ("FW_STYLE_CSS"),
  };
  if (candidates[0] && g_file_test (candidates[0], G_FILE_TEST_IS_REGULAR))
    return g_strdup (candidates[0]);

  const char *xdg = g_getenv ("FRAMEWORK_DATADIR");
  if (xdg) {
    g_autofree char *p =
      g_build_filename (xdg, "framework", "style.css", NULL);
    if (g_file_test (p, G_FILE_TEST_IS_REGULAR))
      return g_steal_pointer (&p);
  }
  {
    g_autofree char *p =
      g_build_filename (FW_DATADIR, "framework", "style.css", NULL);
    if (g_file_test (p, G_FILE_TEST_IS_REGULAR))
      return g_steal_pointer (&p);
  }
  {
    g_autofree char *p =
      g_build_filename (FW_SOURCE_ROOT, "data", "style.css", NULL);
    if (g_file_test (p, G_FILE_TEST_IS_REGULAR))
      return g_steal_pointer (&p);
  }
  return NULL;
}

/* ── Palette application ────────────────────────────────────────────── */

static void
apply_scheme (FwTheme *self, gboolean dark)
{
  g_autofree char *css = palette_css (dark ? &DARK : &LIGHT);
  gtk_css_provider_load_from_string (self->palette_provider, css);

  GtkSettings *settings = gtk_settings_get_default ();
  if (settings)
    g_object_set (settings, "gtk-application-prefer-dark-theme", dark, NULL);

  if (self->dark != dark) {
    self->dark = dark;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DARK]);
  }
}

/* ── Portal color-scheme resolution ─────────────────────────────────── */

#define APPEARANCE   "org.freedesktop.appearance"
#define COLOR_SCHEME "color-scheme"

static GDBusProxy *
get_proxy (FwTheme *self)
{
  if (!self->proxy) {
    self->proxy = g_dbus_proxy_new_for_bus_sync (
      G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Settings",
      NULL, NULL);
  }
  return self->proxy;
}

/* Descend through nested 'v' wrappers to the concrete value.  ReadOne
 * returns (v); the deprecated Read double-wraps.
 *
 * Ownership quirk (verify before "simplifying"; LSan proved this):
 * g_variant_ref_sink ADDS a reference when handed a non-floating
 * variant, so after unwrap() the caller holds a reference to BOTH the
 * returned value AND the wrapper it passed in, and must unref both.
 * on_settings_signal does; query_dark must too. */
static GVariant *
unwrap (GVariant *v)
{
  g_variant_ref_sink (v);
  while (v && g_variant_is_of_type (v, G_VARIANT_TYPE_VARIANT)) {
    GVariant *inner = g_variant_get_variant (v);
    g_variant_unref (v);
    v = inner;
  }
  return v;
}

/* Query the portal for the preferred colour scheme.  1=dark, 2=light,
 * 0/none/no-portal → dark default. Framework's no-answer default is
 * dark, so "no preference" (0) and any unknown value stay dark too;
 * only an explicit 2 turns the lights on. */
static gboolean
query_dark (FwTheme *self)
{
  GDBusProxy *proxy = get_proxy (self);
  if (!proxy)
    return TRUE;

  GVariant *ret = g_dbus_proxy_call_sync (
    proxy, "ReadOne",
    g_variant_new ("(ss)", APPEARANCE, COLOR_SCHEME),
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  if (!ret) {
    /* Older portals only expose the deprecated double-wrapped Read. */
    ret = g_dbus_proxy_call_sync (
      proxy, "Read",
      g_variant_new ("(ss)", APPEARANCE, COLOR_SCHEME),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  }
  if (!ret)
    return TRUE;

  GVariant *child = g_variant_get_child_value (ret, 0);
  GVariant *val = unwrap (child);
  guint32 scheme = (val && g_variant_is_of_type (val, G_VARIANT_TYPE_UINT32))
                     ? g_variant_get_uint32 (val) : 0;
  /* See unwrap(): both the value and the wrapper carry a reference
   * here. Missing the wrapper's unref leaked one GVariant tree per
   * portal query (the sweep's "unref the GVariant child" finding). */
  if (val && val != child)
    g_variant_unref (val);
  g_variant_unref (child);
  g_variant_unref (ret);
  /* scheme == 2 is the portal's only "light" answer; 0 (no preference)
   * and 1 (dark) both keep the dark palette. */
  return scheme != 2;
}

static void
on_settings_signal (GDBusProxy *proxy G_GNUC_UNUSED,
                    const char *sender  G_GNUC_UNUSED,
                    const char *signal_name,
                    GVariant   *params,
                    gpointer    user_data)
{
  if (g_strcmp0 (signal_name, "SettingChanged") != 0)
    return;
  FwTheme *self = FW_THEME (user_data);

  const char *ns = NULL, *key = NULL;
  GVariant *val = NULL;
  g_variant_get (params, "(&s&sv)", &ns, &key, &val);
  if (g_strcmp0 (ns, APPEARANCE) == 0 && g_strcmp0 (key, COLOR_SCHEME) == 0) {
    GVariant *inner = unwrap (val);
    guint32 scheme = (inner && g_variant_is_of_type (inner, G_VARIANT_TYPE_UINT32))
                       ? g_variant_get_uint32 (inner) : 0;
    if (inner) g_variant_unref (inner);
    /* Same contract as query_dark: only an explicit 2 is light. */
    apply_scheme (self, scheme != 2);
    FW_TRACE_WINDOW ("theme: portal scheme changed → %s",
                     scheme != 2 ? "dark" : "light");
  }
  if (val) g_variant_unref (val);
}

/* ── Public API ─────────────────────────────────────────────────────── */

void
fw_theme_install (void)
{
  if (the_theme)
    return;

  GdkDisplay *display = gdk_display_get_default ();
  if (!display) {
    g_warning ("theme: no display — cannot install stylesheet");
    return;
  }

  the_theme = g_object_new (FW_TYPE_THEME, NULL);
  FwTheme *self = the_theme;

  /* Structural sheet first, palette on top; both above PRIORITY_USER so
   * a user ~/.config/gtk-4.0/gtk.css (which loads at USER) cannot
   * outrank them. */
  self->style_provider = gtk_css_provider_new ();
  g_autofree char *css_path = find_style_css ();
  if (css_path) {
    gtk_css_provider_load_from_path (self->style_provider, css_path);
    FW_TRACE_WINDOW ("theme: loaded stylesheet '%s'", css_path);
  } else {
    g_warning ("theme: no style.css found — UI will be unstyled");
  }
  gtk_style_context_add_provider_for_display (
    display, GTK_STYLE_PROVIDER (self->style_provider),
    GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

  self->palette_provider = gtk_css_provider_new ();
  gtk_style_context_add_provider_for_display (
    display, GTK_STYLE_PROVIDER (self->palette_provider),
    GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

  apply_scheme (self, query_dark (self));

  /* Subscribe to live scheme changes. */
  if (get_proxy (self))
    g_signal_connect (self->proxy, "g-signal",
                      G_CALLBACK (on_settings_signal), self);
}

FwTheme *
fw_theme_get_default (void)
{
  return the_theme;
}

gboolean
fw_theme_get_dark (FwTheme *self)
{
  if (!self)
    return TRUE;
  g_return_val_if_fail (FW_IS_THEME (self), TRUE);
  return self->dark;
}
