/*
 * Core driver for the imx pin controller
 *
 * Copyright (C) 2012 Freescale Semiconductor, Inc.
 * Copyright (C) 2012 Linaro Ltd.
 *
 * Author: Dong Aisheng <dong.aisheng@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/slab.h>

#include "core.h"
#include "pinctrl-imx.h"

/* The bits in CONFIG cell defined in binding doc*/
#define IMX_NO_PAD_CTL	0x80000000	/* no pin config need */
#define IMX_PAD_SION 0x40000000		/* set SION */

/**
 * @dev: a pointer back to containing device
 * @base: the offset to the controller in virtual memory
 */
struct imx_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctl;
	void __iomem *base;
	const struct imx_pinctrl_soc_info *info;
};

static const inline struct imx_pin_group
*imx_pinctrl_find_group_by_name(const struct imx_pinctrl_soc_info *info,
				const char *name)
{
	const struct imx_pin_group *grp = NULL;
	int i;

	for (i = 0; i < info->ngroups; i++) {
		if (!strcmp(info->groups[i].name, name)) {
			grp = &info->groups[i];
			break;
		}
	}

	return grp;
}

static int imx_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;

	return info->ngroups;
}

static const char *imx_get_group_name(struct pinctrl_dev *pctldev,
				       unsigned selector)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;

	return info->groups[selector].name;
}

static int imx_get_group_pins(struct pinctrl_dev *pctldev, unsigned selector,
			       const unsigned **pins,
			       unsigned *npins)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;

	if (selector >= info->ngroups)
		return -EINVAL;

	*pins = info->groups[selector].pin_ids;
	*npins = info->groups[selector].npins;

	return 0;
}

static int imx_dt_node_to_map(struct pinctrl_dev *pctldev,
			struct device_node *np,
			struct pinctrl_map **map, unsigned *num_maps)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;
	const struct imx_pin_group *grp;
	struct pinctrl_map *new_map;
	struct device_node *parent;
	int map_num = 1;
	int i, j;

	/*
	 * first find the group of this node and check if we need create
	 * config maps for pins
	 */
	grp = imx_pinctrl_find_group_by_name(info, np->name);
	if (!grp) {
		dev_err(info->dev, "unable to find group for node %s\n",
			np->name);
		return -EINVAL;
	}

	for (i = 0; i < grp->npins; i++) {
		if (!(grp->pins[i].config & IMX_NO_PAD_CTL))
			map_num++;
	}

	new_map = kmalloc(sizeof(struct pinctrl_map) * map_num, GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	*map = new_map;
	*num_maps = map_num;

	/* create mux map */
	parent = of_get_parent(np);
	if (!parent) {
		kfree(new_map);
		return -EINVAL;
	}
	new_map[0].type = PIN_MAP_TYPE_MUX_GROUP;
	new_map[0].data.mux.function = parent->name;
	new_map[0].data.mux.group = np->name;
	of_node_put(parent);

	/* create config map */
	new_map++;
	for (i = j = 0; i < grp->npins; i++) {
		if (!(grp->pins[i].config & IMX_NO_PAD_CTL)) {
			new_map[j].type = PIN_MAP_TYPE_CONFIGS_PIN;
			new_map[j].data.configs.group_or_pin =
					pin_get_name(pctldev, grp->pins[i].pin);
			new_map[j].data.configs.configs = &grp->pins[i].config;
			new_map[j].data.configs.num_configs = 1;
			j++;
		}
	}

	dev_dbg(pctldev->dev, "maps: function %s group %s num %d\n",
		(*map)->data.mux.function, (*map)->data.mux.group, map_num);

	return 0;
}

static void imx_dt_free_map(struct pinctrl_dev *pctldev,
				struct pinctrl_map *map, unsigned num_maps)
{
	kfree(map);
}

static const struct pinctrl_ops imx_pctrl_ops = {
	.get_groups_count = imx_get_groups_count,
	.get_group_name = imx_get_group_name,
	.get_group_pins = imx_get_group_pins,
	.dt_node_to_map = imx_dt_node_to_map,
	.dt_free_map = imx_dt_free_map,

};

static int imx_pmx_enable(struct pinctrl_dev *pctldev, unsigned selector,
			   unsigned group)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;
	const struct imx_pin_reg *pin_reg;
	unsigned int npins, pin_id;
	int i;
	struct imx_pin_group *grp;

	/*
	 * Configure the mux mode for each pin in the group for a specific
	 * function.
	 */
	grp = &info->groups[group];
	npins = grp->npins;

	dev_dbg(ipctl->dev, "enable function %s group %s\n",
		info->functions[selector].name, grp->name);

	for (i = 0; i < npins; i++) {
		struct imx_pin *pin = &grp->pins[i];
		pin_id = pin->pin;
		pin_reg = &info->pin_regs[pin_id];

		if (!(info->flags & ZERO_OFFSET_VALID) && !pin_reg->mux_reg) {
			dev_err(ipctl->dev, "Pin(%s) does not support mux function\n",
				info->pins[pin_id].name);
			return -EINVAL;
		}

		if (info->flags & SHARE_MUX_CONF_REG) {
			u32 reg;
			reg = readl(ipctl->base + pin_reg->mux_reg);
			reg &= ~(0x7 << 20);
			reg |= (pin->mux_mode << 20);
			writel(reg, ipctl->base + pin_reg->mux_reg);
		} else {
			writel(pin->mux_mode, ipctl->base + pin_reg->mux_reg);
		}
		dev_dbg(ipctl->dev, "write: offset 0x%x val 0x%x\n",
			pin_reg->mux_reg, pin->mux_mode);

		/*
		 * If the select input value begins with 0xff, it's a quirky
		 * select input and the value should be interpreted as below.
		 *     31     23      15      7        0
		 *     | 0xff | shift | width | select |
		 * It's used to work around the problem that the select
		 * input for some pin is not implemented in the select
		 * input register but in some general purpose register.
		 * We encode the select input value, width and shift of
		 * the bit field into input_val cell of pin function ID
		 * in device tree, and then decode them here for setting
		 * up the select input bits in general purpose register.
		 */
		if (pin->input_val >> 24 == 0xff) {
			u32 val = pin->input_val;
			u8 select = val & 0xff;
			u8 width = (val >> 8) & 0xff;
			u8 shift = (val >> 16) & 0xff;
			u32 mask = ((1 << width) - 1) << shift;
			/*
			 * The input_reg[i] here is actually some IOMUXC general
			 * purpose register, not regular select input register.
			 */
			val = readl(ipctl->base + pin->input_reg);
			val &= ~mask;
			val |= select << shift;
			writel(val, ipctl->base + pin->input_reg);
		} else if (pin->input_reg) {
			/*
			 * Regular select input register can never be at offset
			 * 0, and we only print register value for regular case.
			 */
			writel(pin->input_val, ipctl->base + pin->input_reg);
			dev_dbg(ipctl->dev,
				"==>select_input: offset 0x%x val 0x%x\n",
				pin->input_reg, pin->input_val);
		}
	}

	return 0;
}

static int imx_pmx_get_funcs_count(struct pinctrl_dev *pctldev)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;

	return info->nfunctions;
}

static const char *imx_pmx_get_func_name(struct pinctrl_dev *pctldev,
					  unsigned selector)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;

	return info->functions[selector].name;
}

static int imx_pmx_get_groups(struct pinctrl_dev *pctldev, unsigned selector,
			       const char * const **groups,
			       unsigned * const num_groups)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;

	*groups = info->functions[selector].groups;
	*num_groups = info->functions[selector].num_groups;

	return 0;
}

static const struct pinmux_ops imx_pmx_ops = {
	.get_functions_count = imx_pmx_get_funcs_count,
	.get_function_name = imx_pmx_get_func_name,
	.get_function_groups = imx_pmx_get_groups,
	.enable = imx_pmx_enable,
};

static int imx_pinconf_get(struct pinctrl_dev *pctldev,
			     unsigned pin_id, unsigned long *config)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;
	const struct imx_pin_reg *pin_reg = &info->pin_regs[pin_id];

	if (!(info->flags & ZERO_OFFSET_VALID) && !pin_reg->conf_reg) {
		dev_err(info->dev, "Pin(%s) does not support config function\n",
			info->pins[pin_id].name);
		return -EINVAL;
	}

	*config = readl(ipctl->base + pin_reg->conf_reg);

	if (info->flags & SHARE_MUX_CONF_REG)
		*config &= 0xffff;

	return 0;
}

static int imx_pinconf_set(struct pinctrl_dev *pctldev,
			     unsigned pin_id, unsigned long *configs,
			     unsigned num_configs)
{
	struct imx_pinctrl *ipctl = pinctrl_dev_get_drvdata(pctldev);
	const struct imx_pinctrl_soc_info *info = ipctl->info;
	const struct imx_pin_reg *pin_reg = &info->pin_regs[pin_id];
	int i;

	if (!(info->flags & ZERO_OFFSET_VALID) && !pin_reg->conf_reg) {
		dev_err(info->dev, "Pin(%s) does not support config function\n",
			info->pins[pin_id].name);
		return -EINVAL;
	}

	dev_dbg(ipctl->dev, "pinconf set pin %s\n",
		info->pins[pin_id].name);

	for (i = 0; i < num_configs; i++) {
		if (info->flags & SHARE_MUX_CONF_REG) {
			u32 reg;
			reg = readl(ipctl->base + pin_reg->conf_reg);
			reg &= ~0xffff;
			reg |= configs[i];
			writel(reg, ipctl->base + pin_reg->conf_reg);
		} else {
			writel(configs[i], ipctl->base + pin_reg->conf_reg);
		}
		dev_dbg(ipctl->dev, "write: offset 0x%x val 0x%lx\n",
			pin_reg->conf_reg, configs[i]);
	} /* for each config */

	return 0;
}

static const struct pinconf_ops imx_pinconf_ops = {
	.pin_config_get = imx_pinconf_get,
	.pin_config_set = imx_pinconf_set,
};

static struct pinctrl_desc imx_pinctrl_desc = {
	.pctlops = &imx_pctrl_ops,
	.pmxops = &imx_pmx_ops,
	.confops = &imx_pinconf_ops,
};

/*
 * Each pin represented in fsl,pins consists of 5 u32 PIN_FUNC_ID and
 * 1 u32 CONFIG, so 24 types in total for each pin.
 */
#define FSL_PIN_SIZE 24
#define SHARE_FSL_PIN_SIZE 20

static int imx_pinctrl_parse_groups(struct device_node *np,
				    struct imx_pin_group *grp,
				    struct imx_pinctrl_soc_info *info,
				    u32 index)
{
	int size, pin_size;
	const __be32 *list;
	int i;
	u32 config;
	u32 r = 0;
	u32 mod = 0;

	dev_dbg(info->dev, "group(%d): %s\n", index, np->name);

	if (info->flags & SHARE_MUX_CONF_REG)
		pin_size = SHARE_FSL_PIN_SIZE;
	else
		pin_size = FSL_PIN_SIZE;
	/* Initialise group */
	grp->name = np->name;

	/*
	 * the binding format is fsl,pins = <PIN_FUNC_ID CONFIG ...>,
	 * do sanity check and calculate pins number
	 */
	list = of_get_property(np, "fsl,pins", &size);
	if (!list) {
		dev_err(info->dev, "no fsl,pins property in node %s\n",
			np->name);
		return -EINVAL;
	}

	r = do_udiv32(size, pin_size, &mod);
	/* we do not check return since it's safe node passed down */
	if (!size || mod) {
		dev_err(info->dev, "Invalid fsl,pins property in node %s\n",
			np->name);
		return -EINVAL;
	}

	grp->npins = r;
	grp->pins = devm_kzalloc(info->dev, grp->npins * sizeof(struct imx_pin),
				GFP_KERNEL);
	grp->pin_ids = devm_kzalloc(info->dev, grp->npins * sizeof(unsigned int),
				GFP_KERNEL);
	if (!grp->pins || ! grp->pin_ids)
		return -ENOMEM;

	for (i = 0; i < grp->npins; i++) {
		u32 mux_reg = be32_to_cpu(*list++);
		u32 conf_reg;
		unsigned int pin_id;
		struct imx_pin_reg *pin_reg;
		struct imx_pin *pin = &grp->pins[i];

		if (info->flags & SHARE_MUX_CONF_REG)
			conf_reg = mux_reg;
		else
			conf_reg = be32_to_cpu(*list++);

		pin_id = mux_reg ? mux_reg / 4 : conf_reg / 4;
		pin_reg = &info->pin_regs[pin_id];
		pin->pin = pin_id;
		grp->pin_ids[i] = pin_id;
		pin_reg->mux_reg = mux_reg;
		pin_reg->conf_reg = conf_reg;
		pin->input_reg = be32_to_cpu(*list++);
		pin->mux_mode = be32_to_cpu(*list++);
		pin->input_val = be32_to_cpu(*list++);

		/* SION bit is in mux register */
		config = be32_to_cpu(*list++);
		if (config & IMX_PAD_SION)
			pin->mux_mode |= IOMUXC_CONFIG_SION;
		pin->config = config & ~IMX_PAD_SION;

		dev_dbg(info->dev, "%s: %d 0x%08lx", info->pins[i].name,
				pin->mux_mode, pin->config);
	}

	return 0;
}

static int imx_pinctrl_parse_functions(struct device_node *np,
				       struct imx_pinctrl_soc_info *info,
				       u32 index)
{
	struct device_node *child;
	struct imx_pmx_func *func;
	struct imx_pin_group *grp;
	static u32 grp_index;
	u32 i = 0;

	dev_dbg(info->dev, "parse function(%d): %s\n", index, np->name);

	func = &info->functions[index];

	/* Initialise function */
	func->name = np->name;

	func->num_groups = 0;
	for_each_child_of_node(np, child) {
		++func->num_groups;
	}

	if (func->num_groups <= 0) {
		dev_err(info->dev, "no groups defined in %s\n", np->name);
		return -EINVAL;
	}
	func->groups = devm_kzalloc(info->dev,
			func->num_groups * sizeof(char *), GFP_KERNEL);

	for_each_child_of_node(np, child) {
		func->groups[i] = child->name;
		grp = &info->groups[grp_index++];
		imx_pinctrl_parse_groups(child, grp, info, i++);
	}

	return 0;
}

static int imx_pinctrl_probe_dt(struct vmm_device *dev,
				struct imx_pinctrl_soc_info *info)
{
	struct device_node *np = dev->node;
	struct device_node *child;
	struct device_node *grandchild;
	u32 nfuncs = 0;
	u32 i = 0;

	if (!np)
		return -ENODEV;

	for_each_child_of_node(np, child) {
		++nfuncs;
	}

	if (nfuncs <= 0) {
		dev_err(dev, "no functions defined\n");
		return -EINVAL;
	}

	info->nfunctions = nfuncs;
	info->functions = devm_kzalloc(dev, nfuncs * sizeof(struct imx_pmx_func),
					GFP_KERNEL);
	if (!info->functions)
		return -ENOMEM;

	info->ngroups = 0;
	for_each_child_of_node(np, child) {
		for_each_child_of_node(child, grandchild) {
			++info->ngroups;
		}
	}
	info->groups = devm_kzalloc(dev, info->ngroups * sizeof(struct imx_pin_group),
					GFP_KERNEL);
	if (!info->groups)
		return -ENOMEM;

	for_each_child_of_node(np, child)
		imx_pinctrl_parse_functions(child, info, i++);

	return 0;
}

int imx_pinctrl_probe(struct vmm_device *dev,
		      struct imx_pinctrl_soc_info *info)
{
	struct imx_pinctrl *ipctl;
	int ret;

	if (!info || !info->pins || !info->npins) {
		dev_err(dev, "wrong pinctrl info\n");
		return -EINVAL;
	}
	info->dev = dev;

	/* Create state holders etc for this driver */
	ipctl = devm_kzalloc(dev, sizeof(*ipctl), GFP_KERNEL);
	if (!ipctl)
		return -ENOMEM;

	info->pin_regs = devm_kzalloc(dev, sizeof(*info->pin_regs) *
				      info->npins, GFP_KERNEL);
	if (!info->pin_regs)
		return -ENOMEM;

	ret = vmm_devtree_request_regmap(dev->node,
				(virtual_addr_t *)&ipctl->base, 0,
				"i.MX Pinctrl");
	if (VMM_OK != ret) {
		dev_err(dev, "fail to map registers\n");
		return VMM_ENODATA;
	}

	imx_pinctrl_desc.name = dev_name(dev);
	imx_pinctrl_desc.pins = info->pins;
	imx_pinctrl_desc.npins = info->npins;

	ret = imx_pinctrl_probe_dt(dev, info);
	if (ret) {
		dev_err(dev, "fail to probe dt properties\n");
		vmm_devtree_regunmap_release(dev->node,
				(virtual_addr_t)ipctl->base, 0);
		return ret;
	}

	ipctl->info = info;
	ipctl->dev = info->dev;
	platform_set_drvdata(dev, ipctl);
	ipctl->pctl = pinctrl_register(&imx_pinctrl_desc, dev, ipctl);
	if (!ipctl->pctl) {
		dev_err(dev, "could not register IMX pinctrl driver\n");
		vmm_devtree_regunmap_release(dev->node,
				(virtual_addr_t)ipctl->base, 0);
		return -EINVAL;
	}

	dev_info(dev, "initialized IMX pinctrl driver\n");

	return 0;
}

int imx_pinctrl_remove(struct vmm_device *dev)
{
	struct imx_pinctrl *ipctl = platform_get_drvdata(dev);

	pinctrl_unregister(ipctl->pctl);
	vmm_devtree_regunmap_release(dev->node,
				(virtual_addr_t)ipctl->base, 0);

	return 0;
}
