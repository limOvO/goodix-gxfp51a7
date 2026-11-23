/*
 * Does the rigid matcher raise the impostor floor?
 *
 *   ./impostor-check <genuine-dir> <impostor-dir>
 *
 * The change from a translation-only vote to a rigid fit recovers rotated
 * genuine presses, but a rotation is an extra degree of freedom and can fit
 * noise. The threshold in the driver was calibrated against impostor scores
 * under the OLD matcher, so this measures both matchers on the same data and
 * reports the distributions side by side.
 *
 * It links against the CURRENT goodix_sift.c, so run it once before the change
 * and once after, or use IMPOSTOR_LEGACY to score with the old algorithm.
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

/* Loads every capture in a directory that has a background. */
static GPtrArray *
load_dir (const char *dir, GArray **out_feat)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GArray *feat = g_array_new (FALSE, TRUE, sizeof (GxSiftFeatures *));

  if (!d) return names;

  const gchar *n;
  while ((n = g_dir_read_name (d)))
    {
      if (!g_str_has_suffix (n, ".bin") || g_str_has_suffix (n, ".bin.bg"))
        continue;
      g_autofree gchar *bg = g_strdup_printf ("%s/%s.bg", dir, n);
      if (!g_file_test (bg, G_FILE_TEST_EXISTS))
        continue;

      g_autofree gchar *p = g_strdup_printf ("%s/%s", dir, n);
      double *img = load_diff (p);
      if (!img) continue;
      prepare (img);
      GxSiftFeatures *f = gx_sift_extract (img, IW, IH);
      g_free (img);
      if (!f) continue;
      g_ptr_array_add (names, g_strdup (n));
      g_array_append_val (feat, f);
    }
  g_dir_close (d);
  *out_feat = feat;
  return names;
}

static int
by_int_desc (const void *a, const void *b)
{
  return *(const int *) b - *(const int *) a;
}

int
main (int argc, char **argv)
{
  if (argc < 3) { fprintf (stderr, "usage: %s <genuine-dir> <impostor-dir>\n", argv[0]); return 2; }

  GArray *gf = NULL, *if_ = NULL;
  GPtrArray *gn = load_dir (argv[1], &gf);
  GPtrArray *in = load_dir (argv[2], &if_);
  printf ("genuine set: %u captures   impostor set: %u captures\n\n", gn->len, in->len);

  /* All cross-set scores are impostor trials: different fingers, by
   * construction of the two directories. */
  GArray *imp = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < gn->len; i++)
    for (guint j = 0; j < in->len; j++)
      {
        GxSiftFeatures *x = g_array_index (gf, GxSiftFeatures *, i);
        GxSiftFeatures *y = g_array_index (if_, GxSiftFeatures *, j);
        int s = gx_sift_match (x, y);
        g_array_append_val (imp, s);
      }

  printf ("impostor trials: %u\n", imp->len);
  if (imp->len)
    {
      g_array_sort (imp, by_int_desc);
      int *v = (int *) imp->data;
      printf ("  max %d, p99 %d, p95 %d, median %d\n",
              v[0], v[imp->len / 100], v[imp->len / 20], v[imp->len / 2]);

      int buckets[] = { 0, 5, 10, 15, 20, 25, 30, 1000 };
      for (guint b = 0; b + 1 < G_N_ELEMENTS (buckets); b++)
        {
          int k = 0;
          for (guint t = 0; t < imp->len; t++)
            if (v[t] >= buckets[b] && v[t] < buckets[b + 1]) k++;
          printf ("  %3d-%-3d: %5d (%.2f%%)\n", buckets[b], buckets[b + 1] - 1,
                  k, 100.0 * k / imp->len);
        }
    }

  /* Genuine pairs within the genuine dir, for the same comparison. */
  printf ("\ngenuine pairs (same finger, different presses):\n");
  GArray *gen = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < gn->len; i++)
    for (guint j = i + 1; j < gn->len; j++)
      {
        GxSiftFeatures *x = g_array_index (gf, GxSiftFeatures *, i);
        GxSiftFeatures *y = g_array_index (gf, GxSiftFeatures *, j);
        int s = gx_sift_match (x, y);
        g_array_append_val (gen, s);
      }
  if (gen->len)
    {
      g_array_sort (gen, by_int_desc);
      int *v = (int *) gen->data;
      printf ("  %u pairs, max %d, p90 %d, median %d\n",
              gen->len, v[0], v[gen->len / 10], v[gen->len / 2]);
    }

  for (guint i = 0; i < gf->len; i++)
    gx_sift_free (g_array_index (gf, GxSiftFeatures *, i));
  for (guint i = 0; i < if_->len; i++)
    gx_sift_free (g_array_index (if_, GxSiftFeatures *, i));
  g_array_free (gf, TRUE);
  g_array_free (if_, TRUE);
  g_array_free (imp, TRUE);
  g_array_free (gen, TRUE);
  g_ptr_array_free (gn, TRUE);
  g_ptr_array_free (in, TRUE);
  return 0;
}
