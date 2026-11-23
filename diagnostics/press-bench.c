/*
 * Bench for "one press counts once".
 *
 *   ./press-bench
 *
 * The rule being tested: two captures promoted as evidence must come from
 * different presses. A press is identified by a generation number, which the
 * driver bumps only when the finger was seen to lift between verifications.
 *
 * This exercises the decision logic in isolation - the same rule as
 * gx_pending_consider - because the hardware path needs a finger, a resting
 * hand and pam retries all at once, which cannot be reproduced on demand.
 *
 * Each scenario states what should happen and fails loudly if it does not.
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>

/* Mirror of the driver's rule, kept deliberately small so the test reads as a
 * statement of intent rather than a reimplementation of the driver. */
typedef struct
{
  guint32 gen;      /* generation of the stored sighting */
  gboolean has;     /* is there a stored sighting at all */
} Pending;

/* Returns TRUE when the sighting should be promoted into the store. */
static gboolean
consider (Pending *p, guint32 gen, gboolean overlaps)
{
  if (!p->has || !overlaps)
    {
      p->gen = gen;
      p->has = TRUE;
      return FALSE;               /* first sighting: remember, do not act */
    }
  if (p->gen == gen)
    return FALSE;                 /* same press seen twice: not evidence */
  p->has = FALSE;                 /* promoted */
  return TRUE;
}

static int failures = 0;

static void
check (const char *what, gboolean got, gboolean want)
{
  printf ("  %-58s %s\n", what, got == want ? "ok" : "FAIL");
  if (got != want)
    failures++;
}

int
main (void)
{
  Pending p = { 0, FALSE };

  printf ("one press held while the caller retries\n");
  check ("first read of press 1: remembered, not promoted",
         consider (&p, 1, TRUE), FALSE);
  check ("second read, same press: not promoted",
         consider (&p, 1, TRUE), FALSE);
  check ("third read, same press: still not promoted",
         consider (&p, 1, TRUE), FALSE);

  printf ("the finger lifts, then presses the same spot again\n");
  check ("new press, same area: promoted",
         consider (&p, 1 + 1, TRUE), TRUE);

  printf ("a held press followed by a genuinely new area\n");
  p = (Pending) { 0, FALSE };
  check ("press A sighted", consider (&p, 2, TRUE), FALSE);
  check ("press A again, still held", consider (&p, 2, TRUE), FALSE);
  check ("press B, different area: not a repeat, just remembered",
         consider (&p, 3, FALSE), FALSE);
  check ("press B again, same press", consider (&p, 3, FALSE), FALSE);

  printf ("unrelated capture never promotes\n");
  p = (Pending) { 0, FALSE };
  check ("press 1, no overlap", consider (&p, 1, FALSE), FALSE);
  check ("press 2, no overlap", consider (&p, 2, FALSE), FALSE);

  printf ("\n%s\n", failures ? "FAILURES" : "all scenarios pass");
  return failures ? 1 : 0;
}
