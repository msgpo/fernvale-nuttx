/****************************************************************************
 * arch/arm/src/sam34/sam_tc.c
 *
 *   Copyright (C) 2014 Gregory Nutt. All rights reserved.
 *   Authors: Gregory Nutt <gnutt@nuttx.org>
 *            Bob Dioron
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <sys/types.h>

#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/timer.h>
#include <arch/board/board.h>

#include "up_arch.h"
#include "sam_tc.h"
#include "sam_periphclks.h"

#if defined(CONFIG_TIMER) && (defined(CONFIG_SAM34_TC0) || \
    defined(CONFIG_SAM34_TC1) || defined(CONFIG_SAM34_TC2) || \
    defined(CONFIG_SAM34_TC3) || defined(CONFIG_SAM34_TC4) || \
    defined(CONFIG_SAM34_TC5))

/****************************************************************************
 * Pre-Processor Definitions
 ****************************************************************************/
/* Clocking *****************************************************************/

/* TODO: Allow selection of any of the input clocks */

#define TC_FCLK        (BOARD_SLCK_FREQUENCY)
#define TC_MAXTIMEOUT  ((1000000ULL * (1ULL + TC_RVALUE_MASK)) / TC_FCLK)

/* Configuration ************************************************************/

/* Debug ********************************************************************/
/* Non-standard debug that may be enabled just for testing the timer
 * driver.  NOTE: that only lldbg types are used so that the output is
 * immediately available.
 */

#ifdef CONFIG_DEBUG_TIMER
#  define tcdbg    lldbg
#  define tcvdbg   llvdbg
#else
#  define tcdbg(x...)
#  define tcvdbg(x...)
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/
/* This structure provides the private representation of the "lower-half"
 * driver state structure.  This structure must be cast-compatible with the
 * timer_lowerhalf_s structure.
 */

struct sam34_lowerhalf_s
{
  FAR const struct timer_ops_s  *ops;  /* Lower half operations */

  /* Private data */

  uint32_t base;            /* Base address of the timer */
  tccb_t   handler;         /* Current user interrupt handler */
  uint32_t timeout;         /* The current timeout value (us) */
  uint32_t adjustment;      /* time lost due to clock resolution truncation (us) */
  uint32_t clkticks;        /* actual clock ticks for current interval */
  bool     started;         /* The timer has been started */
  uint16_t debug;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/
/* Register operations ******************************************************/

#if defined(CONFIG_SAM34_TC_REGDEBUG) && defined(CONFIG_DEBUG)
static uint32_t sam34_getreg(uint32_t addr);
static void     sam34_putreg(uint32_t val, uint32_t addr);
#else
# define        sam34_getreg(addr)     getreg32(addr)
# define        sam34_putreg(val,addr) putreg32(val,addr)
#endif

/* Interrupt handling *******************************************************/

static int      sam34_interrupt(int irq, FAR void *context);

/* "Lower half" driver methods **********************************************/

static int      sam34_start(FAR struct timer_lowerhalf_s *lower);
static int      sam34_stop(FAR struct timer_lowerhalf_s *lower);
static int      sam34_getstatus(FAR struct timer_lowerhalf_s *lower,
                  FAR struct timer_status_s *status);
static int      sam34_settimeout(FAR struct timer_lowerhalf_s *lower,
                  uint32_t timeout);
static tccb_t   sam34_capture(FAR struct timer_lowerhalf_s *lower,
                  tccb_t handler);
static int      sam34_ioctl(FAR struct timer_lowerhalf_s *lower, int cmd,
                  unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/
/* "Lower half" driver methods */

static const struct timer_ops_s g_tcops =
{
  .start      = sam34_start,
  .stop       = sam34_stop,
  .getstatus  = sam34_getstatus,
  .settimeout = sam34_settimeout,
  .capture    = sam34_capture,
  .ioctl      = sam34_ioctl,
};

/* "Lower half" driver state */

static struct sam34_lowerhalf_s g_tcdev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sam34_getreg
 *
 * Description:
 *   Get the contents of a register
 *
 ****************************************************************************/

#if defined(CONFIG_SAM34_TC_REGDEBUG) && defined(CONFIG_DEBUG)
static uint32_t sam34_getreg(uint32_t addr)
{
  static uint32_t prevaddr = 0;
  static uint32_t count = 0;
  static uint32_t preval = 0;

  /* Read the value from the register */

  uint32_t val = getreg32(addr);

  /* Is this the same value that we read from the same registe last time?  Are
   * we polling the register?  If so, suppress some of the output.
   */

  if (addr == prevaddr && val == preval)
    {
      if (count == 0xffffffff || ++count > 3)
        {
           if (count == 4)
             {
               lldbg("...\n");
             }

          return val;
        }
    }

  /* No this is a new address or value */

  else
    {
       /* Did we print "..." for the previous value? */

       if (count > 3)
         {
           /* Yes.. then show how many times the value repeated */

           lldbg("[repeats %d more times]\n", count-3);
         }

       /* Save the new address, value, and count */

       prevaddr = addr;
       preval   = val;
       count    = 1;
    }

  /* Show the register value read */

  lldbg("%08lx->%08lx\n", addr, val);
  return val;
}
#endif

/****************************************************************************
 * Name: sam34_putreg
 *
 * Description:
 *   Set the contents of an SAM34 register to a value
 *
 ****************************************************************************/

#if defined(CONFIG_SAM34_TC_REGDEBUG) && defined(CONFIG_DEBUG)
static void sam34_putreg(uint32_t val, uint32_t addr)
{
  /* Show the register value being written */

  lldbg("%08lx<-%08lx\n", addr, val);

  /* Write the value */

  putreg32(val, addr);
}
#endif


/****************************************************************************
 * Name: sam34_interrupt
 *
 * Description:
 *   TC interrupt
 *
 * Input Parameters:
 *   Usual interrupt handler arguments.
 *
 * Returned Values:
 *   Always returns OK.
 *
 ****************************************************************************/

static int sam34_interrupt(int irq, FAR void *context)
{
  FAR struct sam34_lowerhalf_s *priv = &g_tcdev;
  uint16_t regval;

  tcvdbg("Entry\n");

  /* Check if the interrupt is really pending */

  regval = sam34_getreg(SAM_TC0_SR);
  if ((regval & TC_INT_CPCS) != 0)
    {
      uint32_t timeout;

      /* Is there a registered handler? */

      if (priv->handler && priv->handler(&priv->timeout))
        {
          /* Calculate new ticks */

          priv->clkticks = ((uint64_t)(priv->adjustment + priv->timeout))*TC_FCLK / 1000000;

          /* Set next interval interval. TODO: make sure the interval is not so soon it will be missed! */

          sam34_putreg(priv->clkticks, SAM_TC0_RC);

          timeout = (1000000ULL * priv->clkticks) / TC_FCLK;    /* trucated timeout */
          priv->adjustment = (priv->adjustment + priv->timeout) - timeout;  /* truncated time to be added to next interval (dither) */
        }
      else /* stop */
        {
          sam34_stop((FAR struct timer_lowerhalf_s *)&g_tcdev);
        }

      /* TC_INT_CPCS is cleared by reading SAM_TC0_SR */
    }

  return OK;
}

/****************************************************************************
 * Name: sam34_start
 *
 * Description:
 *   Start the timer, resetting the time to the current timeout,
 *
 * Input Parameters:
 *   lower - A pointer the publicly visible representation of the "lower-half"
 *           driver state structure.
 *
 * Returned Values:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int sam34_start(FAR struct timer_lowerhalf_s *lower)
{
  FAR struct sam34_lowerhalf_s *priv = (FAR struct sam34_lowerhalf_s *)lower;
  uint32_t mr_val = 0;

  tcvdbg("Entry\n");
  DEBUGASSERT(priv);

  if(priv->started)
    {
      return -EINVAL;
    }

  sam_tc0_enableclk();

  sam34_putreg(TC_CCR_CLKDIS, SAM_TC0_CCR); /* Disable counter */

  /* TC_CMR_WAVE - waveform mode
   * TC_CMR_WAVSEL_UPAUTO - reset on RC compare (interval timer)
   * TC_CMR_TCCLKS_TIMERCLOCK5 = SCLK
   */

  mr_val |= (TC_CMR_WAVE + TC_CMR_WAVSEL_UPAUTO + TC_CMR_TCCLKS_TIMERCLOCK5);
  sam34_putreg(mr_val, SAM_TC0_CMR);

  sam34_putreg(priv->clkticks, SAM_TC0_RC); /* Set interval */

  if (priv->handler)
    {
      /* Clear status and enable interrupt */

      sam34_getreg(SAM_TC0_SR);
      sam34_putreg(TC_INT_CPCS, SAM_TC0_IER);
    }

  sam34_putreg(TC_CCR_SWTRG + TC_CCR_CLKEN, SAM_TC0_CCR); /* Start counter */

  priv->started = true;
  return OK;
}

/****************************************************************************
 * Name: sam34_stop
 *
 * Description:
 *   Stop the timer
 *
 * Input Parameters:
 *   lower - A pointer the publicly visible representation of the "lower-half"
 *           driver state structure.
 *
 * Returned Values:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int sam34_stop(FAR struct timer_lowerhalf_s *lower)
{
  FAR struct sam34_lowerhalf_s *priv = (FAR struct sam34_lowerhalf_s *)lower;
  tcvdbg("Entry\n");
  DEBUGASSERT(priv);

  if(!priv->started)
    {
      return -EINVAL;
    }

  sam34_putreg(TC_CCR_CLKDIS, SAM_TC0_CCR); /* Disable counter */
  sam34_putreg(TC_INT_ALL, SAM_TC0_IDR); /* Disable all ints */
  sam_tc0_disableclk();
  priv->started = false;

  return OK;
}

/****************************************************************************
 * Name: sam34_getstatus
 *
 * Description:
 *   Get the current timer status
 *
 * Input Parameters:
 *   lower  - A pointer the publicly visible representation of the "lower-half"
 *            driver state structure.
 *   status - The location to return the status information.
 *
 * Returned Values:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int sam34_getstatus(FAR struct timer_lowerhalf_s *lower,
                           FAR struct timer_status_s *status)
{
  FAR struct sam34_lowerhalf_s *priv = (FAR struct sam34_lowerhalf_s *)lower;
  uint32_t elapsed;

  tcvdbg("Entry\n");
  DEBUGASSERT(priv);

  /* Return the status bit */

  status->flags = 0;
  if (priv->started)
    {
      status->flags |= TCFLAGS_ACTIVE;
    }

  if (priv->handler)
    {
      status->flags |= TCFLAGS_CAPTURE;
    }

  /* Return the actual timeout is milliseconds */

  status->timeout = priv->timeout;

  /* Get the time remaining until the timer expires (in microseconds) */

  elapsed = sam34_getreg(SAM_TC0_CV);
  status->timeleft = (priv->timeout * elapsed) / (priv->clkticks + 1); /* TODO - check on this +1 */

  tcvdbg("  flags    : %08x\n", status->flags);
  tcvdbg("  timeout  : %d\n", status->timeout);
  tcvdbg("  timeleft : %d\n", status->timeleft);
  return OK;
}

/****************************************************************************
 * Name: sam34_settimeout
 *
 * Description:
 *   Set a new timeout value (and reset the timer)
 *
 * Input Parameters:
 *   lower   - A pointer the publicly visible representation of the "lower-half"
 *             driver state structure.
 *   timeout - The new timeout value in milliseconds.
 *
 * Returned Values:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int sam34_settimeout(FAR struct timer_lowerhalf_s *lower,
                            uint32_t timeout)
{
  FAR struct sam34_lowerhalf_s *priv = (FAR struct sam34_lowerhalf_s *)lower;

  DEBUGASSERT(priv);
  tcvdbg("Entry: timeout=%d\n", timeout);

  /* Can this timeout be represented? */

  if (timeout < 1 || timeout > TC_MAXTIMEOUT)
    {
      tcdbg("Cannot represent timeout=%lu > %lu\n",
            timeout, TC_MAXTIMEOUT);
      return -ERANGE;
    }

  priv->timeout = timeout;                                    /* Intended timeout */
  priv->clkticks = (((uint64_t)timeout * TC_FCLK) / 1000000); /* Actual clock ticks */
  timeout = (1000000ULL * priv->clkticks) / TC_FCLK;          /* Truncated timeout */
  priv->adjustment = priv->timeout - timeout;                 /* Truncated time to be added to next interval (dither) */

  tcvdbg("fwdt=%d reload=%d timout=%d, adjustment=%d\n",
         TC_FCLK, reload, priv->timeout, priv->adjustment);

  /* Don't commit to MR register until started! */

  return OK;
}

/****************************************************************************
 * Name: sam34_capture
 *
 * Description:
 *   Don't reset on timer timeout; instead, call this user provider
 *   timeout handler.  NOTE:  Providing handler==NULL will restore the reset
 *   behavior.
 *
 * Input Parameters:
 *   lower      - A pointer the publicly visible representation of the "lower-half"
 *                driver state structure.
 *   newhandler - The new timer expiration function pointer.  If this
 *                function pointer is NULL, then the reset-on-expiration
 *                behavior is restored,
 *
 * Returned Values:
 *   The previous timer expiration function pointer or NULL is there was
 *   no previous function pointer, i.e., if the previous behavior was
 *   reset-on-expiration (NULL is also returned if an error occurs).
 *
 ****************************************************************************/

static tccb_t sam34_capture(FAR struct timer_lowerhalf_s *lower,
                            tccb_t handler)
{
  FAR struct sam34_lowerhalf_s *priv = (FAR struct sam34_lowerhalf_s *)lower;
  irqstate_t flags;
  tccb_t oldhandler;

  flags = irqsave();

  DEBUGASSERT(priv);
  tcvdbg("Entry: handler=%p\n", handler);

  /* Get the old handler return value */

  oldhandler = priv->handler;

  /* Save the new handler */

   priv->handler = handler;

  irqrestore(flags);
  return oldhandler;
}

/****************************************************************************
 * Name: sam34_ioctl
 *
 * Description:
 *   Any ioctl commands that are not recognized by the "upper-half" driver
 *   are forwarded to the lower half driver through this method.
 *
 * Input Parameters:
 *   lower - A pointer the publicly visible representation of the "lower-half"
 *           driver state structure.
 *   cmd   - The ioctl command value
 *   arg   - The optional argument that accompanies the 'cmd'.  The
 *           interpretation of this argument depends on the particular
 *           command.
 *
 * Returned Values:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int sam34_ioctl(FAR struct timer_lowerhalf_s *lower, int cmd,
                    unsigned long arg)
{
  FAR struct sam34_lowerhalf_s *priv = (FAR struct sam34_lowerhalf_s *)lower;
  int ret = -ENOTTY;

  DEBUGASSERT(priv);
  tcvdbg("Entry: cmd=%d arg=%ld\n", cmd, arg);

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sam_tcinitialize
 *
 * Description:
 *   Initialize the timer.  The timer is initialized and
 *   registers as 'devpath'.
 *
 * Input Parameters:
 *   devpath - The full path to the timer.  This should be of the form
 *     /dev/tc0
 *
 * Returned Values:
 *   None
 *
 ****************************************************************************/

void sam_tcinitialize(FAR const char *devpath, int irq)
{
  FAR struct sam34_lowerhalf_s *priv = &g_tcdev;

  tcvdbg("Entry: devpath=%s\n", devpath);

  /* NOTE we assume that clocking to the IWDG has already been provided by
   * the RCC initialization logic.
   */

  /* Initialize the driver state structure.  Here we assume: (1) the state
   * structure lies in .bss and was zeroed at reset time.  (2) This function
   * is only called once so it is never necessary to re-zero the structure.
   */

  priv->ops = &g_tcops;

  /* TODO: Add irq + switch in sam34_interrupt or something (also need register
   * base address...
   */

  (void)irq_attach(irq, sam34_interrupt);

  /* enable interrupt.
   *
   * TODO: May want to enable/disable in start/stop...
   */

  up_enable_irq(irq);

  /* Register the timer driver as /dev/timerX */

  (void)timer_register(devpath, (FAR struct timer_lowerhalf_s *)priv);
}

#endif /* CONFIG_TIMER && CONFIG_SAM34_TCx */