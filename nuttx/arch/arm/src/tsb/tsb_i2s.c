/**
 * Copyright (c) 2015-2016 Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @author Mark Greer
 * @brief TSB I2S device driver
 */
/*
 * Clocks:
 *  MCLK    - Master Clock: used to drive the other clocks.
 *  BCLK    - Bit Clock: used for clocking each bit in/out.
 *            Also referred to as SCLK (Serial Clock).
 *  WCLK    - Word Clock: determines which channel audio data is for.
 *            Also referred to as LRCLK (Left-right Clock).
 */

#include <string.h>
#include <errno.h>
#include <nuttx/lib.h>
#include <nuttx/kmalloc.h>
#include <nuttx/device.h>
#include <nuttx/device_pll.h>
#include <nuttx/device_i2s.h>
#include <nuttx/ring_buf.h>
#include <nuttx/power/pm.h>

#include <arch/byteorder.h>
#include <arch/tsb/pm.h>

#include "up_arch.h"
#include "tsb_scm.h"
#include "tsb_pinshare.h"

#include "tsb_i2s.h"
#include "tsb_i2s_xfer.h"

/*
 * The nuttx IRQ handler interface doesn't provide an 'arg' parameter
 * where 'dev' can be passed so use a global for now.  Must be fixed to
 * support more than one tsb i2s controller.
 */
struct device *saved_dev;

static int tsb_i2s_irq_so_handler(int irq, void *context, void *priv)
{
    struct tsb_i2s_info *info = device_get_private(saved_dev);
    uint32_t intstat;

    intstat = tsb_i2s_read(info, TSB_I2S_BLOCK_SO, TSB_I2S_REG_INTSTAT);

    if (intstat & TSB_I2S_REG_INT_INT)
        tsb_i2s_tx_data(info);
    else
        tsb_i2s_clear_irqs(info, TSB_I2S_BLOCK_SO, intstat);

    return 0;
}

static int tsb_i2s_irq_si_handler(int irq, void *context, void *priv)
{
    struct tsb_i2s_info *info = device_get_private(saved_dev);
    uint32_t intstat;

    intstat = tsb_i2s_read(info, TSB_I2S_BLOCK_SI, TSB_I2S_REG_INTSTAT);

    if (intstat & TSB_I2S_REG_INT_INT) {
        tsb_i2s_mask_irqs(info, TSB_I2S_BLOCK_SI, TSB_I2S_REG_INT_INT);
        tsb_i2s_rx_data(info);
    } else {
        tsb_i2s_clear_irqs(info, TSB_I2S_BLOCK_SI, intstat);
    }

    return 0;
}

static int tsb_i2s_xfer_irq_attach(struct tsb_i2s_info *info)
{
    int retval = 0;

    retval = irq_attach(info->so_irq, tsb_i2s_irq_so_handler, NULL);
    if (retval) {
        lldbg("Failed to attach I2SO irq.\n");
        return retval;
    }

    retval = irq_attach(info->si_irq, tsb_i2s_irq_si_handler, NULL);
    if (retval) {
        lldbg("Failed to attach I2SI irq.\n");
        return retval;
    }

    return retval;
}

static int tsb_i2s_xfer_irq_detach(struct tsb_i2s_info *info)
{
    int retval = 0;

    retval = irq_detach(info->so_irq);
    if (retval) {
        irq_detach(info->si_irq);
        return retval;
    }

    return irq_detach(info->si_irq);
}

int tsb_i2s_xfer_shutdown_transmitter(struct tsb_i2s_info *info)
{
    return 0;
}

int tsb_i2s_xfer_shutdown_receiver(struct tsb_i2s_info *info)
{
    return 0;
}

static int tsb_i2s_verify_pcm_support(uint8_t clk_role, struct device_i2s_pcm *pcm)
{
    int ret = 0;

    /*
     * For now ignore clk_role, TODO if we are a bclk slave.
     * We may be able to support a greater range of frequencies
     */
    if (!ONE_BIT_IS_SET(pcm->format) ||
        !ONE_BIT_IS_SET(pcm->rate)) {
        ret = -EINVAL;
    } else if (!(pcm->format & TSB_I2S_FMT_MASK)) {
        ret = -EINVAL;
    } else if (!(pcm->rate & TSB_I2S_RATE_MASK)) {
        ret = -EINVAL;
    } else if ((pcm->channels > 2) || (pcm->channels == 0)) {
        ret = -EINVAL;
    }

    return ret;
}

static int tsb_i2s_ratebit_to_ratefreq(int rate_bitfield)
{
    int frequency = 0;

    if (!ONE_BIT_IS_SET(rate_bitfield))
        return -EINVAL;

    switch (rate_bitfield) {
    case DEVICE_I2S_PCM_RATE_5512:
        frequency = 5512;
        break;
    case DEVICE_I2S_PCM_RATE_8000:
        frequency = 8000;
        break;
    case DEVICE_I2S_PCM_RATE_11025:
        frequency = 11025;
        break;
    case DEVICE_I2S_PCM_RATE_16000:
        frequency = 16000;
        break;
    case DEVICE_I2S_PCM_RATE_22050:
        frequency = 22050;
        break;
    case DEVICE_I2S_PCM_RATE_32000:
        frequency = 32000;
        break;
    case DEVICE_I2S_PCM_RATE_44100:
        frequency = 44100;
        break;
    case DEVICE_I2S_PCM_RATE_48000:
        frequency = 48000;
        break;
    case DEVICE_I2S_PCM_RATE_64000:
        frequency = 64000;
        break;
    case DEVICE_I2S_PCM_RATE_88200:
        frequency = 88200;
        break;
    case DEVICE_I2S_PCM_RATE_96000:
        frequency = 96000;
        break;
    case DEVICE_I2S_PCM_RATE_176400:
        frequency = 176400;
        break;
    case DEVICE_I2S_PCM_RATE_192000:
        frequency = 192000;
        break;
    default:
        return -EINVAL;
    }

    return frequency;
}

static int tsb_i2s_fmtbit_to_numbits(int format_bitfield)
{
    int bits = 0;

    if (!ONE_BIT_IS_SET(format_bitfield))
        return -EINVAL;

    switch (format_bitfield) {
    case DEVICE_I2S_PCM_FMT_8:
        bits = 8;
        break;
    case DEVICE_I2S_PCM_FMT_16:
        bits = 16;
        break;
    case DEVICE_I2S_PCM_FMT_24:
        bits = 24;
        break;
    case DEVICE_I2S_PCM_FMT_32:
        bits = 32;
        break;
    case DEVICE_I2S_PCM_FMT_64:
        bits = 64;
        break;
    default:
        return -EINVAL;
    }

    return bits;
}

static int tsb_i2s_calc_bclk(struct device_i2s_pcm *pcm)
{
    int sample_frequency = 0;
    int sample_size = 0;

    sample_frequency = tsb_i2s_ratebit_to_ratefreq(pcm->rate);
    if (sample_frequency <= 0) {
        return -EINVAL;
    }
    sample_size = tsb_i2s_fmtbit_to_numbits(pcm->format);
    if (sample_size <= 0) {
        return -EINVAL;
    }

    return (sample_frequency * pcm->channels * sample_size);
}

static int tsb_i2s_test_mclk(struct device *pll_dev,
                             struct device_i2s_pcm *pcm)
{
    int ret;
    uint32_t bclk_freq;
    uint32_t mclk_freq;
    bool close_pll = false;

    /* Can we generate mclk to match pcm setting */
    ret = tsb_i2s_calc_bclk(pcm);
    if (ret < 0) {
        return ret;
    }
    if (ret == 0) {
        return -EINVAL;
    }
    bclk_freq = (uint32_t)ret;

    if (!pll_dev) {
        pll_dev = device_open(DEVICE_TYPE_PLL_HW, TSB_I2S_PLLA_ID);
        if (!pll_dev) {
            return -EIO;
        }

        close_pll = true;
    }
    /*
     * The I2S controller always divides MCLK by four and optionally
     * by two again to get the BCLK.  So try to set the PLL/MCLK
     * frequency to four or eight times the required BCLK frequency.
     */
    mclk_freq = bclk_freq * 4;
    ret = device_pll_query_frequency(pll_dev, mclk_freq);
    if (ret) {
        if (ret == -EINVAL) {
            mclk_freq = bclk_freq * 8;
            ret = device_pll_query_frequency(pll_dev, mclk_freq);
        }
    }

    if (close_pll) {
        device_close(pll_dev);
    }

    if (ret)
        return ret;

    return mclk_freq;
}

static int tsb_i2s_device_is_open(struct tsb_i2s_info *info)
{
    return !!(info->flags & TSB_I2S_FLAG_OPEN);
}

static int tsb_i2s_device_is_configured(struct tsb_i2s_info *info)
{
    return tsb_i2s_device_is_open(info) &&
           (info->flags & TSB_I2S_FLAG_CONFIGURED);
}

static int tsb_i2s_device_is_enabled(struct tsb_i2s_info *info)
{
    return tsb_i2s_device_is_configured(info) &&
           (info->flags & TSB_I2S_FLAG_ENABLED);
}

static int tsb_i2s_rx_is_prepared(struct tsb_i2s_info *info)
{
    return tsb_i2s_device_is_enabled(info) &&
           (info->flags & TSB_I2S_FLAG_RX_PREPARED);
}

static int tsb_i2s_tx_is_prepared(struct tsb_i2s_info *info)
{
    return tsb_i2s_device_is_enabled(info) &&
           (info->flags & TSB_I2S_FLAG_TX_PREPARED);
}

int tsb_i2s_rx_is_active(struct tsb_i2s_info *info)
{
    return tsb_i2s_rx_is_prepared(info) &&
           (info->flags & TSB_I2S_FLAG_RX_ACTIVE);
}

int tsb_i2s_tx_is_active(struct tsb_i2s_info *info)
{
    return tsb_i2s_tx_is_prepared(info) &&
           (info->flags & TSB_I2S_FLAG_TX_ACTIVE);
}

uint32_t tsb_i2s_get_block_base(struct tsb_i2s_info *info,
                                enum tsb_i2s_block block)
{
    uint32_t base;

    switch (block) {
    case TSB_I2S_BLOCK_BRIDGE:
        base = info->cg_base;
        break;
    case TSB_I2S_BLOCK_SC:
        base = info->sc_base;
        break;
    case TSB_I2S_BLOCK_SO:
        base = info->so_base;
        break;
    case TSB_I2S_BLOCK_SI:
        base = info->si_base;
        break;
    default:
        base = 0;
    }

    return base;
}

uint32_t tsb_i2s_read_raw(struct tsb_i2s_info *info,
                                 enum tsb_i2s_block block, unsigned int reg)
{
    uint32_t base;

    base = tsb_i2s_get_block_base(info, block);
    if (!base)
        return 0;

    return getreg32(base + reg);
}

void tsb_i2s_write_raw(struct tsb_i2s_info *info,
                              enum tsb_i2s_block block,
                              unsigned int reg, uint32_t val)
{
    uint32_t base;

    base = tsb_i2s_get_block_base(info, block);
    if (!base)
        return;

    putreg32(val, base + reg);
}

uint32_t tsb_i2s_read(struct tsb_i2s_info *info,
                             enum tsb_i2s_block block, unsigned int reg)
{
    return le32_to_cpu(tsb_i2s_read_raw(info, block, reg));
}

static void tsb_i2s_write(struct tsb_i2s_info *info, enum tsb_i2s_block block,
                          unsigned int reg, uint32_t val)
{
    tsb_i2s_write_raw(info, block, reg, cpu_to_le32(val));
}

static void tsb_i2s_write_field(struct tsb_i2s_info *info,
                                enum tsb_i2s_block block, unsigned int reg,
                                uint32_t mask, uint32_t val)
{
    uint32_t v;

    v = tsb_i2s_read(info, block, reg);
    v &= ~mask;
    v |= (val & mask);
    tsb_i2s_write(info, block, reg, v);
}

void tsb_i2s_mask_irqs(struct tsb_i2s_info *info,
                       enum tsb_i2s_block block, uint32_t mask)
{
    uint32_t v;

    v = tsb_i2s_read(info, block, TSB_I2S_REG_INTMASK);
    tsb_i2s_write(info, block, TSB_I2S_REG_INTMASK, v | mask);
}

void tsb_i2s_unmask_irqs(struct tsb_i2s_info *info,
                         enum tsb_i2s_block block, uint32_t mask)
{
    uint32_t v;

    v = tsb_i2s_read(info, block, TSB_I2S_REG_INTMASK);
    tsb_i2s_write(info, block, TSB_I2S_REG_INTMASK, v & ~mask);
}

void tsb_i2s_clear_irqs(struct tsb_i2s_info *info,
                               enum tsb_i2s_block block, uint32_t mask)
{
    tsb_i2s_write(info, block, TSB_I2S_REG_INTCLR, mask);
}

static void tsb_i2s_mute(struct tsb_i2s_info *info, enum tsb_i2s_block block)
{
    tsb_i2s_write_field(info, block, TSB_I2S_REG_MUTE, TSB_I2S_REG_MUTE_MUTEN,
                        0);
}

static void tsb_i2s_unmute(struct tsb_i2s_info *info, enum tsb_i2s_block block)
{
    tsb_i2s_write_field(info, block, TSB_I2S_REG_MUTE, TSB_I2S_REG_MUTE_MUTEN,
                        TSB_I2S_REG_MUTE_MUTEN);
}

static void tsb_i2s_reset(struct tsb_i2s_info *info)
{
    if (!tsb_i2s_device_is_enabled(info))
        return;

    tsb_reset(TSB_RST_I2SSYS);
    tsb_reset(TSB_RST_I2SBIT);
}

static void tsb_i2s_enable(struct tsb_i2s_info *info)
{
    if (tsb_i2s_device_is_enabled(info))
        return;

    tsb_clk_enable(TSB_CLK_I2SSYS);
    tsb_clk_enable(TSB_CLK_I2SBIT);

    info->flags |= TSB_I2S_FLAG_ENABLED;
}

static void tsb_i2s_disable(struct tsb_i2s_info *info)
{
    if (!tsb_i2s_device_is_enabled(info))
        return;

    tsb_clk_disable(TSB_CLK_I2SBIT);
    tsb_clk_disable(TSB_CLK_I2SSYS);

    info->flags &= ~TSB_I2S_FLAG_ENABLED;
}

#ifdef CONFIG_PM
static int i2s_pm_activity(struct device *dev)
{
    pm_activity(TSB_I2S_ACTIVITY);

    return tsb_pm_wait_for_wakeup();
}
#else
static int i2s_pm_activity(struct device *dev)
{
    return 0;
}
#endif

static void tsb_i2s_init_block(struct tsb_i2s_info *info,
                               enum tsb_i2s_block block)
{
    if (block == TSB_I2S_BLOCK_SO)
        tsb_i2s_write(info, block, TSB_I2S_REG_TX_SSIZE,
                      TSB_I2S_TX_START_THRESHOLD);

    tsb_i2s_mask_irqs(info, block,
                      TSB_I2S_REG_INT_DMACMSK | TSB_I2S_REG_INT_LRCK |
                      TSB_I2S_REG_INT_UR | TSB_I2S_REG_INT_OR |
                      TSB_I2S_REG_INT_INT);

    tsb_i2s_clear_irqs(info, block,
                       TSB_I2S_REG_INT_LRCK | TSB_I2S_REG_INT_UR |
                       TSB_I2S_REG_INT_OR | TSB_I2S_REG_INT_INT);
}

static int tsb_i2s_config_hw(struct tsb_i2s_info *info)
{
    struct device_i2s_dai *dai = &info->dai;
    struct device_i2s_pcm *pcm = &info->pcm;
    uint32_t sc_audioset, so_audioset, si_audioset, modeset;
    uint32_t bits_per_channel;

    sc_audioset = 0;
    so_audioset = 0;
    si_audioset = 0;
    modeset = 0;

    bits_per_channel = tsb_i2s_fmtbit_to_numbits(pcm->format);

    if (bits_per_channel > 16) {
        sc_audioset |= TSB_I2S_REG_AUDIOSET_SCLKTOWS;
        so_audioset |= TSB_I2S_REG_AUDIOSET_SCLKTOWS;
        si_audioset |= TSB_I2S_REG_AUDIOSET_SCLKTOWS;
    }

    if (dai->wclk_change_edge == DEVICE_I2S_EDGE_RISING) {
        sc_audioset |= TSB_I2S_REG_AUDIOSET_EDGE;
        si_audioset |= TSB_I2S_REG_AUDIOSET_EDGE;
    } else {
        so_audioset |= TSB_I2S_REG_AUDIOSET_EDGE;
    }

    if (dai->data_tx_edge == DEVICE_I2S_EDGE_RISING) {
        so_audioset |= TSB_I2S_REG_AUDIOSET_SDEDGE;
    }

    if (dai->data_rx_edge == DEVICE_I2S_EDGE_RISING) {
        si_audioset |= TSB_I2S_REG_AUDIOSET_SDEDGE;
    }

    so_audioset |= (bits_per_channel);
    si_audioset |= (bits_per_channel);

    switch (dai->protocol) {
    case DEVICE_I2S_PROTOCOL_PCM:
        if (dai->wclk_change_edge == DEVICE_I2S_POLARITY_REVERSED)
            modeset |= TSB_I2S_REG_MODESET_PCM_MONO_REV_POL;
        else
            modeset |= TSB_I2S_REG_MODESET_PCM_MONO;
        break;
    case DEVICE_I2S_PROTOCOL_I2S:
        modeset |= TSB_I2S_REG_MODESET_I2S_STEREO;
        break;
    case DEVICE_I2S_PROTOCOL_LR_STEREO:
        if (dai->wclk_change_edge == DEVICE_I2S_POLARITY_REVERSED)
            modeset |= TSB_I2S_REG_MODESET_LR_STEREO;
        else
            modeset |= TSB_I2S_REG_MODESET_LR_STEREO_REV_POL;
        break;
    default:
        return -EINVAL; /* Arguments already checked so should never happen */
    }

    tsb_i2s_init_block(info, TSB_I2S_BLOCK_SO);
    tsb_i2s_init_block(info, TSB_I2S_BLOCK_SI);

    tsb_i2s_mute(info, TSB_I2S_BLOCK_SO);
    tsb_i2s_mute(info, TSB_I2S_BLOCK_SI);

    tsb_i2s_write(info, TSB_I2S_BLOCK_SC, TSB_I2S_REG_AUDIOSET, sc_audioset);
    tsb_i2s_write(info, TSB_I2S_BLOCK_SO, TSB_I2S_REG_AUDIOSET, so_audioset);
    tsb_i2s_write(info, TSB_I2S_BLOCK_SI, TSB_I2S_REG_AUDIOSET, si_audioset);

    tsb_i2s_write(info, TSB_I2S_BLOCK_SC, TSB_I2S_REG_MODESET, modeset);
    tsb_i2s_write(info, TSB_I2S_BLOCK_SO, TSB_I2S_REG_MODESET, modeset);
    tsb_i2s_write(info, TSB_I2S_BLOCK_SI, TSB_I2S_REG_MODESET, modeset);

    return 0;
}

static uint32_t tsb_i2s_clk_selector_bits(uint32_t *mclk_slave,
                                          uint32_t *lr_bclk_slave,
                                          uint32_t *mask)
{
    if (tsb_get_rev_id() == tsb_rev_es2) {
        *mclk_slave = TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_MASTER_CLOCK_SEL_ES2;
        *lr_bclk_slave = TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_LR_BCLK_SEL_ES2;
        *mask = TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_MASK_ES2;
    } else if (tsb_get_rev_id() == tsb_rev_es3) {
        *mclk_slave = TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_MASTER_CLOCK_SEL_ES3 |
                      TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_MASTER_CLOCK_MUX_ES3;
        *lr_bclk_slave = TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_LR_BCLK_SEL_ES3 |
                         TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_LR_BCLK_MUX_ES3;
        *mask = TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_MASK_ES3;
    } else {
        return -EIO;
    }

    return 0;
}

static int tsb_i2s_start_clocks(struct tsb_i2s_info *info)
{
    uint32_t bclk_freq, i2s_clk_sel = 0;
    uint32_t mclk_slave_bits, lr_bclk_slave_bits, i2s_clk_sel_mask;
    int ret;

    ret = tsb_i2s_clk_selector_bits(&mclk_slave_bits, &lr_bclk_slave_bits,
                                    &i2s_clk_sel_mask);
    if (ret) {
        return ret;
    }

    if (info->mclk_role == DEVICE_I2S_ROLE_MASTER) {
        bclk_freq = tsb_i2s_calc_bclk(&info->pcm);
        if (bclk_freq <= 0) {
            return -EINVAL;
        }

        info->pll_dev = device_open(DEVICE_TYPE_PLL_HW, TSB_I2S_PLLA_ID);
        if (!info->pll_dev)
            return -EIO;

        /*
         * The I2S controller always divides MCLK by four and optionally
         * by two again to get the BCLK.  So try to set the PLL/MCLK
         * frequency to four or eight times the required BCLK frequency.
         */
        ret = device_pll_set_frequency(info->pll_dev, bclk_freq * 4);
        if (ret) {
            if (ret == -EINVAL) {
                ret = device_pll_set_frequency(info->pll_dev, bclk_freq * 8);
                if (ret)
                    goto err_close_pll;

                i2s_clk_sel |= TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR_DAC16BIT_SEL;
            } else {
                goto err_close_pll;
            }
        }

        ret = device_pll_start(info->pll_dev);
        if (ret)
            goto err_close_pll;
    } else {
        /* TODO:
         * When MCLK slave and BCLK/WCLK master, will only generate
         * BCLK that is 1/4 the frequency of MCLK.  I2S spec needs to
         * be changed to handle slave mode.
         */
        i2s_clk_sel |= mclk_slave_bits;
    }

    if (info->bclk_role == DEVICE_I2S_ROLE_SLAVE)
        i2s_clk_sel |= lr_bclk_slave_bits;

    /* This write starts MCLK & BCLK (not WCLK) if they are clock masters */
    tsb_i2s_write_field(info, TSB_I2S_BLOCK_BRIDGE,
                        TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR, i2s_clk_sel_mask,
                        i2s_clk_sel);

    return 0;

err_close_pll:
    device_close(info->pll_dev);
    info->pll_dev = NULL;

    return ret;
}

static void tsb_i2s_stop_clocks(struct tsb_i2s_info *info)
{
    uint32_t mclk_slave_bits, lr_bclk_slave_bits, i2s_clk_sel_mask;
    int ret;

    ret = tsb_i2s_clk_selector_bits(&mclk_slave_bits, &lr_bclk_slave_bits,
                                    &i2s_clk_sel_mask);
    if (ret) {
        return;
    }

    /* Make MCLK & BCLK/WCLK slaves to stop their output */
    tsb_i2s_write_field(info, TSB_I2S_BLOCK_BRIDGE,
                        TSB_CG_BRIDGE_I2S_CLOCK_SELECTOR, i2s_clk_sel_mask,
                        mclk_slave_bits | lr_bclk_slave_bits);

    if (info->mclk_role == DEVICE_I2S_ROLE_MASTER) {
        device_pll_stop(info->pll_dev);
        device_close(info->pll_dev);
        info->pll_dev = NULL;
    }
}

static int tsb_i2s_start_block(struct tsb_i2s_info *info,
                               enum tsb_i2s_block block)
{
    unsigned int limit;
    uint32_t v;

    /*
     * When writing to the SC block, the following write starts WCLK
     * (MCLK & BCLK already started in tsb_i2s_start_clocks()).
     */
    tsb_i2s_write(info, block, TSB_I2S_REG_START, TSB_I2S_REG_START_START);

    limit = TSB_I2S_POLL_LIMIT;
    do {
         v = tsb_i2s_read(info, block, TSB_I2S_REG_START);
         v &= TSB_I2S_REG_START_START;
    } while (v && --limit);

    if (!limit)
        return -EIO;

    if ((block == TSB_I2S_BLOCK_SO) || (block == TSB_I2S_BLOCK_SI)) {
        tsb_i2s_unmute(info, block);

        tsb_i2s_write(info, block, TSB_I2S_REG_START,
                      TSB_I2S_REG_START_SPK_MIC_START);

        limit = TSB_I2S_POLL_LIMIT;
        do {
             v = tsb_i2s_read(info, block, TSB_I2S_REG_START);
             v &= TSB_I2S_REG_START_SPK_MIC_START;
        } while (v && --limit);

        if (!limit)
            return -EIO;
    }

    return 0;
}

static int tsb_i2s_stop_block(struct tsb_i2s_info *info,
                              enum tsb_i2s_block block, int is_err)
{
    unsigned int limit;
    uint32_t v, ok_val, mask;

    if ((block == TSB_I2S_BLOCK_SO) || (block == TSB_I2S_BLOCK_SI)) {
        if (is_err) {
            mask = TSB_I2S_REG_BUSY_ERRBUSY;
            ok_val = 0;
        } else {
            mask = TSB_I2S_REG_BUSY_SPK_MIC_BUSY;
            ok_val = TSB_I2S_REG_BUSY_SPK_MIC_BUSY;
        }

        limit = TSB_I2S_POLL_LIMIT;
        do {
             v = tsb_i2s_read(info, block, TSB_I2S_REG_BUSY);
             v &= mask;
        } while ((v != ok_val) && --limit);

        if (!limit)
            return -EIO;

        tsb_i2s_mute(info, block);

        limit = TSB_I2S_POLL_LIMIT;
        do {
             v = tsb_i2s_read(info, block, TSB_I2S_REG_BUSY);
             v &= TSB_I2S_REG_BUSY_SERIBUSY;
        } while (v && --limit);

        if (!limit)
            return -EIO;
    }

    tsb_i2s_write(info, block, TSB_I2S_REG_STOP, TSB_I2S_REG_STOP_I2S_STOP);

    limit = TSB_I2S_POLL_LIMIT;
    do {
         v = tsb_i2s_read(info, block, TSB_I2S_REG_STOP);
         v &= TSB_I2S_REG_STOP_I2S_STOP;
    } while (v && --limit);

    if (!limit)
        return -EIO;

    return 0;
}

int tsb_i2s_start(struct tsb_i2s_info *info, enum tsb_i2s_block block)
{
    int ret;

    if (info->bclk_role == DEVICE_I2S_ROLE_MASTER) {
        ret = tsb_i2s_start_block(info, TSB_I2S_BLOCK_SC);
        if (ret)
            return ret;
    }

    ret = tsb_i2s_start_block(info, block);
    if (ret && (info->bclk_role == DEVICE_I2S_ROLE_MASTER))
        tsb_i2s_stop_block(info, TSB_I2S_BLOCK_SC, 0);

    return ret;
}

void tsb_i2s_stop(struct tsb_i2s_info *info, enum tsb_i2s_block block,
                  int is_err)
{
    tsb_i2s_stop_block(info, block, is_err);

    if (info->bclk_role == DEVICE_I2S_ROLE_MASTER)
        tsb_i2s_stop_block(info, TSB_I2S_BLOCK_SC, is_err);
}


enum device_i2s_event tsb_i2s_intstat2event(uint32_t intstat)
{
    enum device_i2s_event event;

    if (!(intstat & TSB_I2S_REG_INT_ERROR_MASK))
        event = DEVICE_I2S_EVENT_NONE;
    else if (intstat & TSB_I2S_REG_INT_OR)
        event = DEVICE_I2S_EVENT_OVERRUN;
    else if (intstat & TSB_I2S_REG_INT_UR)
        event = DEVICE_I2S_EVENT_UNDERRUN;
    else if (intstat & TSB_I2S_REG_INT_LRCK)
        event = DEVICE_I2S_EVENT_CLOCKING;
    else
        event = DEVICE_I2S_EVENT_UNSPECIFIED;

    return event;
}

static int tsb_i2s_irq_so_err_handler(int irq, void *context, void *priv)
{
    struct tsb_i2s_info *info = device_get_private(saved_dev);
    enum device_i2s_event event;
    uint32_t intstat;

    intstat = tsb_i2s_read(info, TSB_I2S_BLOCK_SO, TSB_I2S_REG_INTSTAT);
    event = tsb_i2s_intstat2event(intstat);

    if (event == DEVICE_I2S_EVENT_NONE) {
        tsb_i2s_clear_irqs(info, TSB_I2S_BLOCK_SO, intstat);
        return OK;
    }

    tsb_i2s_stop_transmitter(info, 1);

    if (info->tx_callback)
        info->tx_callback(info->tx_rb, event, info->tx_arg);

    return OK;
}

static int tsb_i2s_irq_si_err_handler(int irq, void *context, void *priv)
{
    struct tsb_i2s_info *info = device_get_private(saved_dev);
    enum device_i2s_event event;
    uint32_t intstat;

    intstat = tsb_i2s_read(info, TSB_I2S_BLOCK_SI, TSB_I2S_REG_INTSTAT);
    event = tsb_i2s_intstat2event(intstat);

    if (event == DEVICE_I2S_EVENT_NONE) {
        tsb_i2s_clear_irqs(info, TSB_I2S_BLOCK_SI, intstat);
        return OK;
    }

    tsb_i2s_stop_receiver(info, 1);

    if (info->rx_callback)
        info->rx_callback(info->rx_rb, event, info->rx_arg);

    return OK;
}

static int tsb_i2s_op_get_delay_receiver(struct device *dev,
                                         uint32_t *processing_delay)
{
    *processing_delay = 0; /* TODO: Make accurate guess for this */

    return 0;
}

static int tsb_i2s_op_get_delay_transmitter(struct device *dev,
                                            uint32_t *processing_delay)
{
    *processing_delay = 0; /* TODO: Make accurate guess for this */

    return 0;
}

static int tsb_i2s_op_get_caps(struct device *dev,
                               uint8_t clk_role,
                               struct device_i2s_pcm *pcm,
                               struct device_i2s_dai *dai)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret;
    uint32_t mclk_freq;


    ret = tsb_i2s_verify_pcm_support(clk_role, pcm);
    if (ret) {
        return ret;
    }

    if (clk_role == DEVICE_I2S_ROLE_MASTER) {

        ret = tsb_i2s_test_mclk(info->pll_dev, pcm);
        if (ret < 0) {
            return ret;
        } else if (ret == 0) {
            /* this should never happen */
            return -EINVAL;
        }
        mclk_freq = ret;
        ret = 0;

        dai->mclk_freq = mclk_freq;

        dai->protocol = TSB_I2S_PROTOCOL_MASK;
        dai->wclk_polarity = TSB_I2S_WCLK_PALARITY_MASK;
        dai->wclk_change_edge = TSB_I2S_WCLK_EDGE_MASK;
        dai->data_rx_edge = TSB_I2S_RXCLK_EDGE_MASK;
        dai->data_tx_edge = TSB_I2S_TXCLK_EDGE_MASK;
    } else {
        if (dai->mclk_freq > TSB_I2S_MCLK_MAX) {
            return -EINVAL;
        }

        dai->protocol = TSB_I2S_PROTOCOL_MASK;
        dai->wclk_polarity = TSB_I2S_WCLK_PALARITY_MASK;
        dai->wclk_change_edge = TSB_I2S_WCLK_EDGE_MASK;
        dai->data_rx_edge = TSB_I2S_RXCLK_EDGE_MASK;
        dai->data_tx_edge = TSB_I2S_TXCLK_EDGE_MASK;
    }

    return OK;
};

static int tsb_i2s_op_set_config(struct device *dev,
                               uint8_t clk_role,
                               struct device_i2s_pcm *pcm,
                               struct device_i2s_dai *dai)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret;

    ret = tsb_i2s_verify_pcm_support(clk_role, pcm);
    if (ret) {
        return ret;
    }

    /* Check only one bit is set for all bitfields */
    if ( !ONE_BIT_IS_SET(dai->wclk_polarity)  ||
         !ONE_BIT_IS_SET(dai->wclk_change_edge)  ||
         !ONE_BIT_IS_SET(dai->data_rx_edge)  ||
         !ONE_BIT_IS_SET(dai->data_tx_edge)) {

        return -ERANGE;
    }

    /* Check that bit field settings match our capabilities */
    if (!(dai->protocol & TSB_I2S_PROTOCOL_MASK) ||
        !(dai->wclk_polarity & TSB_I2S_WCLK_PALARITY_MASK) ||
        !(dai->wclk_change_edge & TSB_I2S_WCLK_EDGE_MASK) ||
        !(dai->data_rx_edge & TSB_I2S_RXCLK_EDGE_MASK) ||
        !(dai->data_tx_edge & TSB_I2S_TXCLK_EDGE_MASK)) {

        return -ERANGE;
    }

    if (clk_role == DEVICE_I2S_ROLE_MASTER) {
        ret = tsb_i2s_test_mclk(info->pll_dev, pcm);
        if (ret < 0) {
            return ret;
        } else if (ret == 0) {
            /* this should never happen */
            return -EINVAL;
        }
    } else {
        if (dai->mclk_freq > TSB_I2S_MCLK_MAX) {
            return -EINVAL;
        }
    }

    memcpy(&info->pcm, pcm, sizeof(struct device_i2s_pcm));
    memcpy(&info->dai, dai, sizeof(struct device_i2s_dai));
    if (clk_role == DEVICE_I2S_ROLE_MASTER) {
        info->mclk_role = DEVICE_I2S_ROLE_MASTER;
        info->bclk_role = DEVICE_I2S_ROLE_MASTER;
        info->wclk_role = DEVICE_I2S_ROLE_MASTER;
    } else {
        info->mclk_role = DEVICE_I2S_ROLE_SLAVE;
        info->bclk_role = DEVICE_I2S_ROLE_SLAVE;
        info->wclk_role = DEVICE_I2S_ROLE_SLAVE;
    }

    info->flags |= TSB_I2S_FLAG_CONFIGURED;

    return OK;
}


static int tsb_i2s_op_prepare_receiver(struct device *dev,
                                       struct ring_buf *rx_rb,
                                       device_i2s_callback callback, void *arg)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret;

    if (!rx_rb)
        return -EINVAL;

    sem_wait(&info->lock);

    if (!tsb_i2s_device_is_configured(info) || tsb_i2s_rx_is_prepared(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    if (!tsb_i2s_tx_is_prepared(info)) {
        if ((ret = i2s_pm_activity(dev))) {
            goto err_unlock;
        }
#ifndef CONFIG_PM
        tsb_i2s_enable(info);
#endif
        tsb_i2s_reset(info);

        ret = tsb_i2s_config_hw(info);
        if (ret)
            goto err_disable;

        ret = tsb_i2s_start_clocks(info); /* start PLL, MCLK, and BCLK */
        if (ret)
            goto err_disable;
    }

    info->rx_rb = rx_rb;
    info->rx_callback = callback;
    info->rx_arg = arg;

    up_enable_irq(info->sierr_irq);
    up_enable_irq(info->si_irq);

    ret = tsb_i2s_xfer_prepare_receiver(info);
    if (ret) {
        goto err_disable;
    }

    info->flags |= TSB_I2S_FLAG_RX_PREPARED;

    sem_post(&info->lock);

    return 0;

err_disable:
    if (!tsb_i2s_tx_is_prepared(info)) {
        tsb_i2s_reset(info);
#ifndef CONFIG_PM
        tsb_i2s_disable(info);
#endif
    }

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static int tsb_i2s_op_start_receiver(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret = 0;

    sem_wait(&info->lock);

    if (!tsb_i2s_rx_is_prepared(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    ret = tsb_i2s_start_receiver(info);

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static int tsb_i2s_op_stop_receiver(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret = 0;

    sem_wait(&info->lock);

    if (!tsb_i2s_rx_is_active(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    tsb_i2s_stop_receiver(info, 0);

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static int tsb_i2s_op_shutdown_receiver(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret = 0;

    sem_wait(&info->lock);

    if (tsb_i2s_rx_is_active(info)) {
        ret = -EBUSY;
        goto err_unlock;
    }

    if (!tsb_i2s_rx_is_prepared(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    up_disable_irq(info->si_irq);
    up_disable_irq(info->sierr_irq);

    if (tsb_i2s_xfer_shutdown_receiver(info)) {
        ret = -EBUSY;
        goto err_unlock;
    }

    if (!tsb_i2s_tx_is_prepared(info)) {
        tsb_i2s_stop_clocks(info);
        tsb_i2s_reset(info);
#ifndef CONFIG_PM
        tsb_i2s_disable(info);
#endif
    }

    info->rx_rb = NULL;
    info->rx_callback = NULL;
    info->rx_arg = NULL;

    info->flags &= ~TSB_I2S_FLAG_RX_PREPARED;

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static int tsb_i2s_op_prepare_transmitter(struct device *dev,
                                          struct ring_buf *tx_rb,
                                          device_i2s_callback callback,
                                          void *arg)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret;

    if (!tx_rb)
        return -EINVAL;

    sem_wait(&info->lock);

    if (!tsb_i2s_device_is_configured(info) || tsb_i2s_tx_is_prepared(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    if (!tsb_i2s_rx_is_prepared(info)) {
        if ((ret = i2s_pm_activity(dev))) {
            goto err_unlock;
        }
#ifndef CONFIG_PM
        tsb_i2s_enable(info);
#endif
        tsb_i2s_reset(info);

        ret = tsb_i2s_config_hw(info);
        if (ret)
            goto err_disable;

        ret = tsb_i2s_start_clocks(info); /* start PLL, MCLK, and BCLK */
        if (ret)
            goto err_disable;
    }

    info->tx_rb = tx_rb;
    info->tx_callback = callback;
    info->tx_arg = arg;

    if (tsb_i2s_xfer_prepare_transmitter(info)) {
        goto err_disable;
    }

    info->flags |= TSB_I2S_FLAG_TX_PREPARED;

    up_enable_irq(info->soerr_irq);
    up_enable_irq(info->so_irq);

    sem_post(&info->lock);

    return 0;

err_disable:
    if (!tsb_i2s_rx_is_prepared(info)) {
        tsb_i2s_reset(info);
#ifndef CONFIG_PM
        tsb_i2s_disable(info);
#endif
    }

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static int tsb_i2s_op_start_transmitter(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret = 0;

    sem_wait(&info->lock);

    if (!tsb_i2s_tx_is_prepared(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    ret = tsb_i2s_start_transmitter(info);

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static int tsb_i2s_op_stop_transmitter(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret = 0;

    sem_wait(&info->lock);

    if (!tsb_i2s_tx_is_active(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    tsb_i2s_stop_transmitter(info, 0);

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static int tsb_i2s_op_shutdown_transmitter(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret = 0;

    sem_wait(&info->lock);

    if (tsb_i2s_tx_is_active(info)) {
        ret = -EBUSY;
        goto err_unlock;
    }

    if (!tsb_i2s_tx_is_prepared(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    up_disable_irq(info->so_irq);
    up_disable_irq(info->soerr_irq);

    if (tsb_i2s_xfer_shutdown_transmitter(info)) {
        ret = -EIO;
        goto err_unlock;
    }

    if (!tsb_i2s_rx_is_prepared(info)) {
        tsb_i2s_stop_clocks(info);
        tsb_i2s_reset(info);
#ifndef CONFIG_PM
        tsb_i2s_disable(info);
#endif
    }

    info->tx_rb = NULL;
    info->tx_callback = NULL;
    info->tx_arg = NULL;

    info->flags &= ~TSB_I2S_FLAG_TX_PREPARED;

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static int tsb_i2s_dev_open(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    int ret = 0;

    sem_wait(&info->lock);

    if (tsb_i2s_device_is_open(info)) {
        ret = -EBUSY;
        goto err_unlock;
    }

    ret = tsb_i2s_xfer_open(info);
    if (!ret) {
        info->flags = TSB_I2S_FLAG_OPEN;
    }

#ifdef CONFIG_PM
    tsb_i2s_enable(info);
    tsb_i2s_reset(info);
#endif

err_unlock:
    sem_post(&info->lock);

    return ret;
}

static void tsb_i2s_dev_close(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);

    sem_wait(&info->lock);

    if (!tsb_i2s_device_is_open(info))
        goto err_unlock;

    if (tsb_i2s_rx_is_active(info))
        tsb_i2s_op_stop_receiver(dev);

    if (tsb_i2s_tx_is_active(info))
        tsb_i2s_op_stop_transmitter(dev);

    if (tsb_i2s_rx_is_prepared(info))
        tsb_i2s_op_shutdown_receiver(dev);

    if (tsb_i2s_tx_is_prepared(info))
        tsb_i2s_op_shutdown_transmitter(dev);

    tsb_i2s_xfer_close(info);

#ifdef CONFIG_PM
    tsb_i2s_reset(info);
    tsb_i2s_disable(info);
#endif

    info->flags = 0;

err_unlock:
    sem_post(&info->lock);
}

static int tsb_i2s_extract_resources(struct device *dev,
                                     struct tsb_i2s_info *info)
{
    struct device_resource *r;

    r = device_resource_get_by_name(dev, DEVICE_RESOURCE_TYPE_REG,
                                    "cg_bridge");
    if (!r)
        return -EINVAL;

    info->cg_base = r->start;

    r = device_resource_get_by_name(dev, DEVICE_RESOURCE_TYPE_REG, "i2slp_sc");
    if (!r)
        return -EINVAL;

    info->sc_base = r->start;

    r = device_resource_get_by_name(dev, DEVICE_RESOURCE_TYPE_REG, "i2slp_so");
    if (!r)
        return -EINVAL;

    info->so_base = r->start;

    r = device_resource_get_by_name(dev, DEVICE_RESOURCE_TYPE_REG, "i2slp_si");
    if (!r)
        return -EINVAL;

    info->si_base = r->start;

    r = device_resource_get_by_name(dev, DEVICE_RESOURCE_TYPE_IRQ, "i2soerr");
    if (!r)
        return -EINVAL;

    info->soerr_irq = (int)r->start;

    r = device_resource_get_by_name(dev, DEVICE_RESOURCE_TYPE_IRQ, "i2so");
    if (!r)
        return -EINVAL;

    info->so_irq = (int)r->start;

    r = device_resource_get_by_name(dev, DEVICE_RESOURCE_TYPE_IRQ, "i2sierr");
    if (!r)
        return -EINVAL;

    info->sierr_irq = (int)r->start;

    r = device_resource_get_by_name(dev, DEVICE_RESOURCE_TYPE_IRQ, "i2si");
    if (!r)
        return -EINVAL;

    info->si_irq = (int)r->start;

    return 0;
}

static int tsb_i2s_suspend(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);

    tsb_i2s_reset(info);
    tsb_i2s_disable(info);

    return 0;
}

static int tsb_i2s_poweroff(struct device *dev)
{
    return tsb_i2s_suspend(dev);
}

static int tsb_i2s_resume(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);

    tsb_i2s_enable(info);
    tsb_i2s_reset(info);

    return 0;
}

#ifdef CONFIG_PM
static void tsb_i2s_pm_notify(struct pm_callback_s *cb,
                              enum pm_state_e pmstate)
{
    struct device *dev;
    irqstate_t flags;

    dev = cb->priv;

    flags = irqsave();

    switch (pmstate) {
    case PM_NORMAL:
        if (tsb_pm_getstate() == PM_SLEEP) {
            tsb_i2s_resume(dev);
        }
        break;
    case PM_IDLE:
    case PM_STANDBY:
        /* Nothing to do in idle or standby. */
        break;
    case PM_SLEEP:
        tsb_i2s_suspend(dev);
        break;
    default:
        /* Can never happen. */
        PANIC();
    }

    irqrestore(flags);
}

static int tsb_i2s_pm_prepare(struct pm_callback_s *cb,
                              enum pm_state_e pmstate)
{
    struct device *dev;
    struct tsb_i2s_info *info = NULL;
    irqstate_t flags;

    dev = cb->priv;
    info = device_get_private(dev);

    flags = irqsave();

    switch (pmstate) {
    case PM_NORMAL:
        break;
    case PM_IDLE:
    case PM_STANDBY:
        /* Nothing to do in idle or standby. */
        break;
    case PM_SLEEP:
        if (tsb_i2s_rx_is_prepared(info) || tsb_i2s_tx_is_prepared(info)) {
            irqrestore(flags);
            return -EBUSY;
        }
        break;
    default:
        /* Can never happen. */
        PANIC();
    }

    irqrestore(flags);

    return OK;
}
#else
static void tsb_i2s_pm_notify(struct pm_callback_s *cb,
                              enum pm_state_e pmstate)
{

}

static int tsb_i2s_pm_prepare(struct pm_callback_s *cb,
                              enum pm_state_e pmstate)
{
    return OK;
}
#endif

static int tsb_i2s_dev_probe(struct device *dev)
{
    struct tsb_i2s_info *info;
    irqstate_t flags;
    int ret;

    info = zalloc(sizeof(*info));
    if (!info)
        return -ENOMEM;

    ret = tsb_i2s_extract_resources(dev, info);
    if (ret)
        goto err_free_info;

    ret = sem_init(&info->lock, 0, 1);
    if (ret != OK)
        goto err_free_info;

    flags = irqsave();

    ret = irq_attach(info->soerr_irq, tsb_i2s_irq_so_err_handler, NULL);
    if (ret != OK)
        goto err_irqrestore;

    ret = irq_attach(info->sierr_irq, tsb_i2s_irq_si_err_handler, NULL);
    if (ret != OK)
        goto err_detach_soerr_irq;

    ret = tsb_i2s_xfer_irq_attach(info);
    if (ret != OK)
        goto err_detach_sioerr_irq;

    ret = tsb_pin_request(PIN_I2S0);
    if (ret) {
        lowsyslog("I2S: cannot get ownership of PIN_I2S0\n");
        goto err_detach_serr_irq;
    }

    info->dev = dev;
    device_set_private(dev, info);
    saved_dev = dev;

    irqrestore(flags);

    ret = tsb_pm_register(tsb_i2s_pm_prepare, tsb_i2s_pm_notify, dev);
    if (ret < 0) {
        goto err_pm;
    }

    return 0;

err_pm:
    tsb_pin_release(PIN_I2S0);
err_detach_serr_irq:
    tsb_i2s_xfer_irq_detach(info);
err_detach_sioerr_irq:
    irq_detach(info->sierr_irq);
err_detach_soerr_irq:
    irq_detach(info->soerr_irq);
err_irqrestore:
    irqrestore(flags);
    sem_destroy(&info->lock);
err_free_info:
    memset(info, 0, sizeof(*info));
    free(info);

    return ret;
}

static void tsb_i2s_dev_remove(struct device *dev)
{
    struct tsb_i2s_info *info = device_get_private(dev);
    irqstate_t flags;

    tsb_pin_release(PIN_I2S0);

    flags = irqsave();

    irq_detach(info->si_irq);
    irq_detach(info->sierr_irq);
    irq_detach(info->so_irq);
    irq_detach(info->soerr_irq);

    saved_dev = NULL;
    device_set_private(dev, NULL);

    irqrestore(flags);

    sem_destroy(&info->lock);

    memset(info, 0, sizeof(*info));
    free(info);
}

static struct device_pm_ops tsb_i2s_pm_ops = {
    .suspend    = tsb_i2s_suspend,
    .poweroff   = tsb_i2s_poweroff,
    .resume     = tsb_i2s_resume,
};

static struct device_i2s_type_ops tsb_i2s_type_ops = {
    .get_caps                     = tsb_i2s_op_get_caps,
    .set_config                   = tsb_i2s_op_set_config,
    .get_delay_receiver           = tsb_i2s_op_get_delay_receiver,
    .prepare_receiver             = tsb_i2s_op_prepare_receiver,
    .start_receiver               = tsb_i2s_op_start_receiver,
    .stop_receiver                = tsb_i2s_op_stop_receiver,
    .shutdown_receiver            = tsb_i2s_op_shutdown_receiver,
    .get_delay_transmitter        = tsb_i2s_op_get_delay_transmitter,
    .prepare_transmitter          = tsb_i2s_op_prepare_transmitter,
    .start_transmitter            = tsb_i2s_op_start_transmitter,
    .stop_transmitter             = tsb_i2s_op_stop_transmitter,
    .shutdown_transmitter         = tsb_i2s_op_shutdown_transmitter,
};

static struct device_driver_ops tsb_i2s_driver_ops = {
    .probe          = tsb_i2s_dev_probe,
    .remove         = tsb_i2s_dev_remove,
    .open           = tsb_i2s_dev_open,
    .close          = tsb_i2s_dev_close,
    .type_ops       = &tsb_i2s_type_ops,
};

struct device_driver tsb_i2s_driver = {
    .type   = DEVICE_TYPE_I2S_HW,
    .name   = "tsb_i2s",
    .desc   = "TSB I2S Driver",
    .ops    = &tsb_i2s_driver_ops,
    .pm     = &tsb_i2s_pm_ops,
};
