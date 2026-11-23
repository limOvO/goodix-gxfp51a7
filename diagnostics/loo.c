/*
 * Leave-one-out: is the failure the matcher, or a template that never saw
 * these angles?
 *
 *   ./loo <dir> [threshold]
 *
 * For every capture in the directory, treat it as the probe and the rest as the
 * template, and report the score exactly as the driver computes it: flag the
 * probe's points matched by any template view, and count the union. That is the
 * same fusion rule as gx_verify, so the numbers are comparable to a real press.
 *
 * Also reports each probe's best single-view score and the rotation the rigid
 * fit found, which says whether the rotated presses are being reached at all.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "goodix_sift.h"

#define IW 132
#define IH 112
#define NN (IW * IH)

static int cmp_dbl (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return (x > y) - (x < y);
}

static double *
load_diff (const char *cap)
{
  gsize need = sizeof (guint16) * NN;
  g_autofree guint16 *f = NULL, *b = NULL;
  gsize fl = 0, bl = 0;
  g_autofree gchar *bgpath = g_strconcat (cap, ".bg", NULL);
  double *d;

  if (!g_file_get_contents (cap, (gchar **) &f, &fl, NULL) || fl != need)
    return NULL;
  if (!g_file_get_contents (bgpath, (gchar **) &b, &bl, NULL) || bl != need)
    return NULL;

  d = g_new (double, NN);
  for (gsize i = 0; i < (gsize) NN; i++)
    d[i] = (double) b[i] - (double) f[i];
  return d;
}

static void
prepare (double *d)
{
  g_autofree double *buf = g_new (double, MAX (IW, IH));

  for (int y = 0; y < IH; y++)
    {
      memcpy (buf, d + y * IW, sizeof (double) * IW);
      qsort (buf, IW, sizeof (double), cmp_dbl);
      double m = buf[IW / 2];
      for (int x = 0; x < IW; x++)
        d[y * IW + x] -= m;
    }
  for (int x = 0; x < IW; x++)
    {
      for (int y = 0; y < IH; y++)
        buf[y] = d[y * IW + x];
      qsort (buf, IH, sizeof (double), cmp_dbl);
      double m = buf[IH / 2];
      for (int y = 0; y < IH; y++)
        d[y * IW + x] -= m;
    }
}

int
main (int argc, char **argv)
{
  const char *dir = argc > 1 ? argv[1] : "/tmp/sess";
  int threshold = argc > 2 ? atoi (argv[2]) : 20;

  GDir *gd = g_dir_open (dir, 0, NULL);
  if (!gd) { fprintf (stderr, "cannot open %s\n", dir); return 1; }

  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  const gchar *n;
  while ((n = g_dir_read_name (gd)))
    {
      if (!g_str_has_suffix (n, ".bin") || g_str_has_suffix (n, ".bin.bg"))
        continue;
      g_autofree gchar *bg = g_strdup_printf ("%s/%s.bg", dir, n);
      if (g_file_test (bg, G_FILE_TEST_EXISTS))
        g_ptr_array_add (names, g_strdup (n));
    }
  g_dir_close (gd);
  g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);

  guint cnt = names->len;
  GxSiftFeatures **f = g_new0 (GxSiftFeatures *, cnt);
  for (guint i = 0; i < cnt; i++)
    {
      g_autofree gchar *p = g_strdup_printf ("%s/%s", dir, (char *) g_ptr_array_index (names, i));
      double *img = load_diff (p);
      if (img) { prepare (img); f[i] = gx_sift_extract (img, IW, IH); }
      g_free (img);
    }

  printf ("%u captures, threshold %d, leave-one-out\n\n", cnt, threshold);
  printf ("%12s %7s %7s %8s %8s\n", "probe", "fused", "best", "npts", "pass");

  int pass = 0;
  for (guint i = 0; i < cnt; i++)
    {
      if (!f[i]) continue;
      g_autofree guint8 *seen = g_new0 (guint8, f[i]->n ? f[i]->n : 1);
      int best = 0;

      for (guint j = 0; j < cnt; j++)
        {
          if (j == i || !f[j]) continue;
          int s = gx_sift_match_mask (f[j], f[i], seen);
          if (s > best) best = s;
        }
      int fused = 0;
      for (guint k = 0; k < f[i]->n; k++)
        fused += seen[k] ? 1 : 0;

      gboolean ok = fused >= threshold;
      if (ok) pass++;
      printf ("%12s %7d %7d %8u %8s\n",
              (char *) g_ptr_array_index (names, i), fused, best, f[i]->n,
              ok ? "yes" : "NO");
    }

  printf ("\npassed %d/%u\n", pass, cnt);
  printf ("Note: a fused score counts probe points matched by ANY template view,\n");
  printf ("so it is not the same as the best single-view score next to it.\n");

  for (guint i = 0; i < cnt; i++)
    gx_sift_free (f[i]);
  g_free (f);
  g_ptr_array_free (names, TRUE);
  return 0;
}
