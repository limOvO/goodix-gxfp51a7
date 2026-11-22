// Verifies against the enrolled template and reports each score, so matcher
// quality can be measured directly. The driver logs the score; this drives a
// verify attempt per press without fprintd or polkit in the way.
//
//   cc test-verify.c -o test-verify $(pkg-config --cflags libfprint-2-tod-1) \
//      -L/opt/libfprint-tod/lib -lfprint-2-tod -lfprint-2 \
//      $(pkg-config --libs gio-2.0 gobject-2.0 glib-2.0)
//   LD_LIBRARY_PATH=/opt/libfprint-tod/lib ./test-verify [attempts]
#include <fprint.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>

static GMainLoop *loop;
static int remaining;
static FpPrint *tmpl;

static void on_verify_done (FpDevice *d, GAsyncResult *res, gpointer u);

static void
on_match (FpDevice *dev, FpPrint *match, FpPrint *print, gpointer u, GError *err)
{
  (void) print; (void) u;
  if (err)
    printf ("  match: error (%s)\n", err->message);
  else
    printf ("  match: %s\n", match ? "MATCH" : "no match");
}

static void
start_verify (FpDevice *dev)
{
  fp_device_verify (dev, tmpl, NULL, on_match, NULL, NULL,
                    (GAsyncReadyCallback) on_verify_done, NULL);
}

static void
on_verify_done (FpDevice *dev, GAsyncResult *res, gpointer u)
{
  GError *err = NULL;
  gboolean matched = FALSE;
  FpPrint *out = NULL;

  (void) u;
  fp_device_verify_finish (dev, res, &matched, &out, &err);
  if (err)
    printf ("  verify: %s\n", err->message);
  else
    printf ("  verify: %s\n", matched ? "MATCH" : "no match");
  g_clear_object (&out);

  if (--remaining > 0)
    start_verify (dev);
  else
    g_main_loop_quit (loop);
}

static void
on_opened (FpDevice *dev, GAsyncResult *res, gpointer u)
{
  GError *err = NULL;
  GPtrArray *prints;
  GError *lerr = NULL;

  (void) u;
  if (!fp_device_open_finish (dev, res, &err))
    {
      printf ("open failed: %s\n", err->message);
      g_main_loop_quit (loop);
      return;
    }

  /* fprintd owns the storage, so load the template file it saved. */
  {
    g_autofree guchar *data = NULL;
    gsize len = 0;
    const char *path = g_getenv ("GX_TEMPLATE");

    (void) prints; (void) lerr;
    if (!path || !g_file_get_contents (path, (gchar **) &data, &len, NULL))
      {
        printf ("cannot read template (set GX_TEMPLATE)\n");
        g_main_loop_quit (loop);
        return;
      }
    tmpl = fp_print_deserialize (data, len, &lerr);
    if (!tmpl)
      {
        printf ("deserialize failed: %s\n", lerr ? lerr->message : "?");
        g_main_loop_quit (loop);
        return;
      }
    printf ("loaded template from %s (%zu bytes)\n", path, len);
  }
  printf ("verifying %d time(s) against the enrolled print\n", remaining);
  start_verify (dev);
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) ctx = fp_context_new ();
  GPtrArray *devs = fp_context_get_devices (ctx);

  if (!devs || devs->len == 0) { printf ("no devices\n"); return 1; }
  remaining = argc > 1 ? atoi (argv[1]) : 1;

  loop = g_main_loop_new (NULL, FALSE);
  fp_device_open (g_ptr_array_index (devs, 0), NULL,
                  (GAsyncReadyCallback) on_opened, NULL);
  g_main_loop_run (loop);
  return 0;
}
