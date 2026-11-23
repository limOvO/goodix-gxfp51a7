// overlap-check — measures whether fingerprint captures share real content.
//
// Prints, for every pair, the best normalised cross-correlation of the ridge
// images over a search of shifts, plus the shift at which it occurs.
//
// Why this exists: a descriptor matcher can always produce *some*
// correspondences, so "it found no consistent pose" is ambiguous — it may be
// the matcher's fault, or the images may genuinely not overlap. Correlation
// answers the second question directly, with no descriptors involved.
//
// Reading it:
//   r >= 0.6            the ridges themselves line up: genuinely stitchable
//   0.25 - 0.6          partial overlap, or heavy elastic deformation
//   < 0.25              no shared content (the noise floor measured here was
//                       ~0.1-0.2 between unrelated presses)
//
// A best shift pinned at the edge of the search range is another way to see
// "no real peak": it means the search found the least-bad offset, not a match.
//
//   cc -O2 overlap-check.c -o overlap-check $(pkg-config --cflags --libs glib-2.0) -lm
//   ./overlap-check p001.bin p002.bin [--range 40] [--step 2]
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 132
#define H 112
#define BORDER 10              /* ignore a frame of resampling/sensor edge */

static guint16 *
load_u16 (const char *path, gsize *len)
{
  gchar *data = NULL;

  if (!g_file_get_contents (path, &data, len, NULL))
    return NULL;
  if (*len != sizeof (guint16) * W * H)
    {
      g_free (data);
      return NULL;
    }
  return (guint16 *) data;
}

/* background minus frame: the ridge signal the matcher sees */
static double *
load_ridge (const char *cap)
{
  gsize fl = 0, bl = 0;
  g_autofree gchar *bgpath = g_strconcat (cap, ".bg", NULL);
  g_autofree guint16 *f = load_u16 (cap, &fl);
  g_autofree guint16 *b = load_u16 (bgpath, &bl);
  double *d;

  if (!f || !b)
    return NULL;
  d = g_new (double, W * H);
  for (int i = 0; i < W * H; i++)
    d[i] = (double) b[i] - (double) f[i];
  return d;
}

/* Pearson correlation over the window where both images have signal. */
static double
corr_at (const double *a, const double *b, int dx, int dy, int *n_out)
{
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  int n = 0;

  for (int y = BORDER; y < H - BORDER; y++)
    for (int x = BORDER; x < W - BORDER; x++)
      {
        int y2 = y + dy, x2 = x + dx;

        if (y2 < BORDER || y2 >= H - BORDER || x2 < BORDER || x2 >= W - BORDER)
          continue;
        double va = a[y * W + x], vb = b[y2 * W + x2];

        /* Only compare where there is ridge signal in both, so a blank edge
         * cannot inflate the score. */
        if (va <= 0 || vb <= 0)
          continue;
        sa += va; sb += vb; saa += va * va; sbb += vb * vb; sab += va * vb;
        n++;
      }

  if (n_out)
    *n_out = n;
  if (n < 500)
    return -2.0;

  double ma = sa / n, mb = sb / n;
  double va = saa / n - ma * ma;
  double vb = sbb / n - mb * mb;

  if (va <= 1e-9 || vb <= 1e-9)
    return -2.0;
  return (sab / n - ma * mb) / sqrt (va * vb);
}

int
main (int argc, char **argv)
{
  int range = 40, step = 2;
  GPtrArray *caps = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *imgs = g_ptr_array_new_with_free_func (g_free);

  for (int a = 1; a < argc; a++)
    {
      if (!strcmp (argv[a], "--range") && a + 1 < argc) { range = atoi (argv[++a]); continue; }
      if (!strcmp (argv[a], "--step") && a + 1 < argc) { step = atoi (argv[++a]); continue; }
      if (argv[a][0] == '-')
        { fprintf (stderr, "unknown option %s\n", argv[a]); return 2; }

      double *d = load_ridge (argv[a]);

      if (!d)
        { fprintf (stderr, "skip %s (needs .bin and .bin.bg of %dx%d)\n",
                   argv[a], W, H); continue; }
      g_ptr_array_add (caps, g_strdup (argv[a]));
      g_ptr_array_add (imgs, d);
    }

  if (caps->len < 2)
    { fprintf (stderr, "need at least 2 readable captures\n"); return 1; }

  printf ("%u capture(s), search +-%d px step %d\n\n", caps->len, range, step);

  int npair = 0, good = 0, mid = 0, none = 0;
  double best_of_all = -2;

  for (guint i = 0; i < caps->len; i++)
    for (guint j = i + 1; j < caps->len; j++)
      {
        const double *a = g_ptr_array_index (imgs, i);
        const double *b = g_ptr_array_index (imgs, j);
        double best = -2;
        int bx = 0, by = 0;

        for (int dy = -range; dy <= range; dy += step)
          for (int dx = -range; dx <= range; dx += step)
            {
              int n;
              double r = corr_at (a, b, dx, dy, &n);

              if (r > best) { best = r; bx = dx; by = dy; }
            }

        const char *verdict = "no shared content";
        if (best >= 0.6)       { verdict = "STITCHABLE"; good++; }
        else if (best >= 0.25) { verdict = "partial"; mid++; }
        else                   { none++; }

        if (best > best_of_all)
          best_of_all = best;

        printf ("%-28s %-28s r=%+.3f  at %+3d,%+3d  %s\n",
                (const char *) g_ptr_array_index (caps, i),
                (const char *) g_ptr_array_index (caps, j),
                best, bx, by, verdict);
        npair++;
      }

  printf ("\n%d pair(s): %d stitchable (r>=0.60), %d partial (0.25-0.60), "
          "%d no shared content\n", npair, good, mid, none);
  printf ("best correlation seen: %+.3f\n", best_of_all);
  printf ("\nA synthetic control (the same capture against itself) scores "
          "r=1.000 at 0,0,\nso a run whose best pair is near 0.2 means the "
          "presses do not overlap,\nnot that the matcher is weak.\n");
  return (good > 0) ? 0 : 1;
}
