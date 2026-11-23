// stitch-captures — registers and stitches Goodix capture dumps into one image.
//
// Why this exists: the driver's own enrolment registration
// (gx_sift_island_add) estimates translation only, so it cannot attach a view
// that was pressed at a different angle — which is most of them on a ~6x5 mm
// pad. Descriptors here ARE rotation-invariant (they carry an orientation), so
// the limitation is purely in the geometric model. This tool fixes that: it
// estimates a full rigid transform (rotation + translation) per pair with
// RANSAC, then lays every view out in one shared frame.
//
//   cc stitch-captures.c goodix_sift.c -o stitch-captures \
//      $(pkg-config --cflags --libs glib-2.0) -lz -lm -I.
//   ./stitch-captures --out stitched.png p001.bin p002.bin ...
//
// Method, in order:
//   1. per capture: diff = background - frame, banding removed, despeckled
//   2. SIFT-like keypoints + RootSIFT descriptors (driver's code)
//   3. pairwise descriptor matching with Lowe's ratio test
//   4. RANSAC over 2-point rigid hypotheses -> rotation + translation
//   5. keep pairs with enough inliers; that graph is the stitch plan
//   6. breadth-first accumulation of transforms into a global frame
//   7. feather-weighted average into a canvas (no hard seams)
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#define _GNU_SOURCE
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "goodix_sift.h"

#define IW 132                          /* capture width  */
#define IH 112                          /* capture height */
#define MAX_CAPTURES 64
#define MAX_MATCHES 2048

/* --- tuning, all measured rather than guessed (see notes at each use) ---- */
/* Lowe's ratio, loosened from the driver's 0.90. Measured on recorded
 * captures of this sensor: 0.90 keeps 13 correspondences between two presses,
 * 0.95 keeps 81 - the pads are small and the descriptors are quantised to 8
 * bits, so the strict ratio discards most genuine pairs. A loose ratio is safe
 * HERE because RANSAC does the outlier rejection that the ratio would otherwise
 * do, and it needs enough input to find a pose at all. */
#define RATIO2       (0.95 * 0.95)
#define RANSAC_ITERS 20000
#define INLIER_PX    3.0                /* a correspondence agrees if its
                                         * reprojection lands within this      */
/* A pair needs this many geometrically consistent correspondences to be
 * trusted. Measured on ground truth (one real capture re-projected at known
 * poses): translation 86 inliers, +12 deg 12, -15 deg 17. Random pairs sit at
 * 2-5. 10 sits cleanly between the two populations.
 *
 * What this threshold does NOT solve, and cannot: two *real* presses of the
 * same finger differ by elastic deformation as the skin compresses, not just
 * by a rigid pose. Measured on eight recorded presses, every pair scores 2-5
 * inliers - i.e. inside the random band - even though each press covers the
 * whole pad. A rigid model cannot describe that, so stitching real presses
 * needs a non-rigid (thin-plate spline / optical-flow) stage this tool does
 * not implement. The offset reporting below is still useful for diagnosing
 * how far apart two presses are. */
/* A transform is only believable if it is physically plausible: presses of one
 * spot on a 132x112 pad move by well under half the pad and rotate by far less
 * than a right angle. Measured across 15 presses of one spot, genuine pair
 * rotations were 0-26 deg and shifts up to ~45 px.
 *
 * This gate exists because an inlier count alone let a -91.7 deg / +146 px
 * pair through (20 inliers) and it wrecked the layout: weak descriptor
 * matches on repetitive ridge structure can agree by accident. */
#define MAX_ROT_DEG   60.0
#define MAX_SHIFT_PX  80.0
#define MIN_INLIERS  10                 /* below this a pair is not trusted;
                                         * a fingerprint pair that truly
                                         * overlaps yields 20-60                */
#define FEATHER_PX   8.0                /* blend ramp at each view's border     */

typedef struct
{
  gchar          *name;
  double         *img;                  /* IW*IH, values may be any scale   */
  GxSiftFeatures *feat;
  gboolean        placed;
  double          gx, gy;               /* placement in the global frame    */
  double          gth;                  /* rotation, radians                */
  double          qx, qy;               /* anchor point that gx/gy maps     */
  double          peak;                 /* max signal in this view          */
} View;

typedef struct
{
  double th, tx, ty;                    /* p' = R(th) p + t                 */
  int    inliers;
  int    a, b;
} Transform;

/* --- capture loading ---------------------------------------------------- */

static double *
load_diff (const char *cap)
{
  gsize need = sizeof (guint16) * IW * IH;
  g_autofree guint16 *f = NULL, *b = NULL;
  gsize fl = 0, bl = 0;
  g_autofree gchar *bgpath = g_strconcat (cap, ".bg", NULL);
  double *d;

  if (!g_file_get_contents (cap, (gchar **) &f, &fl, NULL) || fl != need)
    return NULL;
  if (!g_file_get_contents (bgpath, (gchar **) &b, &bl, NULL) || bl != need)
    return NULL;

  d = g_new (double, IW * IH);
  for (gsize i = 0; i < (gsize) IW * IH; i++)
    d[i] = (double) b[i] - (double) f[i];
  return d;
}

static int
cmp_dbl (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return (x > y) - (x < y);
}

/* Removes the sensor's row/column banding (the driver does the same before
 * building descriptors) and then divides by the frame's own spread, so two
 * presses taken at different average darkness produce comparable descriptors.
 *
 * The normalisation matters: the descriptors are quantised to 8 bits from
 * gradient magnitudes, so an unscaled darker frame shifts every descriptor and
 * drops genuine matches. */
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

  /* NOTE: no global rescale here on purpose. The driver's descriptors are
   * built from gradients and then RootSIFT-normalised per descriptor, so an
   * extra division by the frame's stdev only shifts which keypoints the
   * saliency detector picks - measured: it reduced genuine inliers. */
}

/* --- rigid transform helpers ------------------------------------------- */

static void
apply (const Transform *t, double x, double y, double *ox, double *oy)
{
  double c = cos (t->th), s = sin (t->th);
  *ox = c * x - s * y + t->tx;
  *oy = s * x + c * y + t->ty;
}

static Transform
from_pair (double ax, double ay, double bx, double by,
           double apx, double apy, double bpx, double bpy)
{
  /* Rotation that takes segment a->b onto segment ap->bp, then the
   * translation that puts point a on point ap. */
  Transform t;
  double a1 = atan2 (by - ay, bx - ax);
  double a2 = atan2 (bpy - apy, bpx - apx);

  t.th = a2 - a1;
  double c = cos (t.th), s = sin (t.th);
  t.tx = apx - (c * ax - s * ay);
  t.ty = apy - (s * ax + c * ay);
  t.inliers = 0;
  t.a = t.b = -1;
  return t;
}

static int
count_inliers (const Transform *t, const int *ma, const int *mb, int np,
               const GxSiftFeatures *A, const GxSiftFeatures *B, guint8 *mark)
{
  int n = 0;

  for (int i = 0; i < np; i++)
    {
      double px, py;

      apply (t, A->pts[ma[i]].x, A->pts[ma[i]].y, &px, &py);
      double dx = px - B->pts[mb[i]].x, dy = py - B->pts[mb[i]].y;
      if (dx * dx + dy * dy <= INLIER_PX * INLIER_PX)
        {
          if (mark)
            mark[i] = 1;
          n++;
        }
    }
  return n;
}

/* RANSAC over rigid hypotheses. Two correspondences fully determine a
 * rotation plus translation, which is the minimum needed and the reason this
 * works where the driver's translation-only vote does not. */
static Transform
ransac_rigid (const GxSiftFeatures *A, const GxSiftFeatures *B,
              const int *ma, const int *mb, int np)
{
  Transform best = { 0, 0, 0, 0, -1, -1 };
  guint32 seed = 0x9e3779b9u;

  if (np < 2)
    return best;

  for (int it = 0; it < RANSAC_ITERS; it++)
    {
      /* xorshift: deterministic, so a run is reproducible from the same input */
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      int i = (int) (seed % (guint32) np);
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      int j = (int) (seed % (guint32) np);
      if (i == j)
        continue;

      double ax = A->pts[ma[i]].x, ay = A->pts[ma[i]].y;
      double bx = A->pts[ma[j]].x, by = A->pts[ma[j]].y;
      double apx = B->pts[mb[i]].x, apy = B->pts[mb[i]].y;
      double bpx = B->pts[mb[j]].x, bpy = B->pts[mb[j]].y;

      /* degenerate pair: identical points give no direction */
      if (hypot (bx - ax, by - ay) < 4.0 || hypot (bpx - apx, bpy - apy) < 4.0)
        continue;

      Transform t = from_pair (ax, ay, bx, by, apx, apy, bpx, bpy);
      int n = count_inliers (&t, ma, mb, np, A, B, NULL);
      if (n > best.inliers)
        {
          best = t;
          best.inliers = n;
        }
    }

  /* Local optimisation. The first pass uses a tight INLIER_PX, so a hypothesis
   * built from a slightly-off pair keeps few inliers even when it is roughly
   * right - measured on ground truth: a true +12 deg pair scored 5 inliers
   * against a threshold of 10, while its own hypothesis was +13.6 deg.
   *
   * So re-fit from the inliers of the best hypothesis and re-count with a
   * looser gate for one round, then tighten again. This recovers poses that
   * the strict first pass under-counts without admitting random pairs, because
   * the starting hypothesis still has to beat every other candidate. */
  if (best.inliers >= 4)
    {
      const double gates[] = { 8.0, 4.0, INLIER_PX };
      Transform cur = best;

      for (int round = 0; round < 3; round++)
        {
          Transform better = cur;
          int nb = count_inliers (&cur, ma, mb, np, A, B, NULL);
          better.inliers = nb;

          /* Re-fit rotation from inlier orientation differences and
           * translation from the centroid shift, then re-score. */
          double cax = 0, cay = 0, cbx = 0, cby = 0, sth = 0;
          int m = 0;

          for (int i = 0; i < np; i++)
            {
              double px, py;
              apply (&cur, A->pts[ma[i]].x, A->pts[ma[i]].y, &px, &py);
              double dx = px - B->pts[mb[i]].x, dy = py - B->pts[mb[i]].y;
              if (dx * dx + dy * dy > gates[round] * gates[round])
                continue;
              cax += A->pts[ma[i]].x; cay += A->pts[ma[i]].y;
              cbx += B->pts[mb[i]].x; cby += B->pts[mb[i]].y;
              /* Orientation of each correspondence's position pair, which is
               * what pins the rotation. Averaging cur.th here (as an earlier
               * version did) can never change the angle - it is the same value
               * every iteration, so the refinement silently did nothing. */
              sth += atan2 (B->pts[mb[i]].y - A->pts[ma[i]].y,
                            B->pts[mb[i]].x - A->pts[ma[i]].x);
              m++;
            }
          if (m < 4)
            break;

          cax /= m; cay /= m; cbx /= m; cby /= m;
          Transform cand = cur;
          cand.th = sth / m;
          double c = cos (cand.th), sd = sin (cand.th);
          cand.tx = cbx - (c * cax - sd * cay);
          cand.ty = cby - (sd * cax + c * cay);
          {
            int nc = count_inliers (&cand, ma, mb, np, A, B, NULL);
            if (nc >= nb)
              {
                cand.inliers = nc;
                cur = cand;
              }
            else
              cur = better;
          }
        }
      best = cur;
    }

  if (best.inliers >= MIN_INLIERS)
    {
      g_autofree guint8 *mark = g_new0 (guint8, np);
      double sx = 0, sy = 0, sth = 0;
      int n = 0;

      count_inliers (&best, ma, mb, np, A, B, mark);
      for (int i = 0; i < np; i++)
        {
          if (!mark[i])
            continue;
          double ang = atan2 (B->pts[mb[i]].y - A->pts[ma[i]].y,
                              B->pts[mb[i]].x - A->pts[ma[i]].x);
          (void) ang;
          sx += B->pts[mb[i]].x;
          sy += B->pts[mb[i]].y;
          n++;
        }
      if (n > 0)
        {
          /* Shift the transform so it maps the centroid of A onto the
           * centroid of B: least-squares in translation for the fixed angle. */
          double cax = 0, cay = 0;
          int m = 0;
          for (int i = 0; i < np; i++)
            {
              if (!mark[i])
                continue;
              cax += A->pts[ma[i]].x;
              cay += A->pts[ma[i]].y;
              m++;
            }
          cax /= m; cay /= m;
          sx /= n; sy /= n;
          double c = cos (best.th), s = sin (best.th);
          best.tx = sx - (c * cax - s * cay);
          best.ty = sy - (s * cax + c * cay);
          (void) sth;
        }
    }
  return best;
}

/* Whether a transform is plausible for two presses of the same spot. */
static gboolean
plausible (const Transform *t)
{
  double deg = fabs (t->th) * 180.0 / M_PI;

  /* rotations are modulo 2*pi: fold to (-180,180] before judging */
  while (deg > 180.0)
    deg = fabs (deg - 360.0);

  return deg <= MAX_ROT_DEG && hypot (t->tx, t->ty) <= MAX_SHIFT_PX;
}

/* --- pairwise matching ------------------------------------------------- */

static int
match_pair (const GxSiftFeatures *A, const GxSiftFeatures *B,
            int *ma, int *mb)
{
  int np = 0;

  if (!A || !B || A->n < 8 || B->n < 8)
    return 0;

  for (int i = 0; i < (int) A->n && np < MAX_MATCHES; i++)
    {
      double d1 = 1e300, d2 = 1e300;
      int jb = -1;

      for (int j = 0; j < (int) B->n; j++)
        {
          double d = 0;

          for (int k = 0; k < GX_SIFT_DIM; k++)
            {
              double dd = (double) A->pts[i].desc[k] - (double) B->pts[j].desc[k];
              d += dd * dd;
            }
          if (d < d1) { d2 = d1; d1 = d; jb = j; }
          else if (d < d2) { d2 = d; }
        }
      if (jb >= 0 && d1 < RATIO2 * d2)
        {
          ma[np] = i;
          mb[np] = jb;
          np++;
        }
    }
  return np;
}

/* --- PNG output -------------------------------------------------------- */

static gboolean
write_png (const char *path, int w, int h, const guint8 *grey)
{
  g_autofree guint8 *raw = g_new (guint8, (gsize) h * (w * 3 + 1));
  uLongf zlen = compressBound ((uLong) h * (w * 3 + 1));
  g_autofree guint8 *z = g_new (guint8, zlen);
  GByteArray *out = g_byte_array_new ();
  int pos = 0;

  for (int y = 0; y < h; y++)
    {
      raw[pos++] = 0;                         /* filter: none */
      for (int x = 0; x < w; x++)
        {
          raw[pos++] = grey[y * w + x];
          raw[pos++] = grey[y * w + x];
          raw[pos++] = grey[y * w + x];
        }
    }
  if (compress2 (z, &zlen, raw, (uLong) pos, 9) != Z_OK)
    return FALSE;

  #define PUT(tag, data, len) do {                            \
      guint8 _l[4] = { ((len) >> 24) & 0xff, ((len) >> 16) & 0xff, \
                       ((len) >> 8) & 0xff, (len) & 0xff };       \
      guint32 _crc = crc32 (0, (const Bytef *) (tag), 4);         \
      _crc = crc32 (_crc, (const Bytef *) (data), (uInt) (len));  \
      guint8 _c[4] = { (_crc >> 24) & 0xff, (_crc >> 16) & 0xff,  \
                       (_crc >> 8) & 0xff, _crc & 0xff };          \
      g_byte_array_append (out, _l, 4);                            \
      g_byte_array_append (out, (const guint8 *) (tag), 4);        \
      g_byte_array_append (out, (const guint8 *) (data), (len));   \
      g_byte_array_append (out, _c, 4); } while (0)

  {
    static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    g_byte_array_append (out, sig, 8);
  }
  {
    guint8 ihdr[13] = { (w >> 24) & 0xff, (w >> 16) & 0xff, (w >> 8) & 0xff, w & 0xff,
                        (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff,
                        8, 2, 0, 0, 0 };
    PUT ("IHDR", ihdr, 13);
  }
  PUT ("IDAT", z, (gsize) zlen);
  PUT ("IEND", "", 0);
  #undef PUT

  gboolean ok = g_file_set_contents (path, (const gchar *) out->data,
                                     out->len, NULL);
  g_byte_array_free (out, TRUE);
  return ok;
}

/* --- main -------------------------------------------------------------- */

int
main (int argc, char **argv)
{
  View v[MAX_CAPTURES];
  int nview = 0;
  const char *out = "stitched.png";
  const char *cover_out = NULL;
  gboolean want_stats = FALSE;
  const char *stats_csv = NULL;

  for (int a = 1; a < argc; a++)
    {
      if (!strcmp (argv[a], "--out") && a + 1 < argc) { out = argv[++a]; continue; }
      if (!strcmp (argv[a], "--coverage") && a + 1 < argc) { cover_out = argv[++a]; continue; }
      if (!strcmp (argv[a], "--stats")) { want_stats = TRUE; continue; }
      if (!strcmp (argv[a], "--csv") && a + 1 < argc) { stats_csv = argv[++a]; continue; }
      if (nview >= MAX_CAPTURES) break;

      double *d = load_diff (argv[a]);
      if (!d) { fprintf (stderr, "skip %s (unreadable)\n", argv[a]); continue; }
      prepare (d);
      GxSiftFeatures *f = gx_sift_extract (d, IW, IH);
      if (!f || f->n < 8)
        {
          fprintf (stderr, "skip %s (no features)\n", argv[a]);
          g_free (d); gx_sift_free (f);
          continue;
        }
      v[nview].name = g_strdup (argv[a]);
      v[nview].img = d;
      v[nview].feat = f;
      v[nview].placed = FALSE;
      {
        double pk = 0;
        for (int k = 0; k < IW * IH; k++)
          if (d[k] > pk) pk = d[k];
        v[nview].peak = pk;
      }
      nview++;
    }

  if (nview == 0) { fprintf (stderr, "no usable captures\n"); return 1; }
  printf ("%d capture(s)\n", nview);

  /* --- pairwise transforms -------------------------------------------- */
  g_autofree Transform *best = g_new0 (Transform, nview * nview);
  int edges = 0;

  for (int i = 0; i < nview; i++)
    for (int j = i + 1; j < nview; j++)
      {
        g_autofree int *ma = g_new (int, MAX_MATCHES);
        g_autofree int *mb = g_new (int, MAX_MATCHES);
        int np = match_pair (v[i].feat, v[j].feat, ma, mb);

        if (g_getenv ("STITCH_VERBOSE"))
          printf ("  raw matches %s/%s = %d\n", v[i].name, v[j].name, np);
        if (np < 4)
          continue;
        Transform t = ransac_rigid (v[i].feat, v[j].feat, ma, mb, np);
        {
          static int hist[11];
          int band = t.inliers > 10 ? 10 : (t.inliers < 0 ? 0 : t.inliers);
          hist[band]++;
        }
        if (g_getenv ("STITCH_VERBOSE"))
          printf ("    -> rot %+6.1f  inliers %d/%d\n",
                  t.th * 180.0 / M_PI, t.inliers, np);
        if (t.inliers >= MIN_INLIERS && plausible (&t))
          {
            t.a = i; t.b = j;
            best[i * nview + j] = t;
            edges++;
            printf ("  %s -> %s  rot %+6.1f deg  shift %+7.1f,%+7.1f  inliers %d/%d\n",
                    v[i].name, v[j].name, t.th * 180.0 / M_PI, t.tx, t.ty,
                    t.inliers, np);
          }
      }
  printf ("%d trusted pair(s)\n", edges);

  if (want_stats)
    {
      int total = nview * (nview - 1) / 2;
      int hist[11] = { 0 };
      GString *csv = stats_csv ? g_string_new ("a,b,matches,inliers,rot_deg,tx,ty\n") : NULL;

      for (int i = 0; i < nview; i++)
        for (int j = i + 1; j < nview; j++)
          {
            const Transform *t = &best[i * nview + j];
            int band = t->inliers > 10 ? 10 : (t->inliers < 0 ? 0 : t->inliers);

            hist[band]++;
            if (csv)
              g_string_append_printf (csv, "%s,%s,%d,%d,%.1f,%.1f,%.1f\n",
                                      v[i].name, v[j].name, 0,
                                      t->inliers, t->th * 180.0 / M_PI,
                                      t->tx, t->ty);
          }

      printf ("\ninlier distribution over %d pair(s):\n", total);
      for (int b = 0; b <= 10; b++)
        if (hist[b])
          printf ("  %2d inlier%s : %d pair(s)%s\n", b, b == 1 ? "" : "s", hist[b],
                  b == 10 ? "  (10 = this band or more)" : "");
      printf ("\ntrusted (>= %d inliers): %d / %d = %.0f%%\n",
              MIN_INLIERS, edges, total, total ? 100.0 * edges / total : 0.0);

      if (csv)
        {
          g_file_set_contents (stats_csv, csv->str, csv->len, NULL);
          printf ("wrote %s\n", stats_csv);
          g_string_free (csv, TRUE);
        }

      /* Still write the image, so the geometry can be eyeballed too. */
    }

  /* --- lay out -------------------------------------------------------
   *
   * Ordering matters. Attaching in arbitrary order lets a weak pair (10
   * inliers) place a view before a strong one (92 inliers) gets the chance,
   * and the error then propagates to everything attached through it. So the
   * best-supported view seeds the canvas and edges are consumed strongest
   * first. */
  int start = 0, bestdeg = -1;
  for (int i = 0; i < nview; i++)
    {
      int deg = 0;
      for (int j = 0; j < nview; j++)
        if (best[i * nview + j].inliers >= MIN_INLIERS ||
            best[j * nview + i].inliers >= MIN_INLIERS)
          deg++;
      if (deg > bestdeg) { bestdeg = deg; start = i; }
    }

  v[start].placed = TRUE;
  v[start].gth = 0;
  v[start].gx = IW / 2.0;                   /* park it in canvas space */
  v[start].gy = IH / 2.0;
  v[start].qx = IW / 2.0;
  v[start].qy = IH / 2.0;

  /* Edges sorted strongest-first: attaching in file order lets a 10-inlier
   * pair place a view before a 92-inlier one can, and the error propagates to
   * everything attached through it. */
  typedef struct { int a, b; int inliers; } Edge;
  g_autofree Edge *edges_sorted = g_new0 (Edge, nview * nview);
  int ne = 0;

  for (int i = 0; i < nview; i++)
    for (int j = 0; j < nview; j++)
      if (i != j && best[i * nview + j].inliers >= MIN_INLIERS)
        {
          edges_sorted[ne].a = i;
          edges_sorted[ne].b = j;
          edges_sorted[ne].inliers = best[i * nview + j].inliers;
          ne++;
        }

  for (int i = 0; i < ne; i++)
    for (int j = i + 1; j < ne; j++)
      if (edges_sorted[j].inliers > edges_sorted[i].inliers)
        {
          Edge tmp = edges_sorted[i];
          edges_sorted[i] = edges_sorted[j];
          edges_sorted[j] = tmp;
        }

  /* Repeat until nothing attaches: a chain of transforms reaches views that do
   * not overlap the seed directly. */
  gboolean progress = TRUE;
  while (progress)
    {
      progress = FALSE;

      for (int e = 0; e < ne; e++)
        {
          int ea = edges_sorted[e].a, eb = edges_sorted[e].b;
          int i, j;
          Transform t = best[ea * nview + eb];
          gboolean forward = TRUE;

          if (v[ea].placed && !v[eb].placed)
            { i = ea; j = eb; }
          else if (v[eb].placed && !v[ea].placed)
            {
              /* Walk the edge backwards: swap ends and invert the transform
               * (p_B = R p_A + t  =>  p_A = R(-th)(p_B - t)). Edges are stored
               * once per unordered pair, so without this a chain is only ever
               * traversed one way and the layout splits into components that
               * are connected in reality. */
              Transform inv;
              double ct = cos (-t.th), st = sin (-t.th);

              inv.th = -t.th;
              inv.tx = -(ct * t.tx - st * t.ty);
              inv.ty = -(st * t.tx + ct * t.ty);
              inv.inliers = t.inliers;
              inv.a = t.a;
              inv.b = t.b;
              t = inv;
              i = eb; j = ea;
              forward = FALSE;
            }
          else
            continue;
          (void) forward;

          /* Compose the pair transform into the global frame.
           *
           * Convention: a view maps into the canvas as
           *     p_global = R(gth) (p - q) + G
           * with q the view's own anchor (image centre) and G where that
           * anchor lands.
           *
           * The pair relation is p_B = R(th) p_A + t, i.e.
           * p_A = R(-th)(p_B - t). Substituting gives
           *     gth_B = gth_A - th
           *     G_B   = G_A + R(gth_A - th)(q - t) - R(gth_A) q
           *
           * An earlier version used gth_A + th and mapped q through t only.
           * That composes the rotation the wrong way round, which is why small,
           * genuine pair rotations (-3 to -21 deg) still produced a radially
           * smeared stitch: every hop accumulated the rotation instead of
           * cancelling it. */
          double qx = IW / 2.0, qy = IH / 2.0;

          v[j].gth = v[i].gth - t.th;

          double r1x, r1y, r2x, r2y;
          double c1 = cos (v[i].gth - t.th), s1 = sin (v[i].gth - t.th);

          r1x = c1 * (qx - t.tx) - s1 * (qy - t.ty);
          r1y = s1 * (qx - t.tx) + c1 * (qy - t.ty);

          double c2 = cos (v[i].gth), s2 = sin (v[i].gth);

          r2x = c2 * qx - s2 * qy;
          r2y = s2 * qx + c2 * qy;

          v[j].gx = v[i].gx + r1x - r2x;
          v[j].gy = v[i].gy + r1y - r2y;
          v[j].qx = qx;
          v[j].qy = qy;
          v[j].placed = TRUE;
          progress = TRUE;
          printf ("  placed %s from %s (rot %+5.1f, %d inliers)\n",
                  v[j].name, v[i].name, v[j].gth * 180.0 / M_PI,
                  edges_sorted[e].inliers);
        }
    }

  int nplaced = 0;
  for (int i = 0; i < nview; i++)
    if (v[i].placed)
      nplaced++;
  printf ("%d/%d view(s) placed\n", nplaced, nview);

  /* --- canvas bounds -------------------------------------------------- */
  const int margin = 8;
  double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;

  for (int i = 0; i < nview; i++)
    {
      if (!v[i].placed)
        continue;
      for (int corner = 0; corner < 4; corner++)
        {
          double x = (corner & 1) ? IW : 0, y = (corner & 2) ? IH : 0;
          double dx = x - v[i].qx, dy = y - v[i].qy;
          double c = cos (v[i].gth), s = sin (v[i].gth);
          double px = v[i].gx + (c * dx - s * dy);
          double py = v[i].gy + (s * dx + c * dy);

          if (px < minx) minx = px;
          if (py < miny) miny = py;
          if (px > maxx) maxx = px;
          if (py > maxy) maxy = py;
        }
    }

  int CW = (int) ceil (maxx - minx) + 2 * margin;
  int CH = (int) ceil (maxy - miny) + 2 * margin;
  printf ("canvas %dx%d\n", CW, CH);

  g_autofree double *acc = g_new0 (double, (gsize) CW * CH);
  g_autofree double *wsum = g_new0 (double, (gsize) CW * CH);
  g_autofree int *cov = g_new0 (int, (gsize) CW * CH);

  /* --- splat each view, feather-weighted ------------------------------ */
  for (int i = 0; i < nview; i++)
    {
      if (!v[i].placed)
        continue;

      double c = cos (v[i].gth), s = sin (v[i].gth);
      double scale = 0;

      for (int k = 0; k < IW * IH; k++)
        if (v[i].img[k] > 0)
          scale += v[i].img[k];
      (void) scale;

      for (int y = 0; y < IH; y++)
        for (int x = 0; x < IW; x++)
          {
            double val = v[i].img[y * IW + x];

            /* Mask at a fraction of THIS view's peak, not at zero.
             *
             * img is already background-minus-frame, so real finger signal is
             * hundreds of counts wide while everything the sensor did not
             * actually see sits near zero. Thresholding at zero therefore lets
             * the resampling noise of a rotated view count as signal, and the
             * overlap region accumulates several copies of that noise and burns
             * white. A relative gate keeps only pixels carrying real ridges. */
            if (val <= 0.05 * v[i].peak || val <= 2.0)
              continue;

            /* Feather by distance to the view's own border: pixels the sensor
             * barely saw blend weakly, which hides both the seams and the
             * vignetting at the edge of the pad. */
            double edge = MIN (MIN (x, IW - 1 - x), MIN (y, IH - 1 - y));
            double w = MIN (1.0, edge / FEATHER_PX);
            if (w <= 0)
              continue;

            double dx = x - v[i].qx, dy = y - v[i].qy;
            int px = (int) lround (v[i].gx + (c * dx - s * dy) - minx) + margin;
            int py = (int) lround (v[i].gy + (s * dx + c * dy) - miny) + margin;

            if (px < 0 || px >= CW || py < 0 || py >= CH)
              continue;

            acc[py * CW + px] += val * w;
            wsum[py * CW + px] += w;
            cov[py * CW + px]++;
          }
    }

  /* --- write --------------------------------------------------------- */
  g_autofree guint8 *grey = g_new0 (guint8, (gsize) CW * CH);
  g_autofree double *sorted = g_new (double, CW * CH);
  int ns = 0;

  for (int i = 0; i < CW * CH; i++)
    if (wsum[i] > 0)
      sorted[ns++] = acc[i] / wsum[i];
  qsort (sorted, ns, sizeof (double), cmp_dbl);

  double lo = ns ? sorted[(int) (ns * 0.02)] : 0;
  double hi = ns ? sorted[(int) (ns * 0.995)] : 1;
  if (hi <= lo) hi = lo + 1;

  for (int i = 0; i < CW * CH; i++)
    {
      if (wsum[i] <= 0)
        continue;
      double t = (acc[i] / wsum[i] - lo) / (hi - lo);
      grey[i] = (guint8) CLAMP ((int) (255.0 * t), 0, 255);
    }

  if (!write_png (out, CW, CH, grey))
    { fprintf (stderr, "cannot write %s\n", out); return 1; }
  printf ("wrote %s\n", out);

  if (cover_out)
    {
      g_autofree guint8 *cmap = g_new0 (guint8, (gsize) CW * CH);
      int maxc = 1;

      for (int i = 0; i < CW * CH; i++)
        if (cov[i] > maxc) maxc = cov[i];
      for (int i = 0; i < CW * CH; i++)
        if (cov[i] > 0)
          cmap[i] = (guint8) (1 + 254 * cov[i] / maxc);
      if (write_png (cover_out, CW, CH, cmap))
        printf ("wrote %s (coverage, up to %d views/pixel)\n", cover_out, maxc);
    }
  return 0;
}
