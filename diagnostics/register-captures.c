// Registers a set of capture dumps against each other using the driver's own
// SIFT island registration — the same code enrolment uses for coverage
// guidance — and prints each capture's offset.
//
// Averaging misaligned fingerprints destroys the ridges, so the merge has to be
// a stitch, not a mean. These offsets are what the stitcher needs.
//
//   cc register-captures.c -o register-captures goodix_sift.c \
//      $(pkg-config --cflags --libs glib-2.0) -lm -I.
//   ./register-captures p001.bin p002.bin ...
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include "goodix_sift.h"

#define W 132
#define H 112

static double *
load_diff (const char *cap)
{
  gsize need = sizeof (guint16) * W * H;
  g_autofree guint16 *i = NULL, *b = NULL;
  gsize il = 0, bl = 0;
  g_autofree gchar *bgpath = g_strconcat (cap, ".bg", NULL);
  double *d;

  if (!g_file_get_contents (cap, (gchar **) &i, &il, NULL) || il != need)
    return NULL;
  if (!g_file_get_contents (bgpath, (gchar **) &b, &bl, NULL) || bl != need)
    return NULL;

  d = g_new (double, W * H);
  for (gsize k = 0; k < W * H; k++)
    d[k] = (double) b[k] - (double) i[k];
  return d;
}

/* Removes the row/column banding the driver's preprocess does, so descriptors
 * see ridges rather than sensor offsets. */
static void
flatten (double *d)
{
  g_autofree double *buf = g_new (double, MAX (W, H));

  for (int y = 0; y < H; y++)
    {
      memcpy (buf, d + y * W, sizeof (double) * W);
      g_qsort_with_data (buf, W, sizeof (double),
                         (GCompareDataFunc) g_strcmp0, NULL);
      double m = buf[W / 2];
      for (int x = 0; x < W; x++)
        d[y * W + x] -= m;
    }
  for (int x = 0; x < W; x++)
    {
      for (int y = 0; y < H; y++)
        buf[y] = d[y * W + x];
      g_qsort_with_data (buf, H, sizeof (double),
                         (GCompareDataFunc) g_strcmp0, NULL);
      double m = buf[H / 2];
      for (int y = 0; y < H; y++)
        d[y * W + x] -= m;
    }
}

int main (int argc, char **argv)
{
  GxSiftIsland *isl;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  guint npts = 0;

  if (argc < 2) { fprintf (stderr, "usage: %s <pNNN.bin>...\n", argv[0]); return 1; }

  isl = gx_sift_island_new ();
  for (int a = 1; a < argc; a++)
    {
      double *d = load_diff (argv[a]);
      GxSiftFeatures *f;
      int dx = 0, dy = 0, sc = 0;
      gboolean placed;

      if (!d) { fprintf (stderr, "skip %s\n", argv[a]); continue; }
      flatten (d);
      f = gx_sift_extract (d, W, H);
      g_free (d);
      if (!f) { fprintf (stderr, "no features in %s\n", argv[a]); continue; }

      placed = gx_sift_island_add (isl, f, &dx, &dy, &sc);
      gx_sift_island_extent (isl, &x0, &y0, &x1, &y1, &npts);
      printf ("%s %d %d %d %d %d\n", argv[a], placed ? 1 : 0, dx, dy, sc, f->n);
      gx_sift_free (f);
    }

  fprintf (stderr, "island extent %dx%d (%d..%d, %d..%d), %u pts\n",
           x1 - x0, y1 - y0, x0, x1, y0, y1, npts);
  return 0;
}
