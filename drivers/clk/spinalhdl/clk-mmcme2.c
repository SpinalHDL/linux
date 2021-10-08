// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clkout driver for Rockchip RK808
 *
 * Copyright (c) 2014, Fuzhou Rockchip Electronics Co., Ltd
 *
 * Author:Chris Zhong <zyw@rock-chips.com>
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/io.h>

#define DIVIN 0x16
#define CLKOUT0_R1 0x08
#define CLKOUT0_R2 0x09
#define CLKFBOUT_R1 0x14
#define CLKFBOUT_R2 0x15

#define LOW_TIME(x) (((x)&0x3F) << 0)
#define HIGH_TIME(x) (((x)&0x3F) << 6)

#define FRAC(x) (((x)&0x7) << 12)
#define FRAC_EN (1 << 11)
#define FRAC_WF_R (1 << 10)

struct spinal_mmcme2_state {
	u32 mul;
	u32 div;
	u32 div0;
	u32 clkout0;
};

struct spinal_mmcme2 {
	//    struct spinal *spinal;
	void __iomem *base;
	struct clk_hw clkout0;
	struct platform_device *pdev;
	struct spinal_mmcme2_state state;
};

#define dbus_write(data, reg) writel(data, spinal_mmcme2->base + reg * 4)
#define dbus_read(reg) readl(spinal_mmcme2->base + reg * 4)

#define FRAC_FACTOR 1

void spinal_mmcme2_solve(struct clk_hw *hw, unsigned long rate,
			 unsigned long *parent_rate,
			 struct spinal_mmcme2_state *result)
{
	u32 mul;
	u32 div;
	u32 div0;
	u64 best_score = -1;

	for (div = 2; div < 106; div += 2) {
		u32 clkin = *parent_rate / div;
		if (clkin < 10000000)
			break;
		for (mul = 2 * FRAC_FACTOR; mul < 63 * FRAC_FACTOR; mul++) {
			u64 vco_frac = ((u64)clkin) * mul;
			u64 vco = vco_frac / FRAC_FACTOR;
			u64 score;
			u32 clkout0;
			if (vco < 600000000)
				continue;
			if (vco > 1200000000)
				continue;
			div0 = div_u64(vco * 2 * FRAC_FACTOR, rate);
			div0 += (div0 & 1) ? 1 : 0;
			div0 >>= 1;
			if (div0 < 2 * FRAC_FACTOR || div0 > 127 * FRAC_FACTOR)
				continue;
			clkout0 = div_u64(vco_frac, div0);
			score = abs((s32)(clkout0 - rate));
			if (score < best_score) {
				result->mul = mul;
				result->div = div;
				result->div0 = div0;
				result->clkout0 = clkout0;
				best_score = score;
			}
		}
	}
}

static struct clk_hw *of_clk_spinal_get(struct of_phandle_args *clkspec,
					void *data)
{
	struct spinal_mmcme2 *spinal_mmcme2 = data;

	return &spinal_mmcme2->clkout0;
}

int spinal_mmcme2_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate)
{
	struct spinal_mmcme2 *spinal_mmcme2 =
		container_of(hw, struct spinal_mmcme2, clkout0);
	struct spinal_mmcme2_state state;
	u32 tmp;
	u32 clkout0_r1, clkfbout_r1, divin;
	dev_info(&spinal_mmcme2->pdev->dev, "Set frequency as %lu hz\n", rate);

	spinal_mmcme2_solve(hw, rate, &parent_rate, &state);

	divin = dbus_read(DIVIN);
	clkout0_r1 = dbus_read(CLKOUT0_R1);
	clkfbout_r1 = dbus_read(CLKFBOUT_R1);

	dbus_write(HIGH_TIME(63) | LOW_TIME(63) | (clkout0_r1 & 0xF000),
		   CLKOUT0_R1);
	dbus_write(HIGH_TIME(63) | LOW_TIME(63) | (divin & 0xE000), DIVIN);

	tmp = state.mul / FRAC_FACTOR;
	dbus_write(HIGH_TIME(tmp / 2) | LOW_TIME(tmp / 2 + (tmp % 2)) |
			   (clkfbout_r1 & 0xF000),
		   CLKFBOUT_R1);

	tmp = state.div0 / FRAC_FACTOR;
	dbus_write(HIGH_TIME(tmp / 2) | LOW_TIME(tmp / 2 + (tmp % 2)) |
			   (clkout0_r1 & 0xF000),
		   CLKOUT0_R1);

	dbus_write(HIGH_TIME(state.div / 2) |
			   LOW_TIME(state.div / 2 + (state.div % 2)) |
			   (divin & 0xE000),
		   DIVIN);

	spinal_mmcme2->state = state;
	return 0;
}

unsigned long spinal_mmcme2_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct spinal_mmcme2 *spinal_mmcme2 =
		container_of(hw, struct spinal_mmcme2, clkout0);
	struct spinal_mmcme2_state *state = &spinal_mmcme2->state;

	return div_u64(((u64)(parent_rate / state->div)) * state->mul,
		       state->div0);
}

long spinal_mmcme2_round_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long *parent_rate)
{
	struct spinal_mmcme2_state state;

	spinal_mmcme2_solve(hw, rate, parent_rate, &state);
	return state.clkout0;
}

static const struct clk_ops spinal_mmcme2_ops = {
	.set_rate = spinal_mmcme2_set_rate,
	.round_rate = spinal_mmcme2_round_rate,
	.recalc_rate = spinal_mmcme2_recalc_rate,
};

const char *miaouMiaou = "periph_clock";
static int spinal_mmcme2_probe(struct platform_device *pdev)
{
	struct clk_init_data init = {};
	struct spinal_mmcme2 *spinal_mmcme2;
	int ret;
	const char *str;
	struct resource *reg;

	spinal_mmcme2 =
		devm_kzalloc(&pdev->dev, sizeof(*spinal_mmcme2), GFP_KERNEL);
	if (!spinal_mmcme2)
		return -ENOMEM;

	reg = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!reg) {
		dev_err(&pdev->dev, "No reg resource\n");
		return -ENXIO;
	}

	spinal_mmcme2->base = devm_ioremap_resource(&pdev->dev, reg);
	if (IS_ERR(spinal_mmcme2->base)) {
		dev_err(&pdev->dev, "Can't ioremap reg\n");
		return -ENXIO;
	}

	spinal_mmcme2->pdev = pdev;

	str = of_clk_get_parent_name(pdev->dev.of_node, 0);

	init.name = "spinal_mmcme2";
	init.parent_names = &str;
	init.num_parents = 1;
	init.ops = &spinal_mmcme2_ops;

	spinal_mmcme2->clkout0.init = &init;

	ret = devm_clk_hw_register(&pdev->dev, &spinal_mmcme2->clkout0);
	if (ret) {
		dev_err(&pdev->dev, "Bad devm_clk_hw_register\n");
		return ret;
	}

	ret = devm_of_clk_add_hw_provider(&pdev->dev, of_clk_spinal_get,
					  spinal_mmcme2);

	if (ret) {
		dev_err(&pdev->dev, "could not add hw_provider: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "Probed !\n");
	return 0;
}

static const struct of_device_id spinal_mmcme2_of_match[] = {
	{
		.compatible = "spinal,clk-mmcme2",
	}, /* Keep for legacy */
	{
		.compatible = "spinalhdl,clk-mmcme2-1.0",
	},
	{}
};
MODULE_DEVICE_TABLE(of, spinal_mmcme2_of_match);

static struct platform_driver spinal_mmcme2_driver = {
    .probe = spinal_mmcme2_probe,
    .driver     = {
        .name   = "spinal-clk-mmcme2",
        .of_match_table = spinal_mmcme2_of_match,
    },
};

static int __init spinal_mmcme2_init(void)
{
	return platform_driver_register(&spinal_mmcme2_driver);
}
core_initcall(spinal_mmcme2_init);

MODULE_DESCRIPTION("Clkout driver for the spinal series PMICs");
MODULE_AUTHOR("Chris Zhong <zyw@rock-chips.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:spinalhdl-mmcme2");
