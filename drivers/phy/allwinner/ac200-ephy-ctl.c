// SPDX-License-Identifier: GPL-2.0+
/**
 * syscon driver to control and configure AC200 Ethernet PHY
 * Copyright (c) 2022 Arm Ltd.
 *
 * TODO's and questions:
 * =========================
 * - This driver is something like a syscon driver, as it controls various
 *   bits and registers that effect other devices (the actual PHY). It's
 *   unclear where it should live, though:
 *   - it could be integrated into the MFD driver, but this looks messy
 *   - it could live at the current location (drivers/phy/allwinner), but that
 *     sounds wrong
 *   - it could be a separate file, but in drivers/mfd
 *   - anything else
 *
 */

#include <dt-bindings/gpio/gpio.h>

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>

/* macros for system ephy control 0 register */
#define AC200_SYS_EPHY_CTL0		0x0014
#define AC200_EPHY_RESET_INVALID	BIT(0)
#define AC200_EPHY_SYSCLK_GATING	1

/* macros for system ephy control 1 register */
#define AC200_SYS_EPHY_CTL1		0x0016
#define AC200_EPHY_E_EPHY_MII_IO_EN	BIT(0)
#define AC200_EPHY_E_LNK_LED_IO_EN	BIT(1)
#define AC200_EPHY_E_SPD_LED_IO_EN	BIT(2)
#define AC200_EPHY_E_DPX_LED_IO_EN	BIT(3)

/* macros for ephy control register */
#define AC200_EPHY_CTL			0x6000
#define AC200_EPHY_SHUTDOWN		BIT(0)
#define AC200_EPHY_LED_POL		BIT(1)
#define AC200_EPHY_CLK_SEL		BIT(2)
#define AC200_EPHY_ADDR(x)		(((x) & 0x1F) << 4)
#define AC200_EPHY_XMII_SEL		BIT(11)
#define AC200_EPHY_CALIB(x)		(((x) & 0xF) << 12)

struct ac200_ephy_ctl_dev {
	struct reset_controller_dev	rcdev;
	struct clk_hw			*gate_clk;
	struct regmap			*regmap;
};

static struct ac200_ephy_ctl_dev *to_phy_dev(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct ac200_ephy_ctl_dev, rcdev);
}

static int ephy_ctl_reset(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ac200_ephy_ctl_dev *ac200 = to_phy_dev(rcdev);
	int ret;

	ret = regmap_clear_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
				AC200_EPHY_RESET_INVALID);
	if (ret)
		return ret;

	/* This is going via I2C, so there is plenty of built-in delay. */
	return regmap_set_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
			       AC200_EPHY_RESET_INVALID);
}

static int ephy_ctl_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ac200_ephy_ctl_dev *ac200 = to_phy_dev(rcdev);

	return regmap_clear_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
				 AC200_EPHY_RESET_INVALID);
}

static int ephy_ctl_deassert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct ac200_ephy_ctl_dev *ac200 = to_phy_dev(rcdev);

	return regmap_set_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
			       AC200_EPHY_RESET_INVALID);
}

static int ephy_ctl_status(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ac200_ephy_ctl_dev *ac200 = to_phy_dev(rcdev);

	return regmap_test_bits(ac200->regmap, AC200_SYS_EPHY_CTL0,
				AC200_EPHY_RESET_INVALID);
}

static int ephy_ctl_reset_of_xlate(struct reset_controller_dev *rcdev,
				   const struct of_phandle_args *reset_spec)
{
	if (WARN_ON(reset_spec->args_count != 0))
		return -EINVAL;

	return 0;
}

const struct reset_control_ops ephy_ctl_reset_ops = {
	.assert		= ephy_ctl_assert,
	.deassert	= ephy_ctl_deassert,
	.reset		= ephy_ctl_reset,
	.status		= ephy_ctl_status,
};

static void ac200_ephy_ctl_disable(struct ac200_ephy_ctl_dev *priv)
{
	regmap_write(priv->regmap, AC200_EPHY_CTL, AC200_EPHY_SHUTDOWN);
	regmap_write(priv->regmap, AC200_SYS_EPHY_CTL1, 0);
	regmap_write(priv->regmap, AC200_SYS_EPHY_CTL0, 0);
}

static int ac200_ephy_ctl_probe(struct platform_device *pdev)
{
	struct reset_controller_dev *rcdev;
	struct device *dev = &pdev->dev;
	struct ac200_ephy_ctl_dev *priv;
	struct nvmem_cell *calcell;
	const char *parent_name;
	phy_interface_t phy_if;
	u16 *caldata, ephy_ctl;
	struct clk *clk;
	size_t callen;
	u32 value;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->regmap = dev_get_regmap(dev->parent, NULL);
	if (!priv->regmap)
		return -EPROBE_DEFER;

	calcell = devm_nvmem_cell_get(dev, "calibration");
	if (IS_ERR(calcell))
		return dev_err_probe(dev, PTR_ERR(calcell),
				     "Unable to find calibration data!\n");

	caldata = nvmem_cell_read(calcell, &callen);
	if (IS_ERR(caldata)) {
		dev_err(dev, "Unable to read calibration data!\n");
		return PTR_ERR(caldata);
	}

	if (callen != 2) {
		dev_err(dev, "Calibration data length must be 2 bytes!\n");
		kfree(caldata);
		return -EINVAL;
	}

	ephy_ctl = AC200_EPHY_CALIB(*caldata + 3);
	kfree(caldata);

	ret = of_get_phy_mode(dev->of_node, &phy_if);
	if (ret) {
		dev_err(dev, "Unable to read PHY connection mode\n");
		return ret;
	}

	switch (phy_if) {
	case PHY_INTERFACE_MODE_MII:
		break;
	case PHY_INTERFACE_MODE_RMII:
		ephy_ctl |= AC200_EPHY_XMII_SEL;
		break;
	default:
		dev_err(dev, "Illegal PHY connection mode (%d), only RMII or MII supported\n",
			phy_if);
		return -EINVAL;
	}

	ret = of_property_read_u32(dev->of_node, "x-powers,led-polarity",
				   &value);
	if (ret) {
		dev_err(dev, "Unable to read LED polarity setting\n");
		return ret;
	}

	if (value == GPIO_ACTIVE_LOW)
		ephy_ctl |= AC200_EPHY_LED_POL;

	ret = of_property_read_u32(dev->of_node, "phy-address", &value);
	if (ret) {
		dev_err(dev, "Unable to read PHY address value\n");
		return ret;
	}

	ephy_ctl |= AC200_EPHY_ADDR(value);

	clk = clk_get(dev->parent, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Unable to obtain the clock\n");

	if (clk_get_rate(clk) == 24000000)
		ephy_ctl |= AC200_EPHY_CLK_SEL;

	clk_put(clk);

	/* Assert reset and gate clock, to disable PHY for now */
	ret = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL0, 0);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL1,
			   AC200_EPHY_E_EPHY_MII_IO_EN |
			   AC200_EPHY_E_LNK_LED_IO_EN |
			   AC200_EPHY_E_SPD_LED_IO_EN |
			   AC200_EPHY_E_DPX_LED_IO_EN);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, AC200_EPHY_CTL, ephy_ctl);
	if (ret)
		return ret;

	rcdev = &priv->rcdev;
	rcdev->owner = dev->driver->owner;
	rcdev->nr_resets = 1;
	rcdev->ops = &ephy_ctl_reset_ops;
	rcdev->of_node = dev->of_node;
	rcdev->of_reset_n_cells = 0;
	rcdev->of_xlate = ephy_ctl_reset_of_xlate;

	ret = devm_reset_controller_register(dev, rcdev);
	if (ret) {
		dev_err(dev, "Unable to register reset controller: %d\n", ret);
		goto err_disable_ephy;
	}

	parent_name = of_clk_get_parent_name(dev->parent->of_node, 0);
	priv->gate_clk = devm_clk_hw_register_regmap_gate(dev,
				"ac200-ephy-ctl-gate", parent_name, 0,
				priv->regmap, AC200_SYS_EPHY_CTL0,
				AC200_EPHY_SYSCLK_GATING, 0);
	if (IS_ERR(priv->gate_clk)) {
		ret = PTR_ERR(priv->gate_clk);
		dev_err(dev, "Unable to register gate clock: %d\n", ret);
		goto err_disable_ephy;
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					  priv->gate_clk);
	if (ret) {
		dev_err(dev, "Unable to register clock provider: %d\n", ret);
		goto err_disable_ephy;
	}

	return 0;

err_disable_ephy:
	ac200_ephy_ctl_disable(priv);

	return ret;
}

static int ac200_ephy_ctl_remove(struct platform_device *pdev)
{
	struct ac200_ephy_ctl_dev *priv = platform_get_drvdata(pdev);

	ac200_ephy_ctl_disable(priv);

	return 0;
}

static const struct of_device_id ac200_ephy_ctl_match[] = {
	{ .compatible = "x-powers,ac200-ephy-ctl" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ac200_ephy_ctl_match);

static struct platform_driver ac200_ephy_ctl_driver = {
	.probe		= ac200_ephy_ctl_probe,
	.remove		= ac200_ephy_ctl_remove,
	.driver		= {
		.name		= "ac200-ephy-ctl",
		.of_match_table	= ac200_ephy_ctl_match,
	},
};
module_platform_driver(ac200_ephy_ctl_driver);

MODULE_AUTHOR("Andre Przywara <andre.przywara@arm.com>");
MODULE_DESCRIPTION("AC200 Ethernet PHY control driver");
MODULE_LICENSE("GPL");
