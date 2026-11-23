/*
 * Rotation tolerance against a real multi-view template.
 *
 *   ./rot-template <dir> [threshold]
 *
 * For each capture: template = every OTHER capture, probe = this capture
 * rotated by 0..35 degrees. The score is computed the way the driver does it -
 * flag the probe's points matched by any template view, count the union - so a
 * number here means the same thing as "verify: attempt 1 -> N matches".
 *
 * This answers the question the synthetic self-rotation cannot: a template
 * already covering several positions is the realistic case, and rotation is how
 * one press stands in for many angles.
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

static double *
rotate_img (const double *src, double deg)
{
  if (deg == 0.0)
    return g_memdup2 (src, sizeof (double) * NN);

  double th = deg * M_PI / 180.0, c = cos (th), s = sin (th);
  double cx = (IW - 1) / 2.0, cy = (IH - 1) / 2.0;
  double *out = g_new0 (double, NN);
  double *tmp = g_memdup2 (src, sizeof (double) * NN);
  qsort (tmp, NN, sizeof (double), cmp_dbl);
  double median = tmp[NN / 2];
  g_free (tmp);

  for (int y = 0; y < IH; y++)
    for (int x = 0; x < IW; x++)
      {
        double dx = x - cx, dy = y - cy;
        double sx = c * dx + s * dy + cx, sy = -s * dx + c * dy + cy;
        int x0 = (int) floor (sx), y0 = (int) floor (sy);
        double fx = sx - x0, fy = sy - y0;
        if (x0 < 0 || y0 < 0 || x0 + 1 >= IW || y0 + 1 >= IH)
          { out[y * IW + x] = median; continue; }
        double v00 = src[y0 * IW + x0], v10 = src[y0 * IW + x0 + 1];
        double v01 = src[(y0 + 1) * IW + x0], v11 = src[(y0 + 1) * IW + x0 + 1];
        out[y * IW + x] = (v00 * (1 - fx) + v10 * fx) * (1 - fy)
                        + (v01 * (1 - fx) + v11 * fx) * fy;
      }
  return out;
}

int
main (int argc, char **argv)
{
  const char *dir = argc > 1 ? argv[1] : "/tmp/sess";
  int threshold = argc > 2 ? atoi (argv[2]) : 20;

  GDir *gd = g_dir_open (dir, 0, NULL);
  if (!gd) return 1;
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
  double **raw = g_new0 (double *, cnt);
  for (guint i = 0; i < cnt; i++)
    {
      g_autofree gchar *p = g_strdup_printf ("%s/%s", dir, (char *) g_ptr_array_index (names, i));
      raw[i] = load_diff (p);
      if (raw[i]) { prepare (raw[i]); f[i] = gx_sift_extract (raw[i], IW, IH); }
    }

  printf ("template = the other %u captures; probe = one capture rotated\n",
          cnt - 1);
  printf ("threshold %d\n\n", threshold);
  printf ("%12s", "probe");
  for (int d = 0; d <= 35; d += 5) printf ("%7d°", d);
  printf ("\n");

  int pass[8] = { 0 };
  int rows = 0;

  for (guint i = 0; i < cnt; i++)
    {
      if (!f[i] || !raw[i]) continue;
      printf ("%12s", (char *) g_ptr_array_index (names, i));
      rows++;

      for (int d = 0; d <= 35; d += 5)
        {
          double *rot = rotate_img (raw[i], (double) d);
          GxSiftFeatures *probe;
          int fused = 0;

          /* The template views were prepared already; only the probe needs
           * the pipeline. Rotating after prepare() keeps the two consistent,
           * which is what the driver does with a fresh capture. */
          prepare (rot);
          probe = gx_sift_extract (rot, IW, IH);
          if (probe)
            {
              g_autofree guint8 *seen = g_new0 (guint8, probe->n ? probe->n : 1);
              for (guint j = 0; j < cnt; j++)
                {
                  if (j == i || !f[j]) continue;
                  gx_sift_match_mask (f[j], probe, seen);
                }
              for (guint k = 0; k < probe->n; k++)
                fused += seen[k] ? 1 : 0;
              gx_sift_free (probe);
            }
          g_free (rot);

          printf ("%8d", fused);
          if (fused >= threshold) pass[d / 5]++;
        }
      printf ("\n");
    }

  printf ("\npass rate by angle (of %d probes):\n", rows);
  for (int d = 0; d <= 35; d += 5)
    printf ("  %2d°: %d/%d%s\n", d, pass[d / 5], rows,
            pass[d / 5] == rows ? "  all" : "");

  for (guint i = 0; i < cnt; i++)
    { gx_sift_free (f[i]); g_free (raw[i]); }
  g_free (f);
  g_free (raw);
  g_ptr_array_free (names, TRUE);
  return 0;
}
