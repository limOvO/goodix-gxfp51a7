// Exercises the enrolled/capture path directly against libfprint, with no
// fprintd and no polkit in the way. Prints the driver's own diagnostics.
//
//   cc test-direct.c -o test-direct $(pkg-config --cflags --libs libfprint-2-tod-1)
//   LD_LIBRARY_PATH=/opt/libfprint-tod/lib \
//   FP_TOD_DRIVERS_DIR=/opt/libfprint-tod/lib/libfprint-2/tod-1 ./test-direct
#include <fprint.h>
#include <gio/gio.h>
#include <stdio.h>

static GMainLoop *loop;
static FpDevice *dev;
static FpPrint *print;

static void
on_enroll_progress (FpDevice *d, gint stage, FpPrint *p,
                    gpointer u, GError *err)
{
  if (err)
    {
      printf ("  stage %d: retry (%s)\n", stage, err->message);
      return;
    }
  printf ("  stage %d ok\n", stage);
}

static void
on_enroll_done (FpDevice *d, GAsyncResult *res, gpointer u)
{
  GError *err = NULL;

  print = fp_device_enroll_finish (d, res, &err);
  if (err)
    printf ("ENROLL FAILED: %s\n", err->message);
  else
    printf ("ENROLL OK\n");
  g_main_loop_quit (loop);
}

static void
on_open (FpDevice *d, GAsyncResult *res, gpointer u)
{
  GError *err = NULL;

  if (!fp_device_open_finish (d, res, &err))
    {
      printf ("OPEN FAILED: %s\n", err->message);
      g_main_loop_quit (loop);
      return;
    }
  printf ("opened: %s (%s)\n", fp_device_get_name (d), fp_device_get_driver (d));

  FpPrint *p = fp_print_new (d);
  fp_print_set_finger (p, FP_FINGER_RIGHT_INDEX);
  fp_device_enroll (d, p, NULL, on_enroll_progress, NULL, NULL,
                    (GAsyncReadyCallback) on_enroll_done, NULL);
}

int
main (void)
{
  g_autoptr(FpContext) ctx = fp_context_new ();
  GPtrArray *devs = fp_context_get_devices (ctx);

  if (!devs || devs->len == 0)
    {
      printf ("no devices\n");
      return 1;
    }
  dev = g_ptr_array_index (devs, 0);
  printf ("device: %s\n", fp_device_get_name (dev));

  loop = g_main_loop_new (NULL, FALSE);
  fp_device_open (dev, NULL, (GAsyncReadyCallback) on_open, NULL);
  g_main_loop_run (loop);
  return 0;
}
