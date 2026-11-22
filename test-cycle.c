// Opens the device, then closes it cleanly — enough to exercise the timing
// bookkeeping on close without needing a finger.
//
//   cc test-cycle.c -o test-cycle $(pkg-config --cflags libfprint-2-tod-1)
//   LD_LIBRARY_PATH=/opt/libfprint-tod/lib ./test-cycle
#include <fprint.h>
#include <stdio.h>

static GMainLoop *loop;

static void
on_closed (FpDevice *d, GAsyncResult *res, gpointer u)
{
  GError *err = NULL;

  if (!fp_device_close_finish (d, res, &err))
    printf ("close failed: %s\n", err->message);
  else
    printf ("closed cleanly\n");
  g_main_loop_quit (loop);
}

static void
on_opened (FpDevice *d, GAsyncResult *res, gpointer u)
{
  GError *err = NULL;

  if (!fp_device_open_finish (d, res, &err))
    {
      printf ("open failed: %s\n", err->message);
      g_main_loop_quit (loop);
      return;
    }
  fp_device_close (d, NULL, (GAsyncReadyCallback) on_closed, NULL);
}

int
main (void)
{
  g_autoptr(FpContext) ctx = fp_context_new ();
  GPtrArray *devs = fp_context_get_devices (ctx);
  FpDevice *dev;

  if (!devs || devs->len == 0)
    {
      printf ("no devices\n");
      return 1;
    }
  dev = g_ptr_array_index (devs, 0);
  loop = g_main_loop_new (NULL, FALSE);
  fp_device_open (dev, NULL, (GAsyncReadyCallback) on_opened, NULL);
  g_main_loop_run (loop);
  return 0;
}
