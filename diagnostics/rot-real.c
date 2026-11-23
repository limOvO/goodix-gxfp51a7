/*
 * Rotation sensitivity, measured correctly.
 *
 *   ./rot-real <dir> [p0NN]
 *
 * Earlier attempts here decoded the saved captures as packed 12-bit. They are
 * not: the recorder writes already-decoded 16-bit samples, 132*112 of them,
 * 29568 bytes per file. Feeding 12-bit unpacking to 16-bit data produces
 * self-consistent but meaningless features, which is how a bogus conclusion
 * ("the descriptor is not rotation-invariant at all") got established.
 *
 * This loader matches diagnostics/stitch-captures.c exactly, since that is the
 * code that demonstrably stitched real presses of one finger.
 *
 * Two experiments:
 *
 *   A. synthetic  - rotate one capture by 0..40 deg and score it against itself.
 *                   Isolates the descriptor from finger-to-finger variation.
 *
 *   B. real pairs - every pair of recorded presses, reporting how many
 *                   correspondences survive the ratio test (np) and how many
 *                   agree under a rigid transform (inl). This is what "same
 *                   area, different angle" actually looks like.
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
#define RATIO2 (0.95 * 0.95)
#define INLIER_PX 3.0

static int cmp_dbl (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return (x > y) - (x < y);
}

/* Same as stitch-captures.c: 16-bit samples, background minus finger. */
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

/* Same as stitch-captures.c prepare(): per-row and per-column median removal,
 * no global rescale. */
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

static double desc_dist2 (const guint8 *a, const guint8 *b)
{
  double s = 0;
  for (int i = 0; i < GX_SIFT_DIM; i++)
    { double d = (double) a[i] - (double) b[i]; s += d * d; }
  return s;
}

typedef struct { int i, j; } Pair;

static int
desc_pairs (const GxSiftFeatures *A, const GxSiftFeatures *B, Pair *out, int cap)
{
  int np = 0;
  for (guint i = 0; i < A->n && np < cap; i++)
    {
      double d1 = 1e300, d2 = 1e300;
      int jb = -1;
      for (guint j = 0; j < B->n; j++)
        {
          double d = desc_dist2 (A->pts[i].desc, B->pts[j].desc);
          if (d < d1) { d2 = d1; d1 = d; jb = (int) j; }
          else if (d < d2) d2 = d;
        }
      if (jb >= 0 && d1 < RATIO2 * d2)
        { out[np].i = i; out[np].j = jb; np++; }
    }
  return np;
}

/* Rigid RANSAC over pairs of correspondences. Reports inliers and the angle. */
static int
rigid_best (const GxSiftFeatures *A, const GxSiftFeatures *B,
            const Pair *p, int np, double *out_deg)
{
  int best = 0;
  double best_th = 0;
  int tried = 0;

  if (np < 2) return 0;

  for (int a = 0; a < np && tried < 3000; a++)
    for (int b = a + 1; b < np && tried < 3000; b++)
      {
        double ax = A->pts[p[a].i].x, ay = A->pts[p[a].i].y;
        double bx = A->pts[p[b].i].x, by = A->pts[p[b].i].y;
        double apx = B->pts[p[a].j].x, apy = B->pts[p[a].j].y;
        double bpx = B->pts[p[b].j].x, bpy = B->pts[p[b].j].y;
        if (hypot (bx - ax, by - ay) < 6.0 || hypot (bpx - apx, bpy - apy) < 6.0)
          continue;
        tried++;

        double th = atan2 (bpy - apy, bpx - apx) - atan2 (by - ay, bx - ax);
        while (th > M_PI) th -= 2 * M_PI;
        while (th <= -M_PI) th += 2 * M_PI;
        double c = cos (th), s = sin (th);
        double tx = apx - (c * ax - s * ay), ty = apy - (s * ax + c * ay);

        int cnt = 0;
        for (int j = 0; j < np; j++)
          {
            double px = c * A->pts[p[j].i].x - s * A->pts[p[j].i].y + tx;
            double py = s * A->pts[p[j].i].x + c * A->pts[p[j].i].y + ty;
            double dx = px - B->pts[p[j].j].x, dy = py - B->pts[p[j].j].y;
            if (dx * dx + dy * dy <= INLIER_PX * INLIER_PX) cnt++;
          }
        if (cnt > best) { best = cnt; best_th = th; }
      }

  if (out_deg) *out_deg = best_th * 180.0 / M_PI;
  return best;
}

int
main (int argc, char **argv)
{
  const char *dir = argc > 1 ? argv[1] : "/tmp/sess";
  const char *one = argc > 2 ? argv[2] : NULL;

  /* --- A. synthetic rotation of one capture ---------------------------- */
  {
    g_autofree gchar *cap = g_strdup_printf ("%s/%s.bin", dir, one ? one : "p001");
    double *img = load_diff (cap);
    if (!img) { fprintf (stderr, "cannot load %s\n", cap); return 1; }
    prepare (img);

    GxSiftFeatures *ref = gx_sift_extract (img, IW, IH);
    printf ("A. rotate one capture against itself (%s, %u keypoints)\n",
            one ? one : "p001", ref->n);
    printf ("%7s %7s %8s %10s %10s\n", "angle", "npts", "plain", "rigid-inl", "rigid-deg");

    Pair *pairs = g_new (Pair, 4000);
    for (int deg = 0; deg <= 40; deg += 5)
      {
        double *rot = rotate_img (img, deg);
        prepare (rot);
        GxSiftFeatures *f = gx_sift_extract (rot, IW, IH);
        int plain = gx_sift_match (ref, f);
        int np = f ? desc_pairs (ref, f, pairs, 4000) : 0;
        double rdeg = 0;
        int inl = f ? rigid_best (ref, f, pairs, np, &rdeg) : 0;
        printf ("%5d° %7u %8d %3d/%-3d %9.1f°\n",
                deg, f ? f->n : 0, plain, inl, np, rdeg);
        gx_sift_free (f);
        g_free (rot);
      }
    g_free (pairs);
    gx_sift_free (ref);
    g_free (img);
  }

  /* --- B. every real pair --------------------------------------------- */
  GDir *d = g_dir_open (dir, 0, NULL);
  if (!d) return 0;
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  const gchar *n;
  while ((n = g_dir_read_name (d)))
    {
      if (!g_str_has_suffix (n, ".bin") || g_str_has_suffix (n, ".bin.bg"))
        continue;
      g_autofree gchar *bg = g_strdup_printf ("%s/%s.bg", dir, n);
      if (g_file_test (bg, G_FILE_TEST_EXISTS))
        g_ptr_array_add (names, g_strdup (n));
    }
  g_dir_close (d);
  g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);

  guint cnt = names->len;
  GxSiftFeatures **feat = g_new0 (GxSiftFeatures *, cnt);
  for (guint i = 0; i < cnt; i++)
    {
      g_autofree gchar *p = g_strdup_printf ("%s/%s", dir, (char *) g_ptr_array_index (names, i));
      double *img = load_diff (p);
      if (img) { prepare (img); feat[i] = gx_sift_extract (img, IW, IH); }
      g_free (img);
    }

  printf ("\nB. every real pair of %u presses\n", cnt);
  printf ("%10s %10s %6s %6s %8s %8s\n",
          "A", "B", "np", "inl", "rigid-deg", "plain");

  Pair *pairs = g_new (Pair, 4000);
  int hist_inl[21] = { 0 };
  int hist_rot[7] = { 0 };        /* buckets of 5 deg up to 30+ */
  int n_rows = 0;

  for (guint i = 0; i < cnt; i++)
    for (guint j = i + 1; j < cnt; j++)
      {
        if (!feat[i] || !feat[j]) continue;
        int np = desc_pairs (feat[i], feat[j], pairs, 4000);
        if (np < 2) continue;
        double rdeg = 0;
        int inl = rigid_best (feat[i], feat[j], pairs, np, &rdeg);
        n_rows++;

        hist_inl[MIN (inl, 20)]++;
        int b = (int) (fabs (rdeg) / 5.0);
        hist_rot[MIN (b, 6)]++;

        if (inl >= 5)
          printf ("%10s %10s %6d %6d %7.1f° %8d\n",
                  (char *) g_ptr_array_index (names, i),
                  (char *) g_ptr_array_index (names, j),
                  np, inl, rdeg, gx_sift_match (feat[i], feat[j]));
      }

  printf ("\n  %d pairs examined; rigid inliers distribution:\n", n_rows);
  for (int k = 0; k <= 20; k++)
    if (hist_inl[k])
      printf ("    %2d inliers: %d%s\n", k, hist_inl[k], k == 20 ? "+" : "");

  printf ("\n  rotation of the best hypothesis:\n");
  for (int k = 0; k < 7; k++)
    if (hist_rot[k])
      printf ("    %2d-%2d°: %d pairs\n", k * 5, k * 5 + 4, hist_rot[k]);

  g_free (pairs);
  for (guint i = 0; i < cnt; i++)
    gx_sift_free (feat[i]);
  g_free (feat);
  g_ptr_array_free (names, TRUE);
  return 0;
}
