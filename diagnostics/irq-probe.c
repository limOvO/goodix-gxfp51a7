// irq-probe — verifies the sensor's IRQ line is usable, and measures how much
// of each protocol wait it could save.
//
// The driver currently waits a fixed 15 ms after every command and then polls
// for a reply. The vendor driver, and the public protocol notes, instead wait
// for GPIO 48 to assert — the sensor raises it when a reply is staged. If that
// line works here, every command can return as soon as the sensor is ready
// instead of after a guess, which is most of the capture sequence's 607 ms.
//
//   cc -O2 irq-probe.c -o irq-probe $(pkg-config --cflags --libs glib-2.0)
//   sudo ./irq-probe            # needs /dev/gpiochip0 and the spidev node
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define IRQ_LINE 48
#define SPI_DEV "/dev/spidev1.0"

static const guint8 AMORCE[] = { 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88 };

static gint64
now_us (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (gint64) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

typedef struct
{
  int spi;
  int gpio_fd;
  int irq_fd;
} Ctx;

static guint8
cksum (const guint8 *b, gsize n)
{
  guint8 s = 0;

  while (n--)
    s += *b++;
  return (guint8) (0xAA - s);
}

static gboolean
spi_xfer (int fd, const guint8 *tx, guint8 *rx, gsize n)
{
  struct spi_ioc_transfer x = { 0 };

  x.tx_buf = (unsigned long) tx;
  x.rx_buf = (unsigned long) rx;
  x.len = (guint32) n;
  x.speed_hz = 1000000;
  x.bits_per_word = 8;
  return ioctl (fd, SPI_IOC_MESSAGE (1), &x) >= 1;
}

static gboolean
write_frame (Ctx *c, guint8 type, const guint8 *body, gsize n)
{
  guint8 buf[512], rx[512];
  gsize len = 0;

  buf[len++] = type;
  buf[len++] = n & 0xFF;
  buf[len++] = (n >> 8) & 0xFF;
  buf[len++] = (guint8) (buf[0] + buf[1] + buf[2]);
  memcpy (buf + len, body, n);
  len += n;

  if (!spi_xfer (c->spi, buf, rx, 4))
    return FALSE;
  usleep (2000);
  return spi_xfer (c->spi, buf + 4, rx + 4, n);
}

/* Waits for GPIO 48 to read high. Returns the delay in microseconds, or -1. */
static gint64
wait_irq (Ctx *c, int timeout_ms)
{
  struct gpio_v2_line_values v = { 0 };
  gint64 t0 = now_us ();

  v.mask = 1;
  for (;;)
    {
      if (ioctl (c->irq_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) == 0 && (v.bits & 1))
        return now_us () - t0;
      if ((now_us () - t0) > (gint64) timeout_ms * 1000)
        return -1;
      usleep (500);
    }
}

static gboolean
open_irq (Ctx *c)
{
  struct gpio_v2_line_request req = { 0 };

  c->gpio_fd = open ("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
  if (c->gpio_fd < 0)
    {
      fprintf (stderr, "open gpiochip0: %s\n", strerror (errno));
      return FALSE;
    }

  req.offsets[0] = IRQ_LINE;
  req.num_lines = 1;
  req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
  g_strlcpy (req.consumer, "irq-probe", sizeof req.consumer);

  if (ioctl (c->gpio_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
    {
      fprintf (stderr, "request line %d: %s\n", IRQ_LINE, strerror (errno));
      return FALSE;
    }
  c->irq_fd = req.fd;
  return TRUE;
}

int
main (void)
{
  Ctx c = { .spi = -1, .gpio_fd = -1, .irq_fd = -1 };
  guint8 mode = 0, bits = 8;
  guint32 speed = 1000000;
  struct gpio_v2_line_values idle = { 0 };

  c.spi = open (SPI_DEV, O_RDWR);
  if (c.spi < 0)
    {
      fprintf (stderr, "open %s: %s\n", SPI_DEV, strerror (errno));
      fprintf (stderr, "bind spidev first (see recover.sh)\n");
      return 1;
    }
  ioctl (c.spi, SPI_IOC_WR_MODE, &mode);
  ioctl (c.spi, SPI_IOC_WR_BITS_PER_WORD, &bits);
  ioctl (c.spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

  if (!open_irq (&c))
    return 1;

  /* Baseline: what does the line read while the sensor is quiet? */
  idle.mask = 1;
  ioctl (c.irq_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &idle);
  printf ("GPIO %d idle level: %s\n\n", IRQ_LINE, (idle.bits & 1) ? "HIGH" : "low");

  /* Firmware-version command: a known-good exchange with a two-part reply. */
  static const guint8 fwcmd[] = { 0xA8, 0x03, 0x00, 0x70, 0xDC, 0xB3 };
  const int N = 8;
  gint64 fixed_total = 0, irq_total = 0;
  int irq_hits = 0;

  printf ("%3s %14s %14s %s\n", "run", "fixed 15ms", "irq-driven", "saving");
  printf ("%s\n", "-----------------------------------------------------");

  for (int i = 0; i < N; i++)
    {
      gint64 t0, t_fixed, t_irq;
      guint8 hdr[4], body[256];

      /* --- the current driver's approach: sleep, then poll --- */
      t0 = now_us ();
      write_frame (&c, 0xA0, AMORCE, sizeof AMORCE);
      usleep (8000);
      write_frame (&c, 0xA0, fwcmd, sizeof fwcmd);
      usleep (15000);
      spi_xfer (c.spi, hdr, hdr, 4);            /* ACK-ish */
      usleep (5000);
      spi_xfer (c.spi, body, body, 26);         /* payload */
      t_fixed = now_us () - t0;

      /* --- irq-driven: return as soon as the sensor asserts --- */
      t0 = now_us ();
      write_frame (&c, 0xA0, AMORCE, sizeof AMORCE);
      write_frame (&c, 0xA0, fwcmd, sizeof fwcmd);

      gint64 w1 = wait_irq (&c, 200);
      if (w1 >= 0)
        {
          spi_xfer (c.spi, hdr, hdr, 4);
          gint64 w2 = wait_irq (&c, 200);

          if (w2 >= 0)
            {
              spi_xfer (c.spi, body, body, 26);
              irq_hits++;
            }
          t_irq = now_us () - t0;
          printf ("%3d %11ld us %11ld us %8ld us\n", i + 1,
                  (long) t_fixed, (long) t_irq, (long) (t_fixed - t_irq));
        }
      else
        {
          printf ("%3d %11ld us %14s %s\n", i + 1, (long) t_fixed,
                  "no IRQ", "(line does not assert here)");
        }

      fixed_total += t_fixed;
      if (w1 >= 0)
        irq_total += t_irq;
      usleep (200000);
    }

  printf ("\nIRQ asserted on %d of %d exchanges\n", irq_hits, N);
  if (irq_hits)
    {
      printf ("fixed 15ms approach : %ld us/command\n", (long) (fixed_total / N));
      printf ("irq-driven approach  : %ld us/command\n", (long) (irq_total / irq_hits));
      printf ("saving               : %ld us/command\n",
              (long) ((fixed_total / N) - (irq_total / irq_hits)));
      printf ("\nOver 5 commands per capture that is ~%ld ms off the sequence.\n",
              (long) (((fixed_total / N) - (irq_total / irq_hits)) * 5 / 1000));
    }
  else
    {
      printf ("The line never asserted, so it cannot gate the protocol here.\n");
      printf ("Falling back to timed waits (the current behaviour).\n");
    }
  return 0;
}
