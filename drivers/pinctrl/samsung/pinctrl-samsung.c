// SPDX-License-Identifier: GPL-2.0+
//
// pin-controller/pin-mux/pin-config/gpio-driver for Samsung's SoC's.
//
// Copyright (c) 2012 Samsung Electronics Co., Ltd.
//		http://www.samsung.com
// Copyright (c) 2012 Linaro Ltd
//		http://www.linaro.org
//
// Author: Thomas Abraham <thomas.ab@samsung.com>
//
// This driver implements the Samsung pinctrl driver. It supports setting up of
// pinmux and pinconf configurations. The gpiolib interface is also included.
// External interrupt (gpio and wakeup) support are not included in this driver
// but provides extensions to which platform specific implementation of the gpio
// and wakeup interrupts can be hooked to.

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/irqdomain.h>
#include <linux/of_device.h>
#include <linux/spinlock.h>
#include <linux/syscore_ops.h>

#include <soc/samsung/exynos-s2i.h>

#include "../core.h"
#include "pinctrl-samsung.h"
#if IS_ENABLED(CONFIG_PINCTRL_SEC_GPIO_DVS)
#include "secgpio_dvs.h"
#endif /* CONFIG_PINCTRL_SEC_GPIO_DVS */

/* maximum number of the memory resources */
#define	SAMSUNG_PINCTRL_NUM_RESOURCES	2

/* list of all possible config options supported */
static struct pin_config {
	const char *property;
	enum pincfg_type param;
} cfg_params[] = {
	{ "samsung,pin-pud", PINCFG_TYPE_PUD },
	{ "samsung,pin-drv", PINCFG_TYPE_DRV },
	{ "samsung,pin-con-pdn", PINCFG_TYPE_CON_PDN },
	{ "samsung,pin-pud-pdn", PINCFG_TYPE_PUD_PDN },
	{ "samsung,pin-val", PINCFG_TYPE_DAT },
};

/* Global list of devices (struct samsung_pinctrl_drv_data) */
static LIST_HEAD(drvdata_list);

static unsigned int pin_base;

static int samsung_get_group_count(struct pinctrl_dev *pctldev)
{
	struct samsung_pinctrl_drv_data *pmx = pinctrl_dev_get_drvdata(pctldev);

	return pmx->nr_groups;
}

static const char *samsung_get_group_name(struct pinctrl_dev *pctldev,
						unsigned group)
{
	struct samsung_pinctrl_drv_data *pmx = pinctrl_dev_get_drvdata(pctldev);

	return pmx->pin_groups[group].name;
}

static int samsung_get_group_pins(struct pinctrl_dev *pctldev,
					unsigned group,
					const unsigned **pins,
					unsigned *num_pins)
{
	struct samsung_pinctrl_drv_data *pmx = pinctrl_dev_get_drvdata(pctldev);

	*pins = pmx->pin_groups[group].pins;
	*num_pins = pmx->pin_groups[group].num_pins;

	return 0;
}

static int reserve_map(struct device *dev, struct pinctrl_map **map,
		       unsigned *reserved_maps, unsigned *num_maps,
		       unsigned reserve)
{
	unsigned old_num = *reserved_maps;
	unsigned new_num = *num_maps + reserve;
	struct pinctrl_map *new_map;

	if (old_num >= new_num)
		return 0;

	new_map = krealloc(*map, sizeof(*new_map) * new_num, GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	memset(new_map + old_num, 0, (new_num - old_num) * sizeof(*new_map));

	*map = new_map;
	*reserved_maps = new_num;

	return 0;
}

static int add_map_mux(struct pinctrl_map **map, unsigned *reserved_maps,
		       unsigned *num_maps, const char *group,
		       const char *function)
{
	if (WARN_ON(*num_maps == *reserved_maps))
		return -ENOSPC;

	(*map)[*num_maps].type = PIN_MAP_TYPE_MUX_GROUP;
	(*map)[*num_maps].data.mux.group = group;
	(*map)[*num_maps].data.mux.function = function;
	(*num_maps)++;

	return 0;
}

static int add_map_configs(struct device *dev, struct pinctrl_map **map,
			   unsigned *reserved_maps, unsigned *num_maps,
			   const char *group, unsigned long *configs,
			   unsigned num_configs)
{
	unsigned long *dup_configs;

	if (WARN_ON(*num_maps == *reserved_maps))
		return -ENOSPC;

	dup_configs = kmemdup(configs, num_configs * sizeof(*dup_configs),
			      GFP_KERNEL);
	if (!dup_configs)
		return -ENOMEM;

	(*map)[*num_maps].type = PIN_MAP_TYPE_CONFIGS_GROUP;
	(*map)[*num_maps].data.configs.group_or_pin = group;
	(*map)[*num_maps].data.configs.configs = dup_configs;
	(*map)[*num_maps].data.configs.num_configs = num_configs;
	(*num_maps)++;

	return 0;
}

static int add_config(struct device *dev, unsigned long **configs,
		      unsigned *num_configs, unsigned long config)
{
	unsigned old_num = *num_configs;
	unsigned new_num = old_num + 1;
	unsigned long *new_configs;

	new_configs = krealloc(*configs, sizeof(*new_configs) * new_num,
			       GFP_KERNEL);
	if (!new_configs)
		return -ENOMEM;

	new_configs[old_num] = config;

	*configs = new_configs;
	*num_configs = new_num;

	return 0;
}

static void samsung_dt_free_map(struct pinctrl_dev *pctldev,
				      struct pinctrl_map *map,
				      unsigned num_maps)
{
	int i;

	for (i = 0; i < num_maps; i++)
		if (map[i].type == PIN_MAP_TYPE_CONFIGS_GROUP)
			kfree(map[i].data.configs.configs);

	kfree(map);
}

static int samsung_dt_subnode_to_map(struct samsung_pinctrl_drv_data *drvdata,
				     struct device *dev,
				     struct device_node *np,
				     struct pinctrl_map **map,
				     unsigned *reserved_maps,
				     unsigned *num_maps)
{
	int ret, i;
	u32 val;
	unsigned long config;
	unsigned long *configs = NULL;
	unsigned num_configs = 0;
	unsigned reserve;
	struct property *prop;
	const char *group;
	bool has_func = false;

	ret = of_property_read_u32(np, "samsung,pin-function", &val);
	if (!ret)
		has_func = true;

	for (i = 0; i < ARRAY_SIZE(cfg_params); i++) {
		ret = of_property_read_u32(np, cfg_params[i].property, &val);
		if (!ret) {
			config = PINCFG_PACK(cfg_params[i].param, val);
			ret = add_config(dev, &configs, &num_configs, config);
			if (ret < 0)
				goto exit;
		/* EINVAL=missing, which is fine since it's optional */
		} else if (ret != -EINVAL) {
			dev_err(dev, "could not parse property %s\n",
				cfg_params[i].property);
		}
	}

	reserve = 0;
	if (has_func)
		reserve++;
	if (num_configs)
		reserve++;
	ret = of_property_count_strings(np, "samsung,pins");
	if (ret < 0) {
		dev_err(dev, "could not parse property samsung,pins\n");
		goto exit;
	}
	reserve *= ret;

	ret = reserve_map(dev, map, reserved_maps, num_maps, reserve);
	if (ret < 0)
		goto exit;

	of_property_for_each_string(np, "samsung,pins", prop, group) {
		if (has_func) {
			ret = add_map_mux(map, reserved_maps,
						num_maps, group, np->full_name);
			if (ret < 0)
				goto exit;
		}

		if (num_configs) {
			ret = add_map_configs(dev, map, reserved_maps,
					      num_maps, group, configs,
					      num_configs);
			if (ret < 0)
				goto exit;
		}
	}

	ret = 0;

exit:
	kfree(configs);
	return ret;
}

static int samsung_dt_node_to_map(struct pinctrl_dev *pctldev,
					struct device_node *np_config,
					struct pinctrl_map **map,
					unsigned *num_maps)
{
	struct samsung_pinctrl_drv_data *drvdata;
	unsigned reserved_maps;
	struct device_node *np;
	int ret;

	drvdata = pinctrl_dev_get_drvdata(pctldev);

	reserved_maps = 0;
	*map = NULL;
	*num_maps = 0;

	if (!of_get_child_count(np_config))
		return samsung_dt_subnode_to_map(drvdata, pctldev->dev,
							np_config, map,
							&reserved_maps,
							num_maps);

	for_each_child_of_node(np_config, np) {
		ret = samsung_dt_subnode_to_map(drvdata, pctldev->dev, np, map,
						&reserved_maps, num_maps);
		if (ret < 0) {
			samsung_dt_free_map(pctldev, *map, *num_maps);
			of_node_put(np);
			return ret;
		}
	}

	return 0;
}

#ifdef CONFIG_DEBUG_FS
/* Forward declaration which can be used by samsung_pin_dbg_show */
static int samsung_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
					unsigned long *config);
static const char * const reg_names[] = {"CON", "DAT", "PUD", "DRV", "CON_PDN",
					 "PUD_PDN"};

static void samsung_pin_dbg_show(struct pinctrl_dev *pctldev,
				struct seq_file *s, unsigned int pin)
{
	enum pincfg_type cfg_type;
	struct samsung_pinctrl_drv_data *drvdata;
	unsigned long config;
	int ret;

	drvdata = pinctrl_dev_get_drvdata(pctldev);

	if (!drvdata->resume)
		return;

	for (cfg_type = 0; cfg_type < PINCFG_TYPE_NUM; cfg_type++) {
		config = PINCFG_PACK(cfg_type, 0);
		ret = samsung_pinconf_get(pctldev, pin, &config);
		if (ret < 0)
			continue;

		seq_printf(s, " %s(0x%lx)", reg_names[cfg_type],
			   PINCFG_UNPACK_VALUE(config));
	}
}
#endif

/* list of pinctrl callbacks for the pinctrl core */
static const struct pinctrl_ops samsung_pctrl_ops = {
	.get_groups_count	= samsung_get_group_count,
	.get_group_name		= samsung_get_group_name,
	.get_group_pins		= samsung_get_group_pins,
	.dt_node_to_map		= samsung_dt_node_to_map,
	.dt_free_map		= samsung_dt_free_map,
#ifdef CONFIG_DEBUG_FS
	.pin_dbg_show		= samsung_pin_dbg_show,
#endif
};

/* check if the selector is a valid pin function selector */
static int samsung_get_functions_count(struct pinctrl_dev *pctldev)
{
	struct samsung_pinctrl_drv_data *drvdata;

	drvdata = pinctrl_dev_get_drvdata(pctldev);
	return drvdata->nr_functions;
}

/* return the name of the pin function specified */
static const char *samsung_pinmux_get_fname(struct pinctrl_dev *pctldev,
						unsigned selector)
{
	struct samsung_pinctrl_drv_data *drvdata;

	drvdata = pinctrl_dev_get_drvdata(pctldev);
	return drvdata->pmx_functions[selector].name;
}

/* return the groups associated for the specified function selector */
static int samsung_pinmux_get_groups(struct pinctrl_dev *pctldev,
		unsigned selector, const char * const **groups,
		unsigned * const num_groups)
{
	struct samsung_pinctrl_drv_data *drvdata;

	drvdata = pinctrl_dev_get_drvdata(pctldev);
	*groups = drvdata->pmx_functions[selector].groups;
	*num_groups = drvdata->pmx_functions[selector].num_groups;
	return 0;
}

/*
 * given a pin number that is local to a pin controller, find out the pin bank
 * and the register base of the pin bank.
 */
static void pin_to_reg_bank(struct samsung_pinctrl_drv_data *drvdata,
			unsigned pin, void __iomem **reg, u32 *offset,
			struct samsung_pin_bank **bank)
{
	struct samsung_pin_bank *b;

	b = drvdata->pin_banks;

	while ((pin >= b->pin_base) &&
			((b->pin_base + b->nr_pins - 1) < pin))
		b++;

	*reg = b->pctl_base + b->pctl_offset;
	*offset = pin - b->pin_base;
	if (bank)
		*bank = b;
}

/* enable or disable a pinmux function */
static void samsung_pinmux_setup(struct pinctrl_dev *pctldev, unsigned selector,
					unsigned group)
{
	struct samsung_pinctrl_drv_data *drvdata;
	const struct samsung_pin_bank_type *type;
	struct samsung_pin_bank *bank;
	void __iomem *reg;
	u32 mask, shift, data, pin_offset;
	unsigned long flags;
	const struct samsung_pmx_func *func;
	const struct samsung_pin_group *grp;

	drvdata = pinctrl_dev_get_drvdata(pctldev);
	func = &drvdata->pmx_functions[selector];
	grp = &drvdata->pin_groups[group];

	pin_to_reg_bank(drvdata, grp->pins[0] - drvdata->pin_base,
			&reg, &pin_offset, &bank);
	type = bank->type;
	mask = (1 << type->fld_width[PINCFG_TYPE_FUNC]) - 1;
	shift = pin_offset * type->fld_width[PINCFG_TYPE_FUNC];
	if (shift >= 32) {
		/* Some banks have two config registers */
		shift -= 32;
		reg += 4;
	}

	raw_spin_lock_irqsave(&bank->slock, flags);

	data = readl(reg + type->reg_offset[PINCFG_TYPE_FUNC]);
	data &= ~(mask << shift);
	data |= func->val << shift;
	writel(data, reg + type->reg_offset[PINCFG_TYPE_FUNC]);

	drvdata->pin_groups[grp->pins[0] - drvdata->pin_base].state[PINCFG_TYPE_FUNC] =
		((data >> shift) & mask);

	raw_spin_unlock_irqrestore(&bank->slock, flags);
}

/* enable a specified pinmux by writing to registers */
static int samsung_pinmux_set_mux(struct pinctrl_dev *pctldev,
				  unsigned selector,
				  unsigned group)
{
	samsung_pinmux_setup(pctldev, selector, group);
	return 0;
}

/* list of pinmux callbacks for the pinmux vertical in pinctrl core */
static const struct pinmux_ops samsung_pinmux_ops = {
	.get_functions_count	= samsung_get_functions_count,
	.get_function_name	= samsung_pinmux_get_fname,
	.get_function_groups	= samsung_pinmux_get_groups,
	.set_mux		= samsung_pinmux_set_mux,
};

/* set or get the pin config settings for a specified pin */
static int samsung_pinconf_rw(struct pinctrl_dev *pctldev, unsigned int pin,
				unsigned long *config, bool set)
{
	struct samsung_pinctrl_drv_data *drvdata;
	const struct samsung_pin_bank_type *type;
	struct samsung_pin_bank *bank;
	void __iomem *reg_base;
	enum pincfg_type cfg_type = PINCFG_UNPACK_TYPE(*config);
	u32 data, width, pin_offset, mask, shift;
	u32 cfg_value, cfg_reg;
	unsigned long flags;

	drvdata = pinctrl_dev_get_drvdata(pctldev);
	pin_to_reg_bank(drvdata, pin - drvdata->pin_base, &reg_base,
					&pin_offset, &bank);
	type = bank->type;

	if (cfg_type >= PINCFG_TYPE_NUM || !type->fld_width[cfg_type])
		return -EINVAL;

	width = type->fld_width[cfg_type];
	cfg_reg = type->reg_offset[cfg_type];

	raw_spin_lock_irqsave(&bank->slock, flags);

	mask = (1 << width) - 1;
	shift = pin_offset * width;
	data = readl(reg_base + cfg_reg);

	if (set) {
		cfg_value = PINCFG_UNPACK_VALUE(*config);
		data &= ~(mask << shift);
		data |= (cfg_value << shift);
		writel(data, reg_base + cfg_reg);
		drvdata->pin_groups[pin - drvdata->pin_base].state[cfg_type] =
			((data >> shift) & mask);
	} else {
		data >>= shift;
		data &= mask;
		*config = PINCFG_PACK(cfg_type, data);
	}

	raw_spin_unlock_irqrestore(&bank->slock, flags);

	return 0;
}

/* set the pin config settings for a specified pin */
static int samsung_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
				unsigned long *configs, unsigned num_configs)
{
	int i, ret;

	for (i = 0; i < num_configs; i++) {
		ret = samsung_pinconf_rw(pctldev, pin, &configs[i], true);
		if (ret < 0)
			return ret;
	} /* for each config */

	return 0;
}

/* get the pin config settings for a specified pin */
static int samsung_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
					unsigned long *config)
{
	return samsung_pinconf_rw(pctldev, pin, config, false);
}

/* set the pin config settings for a specified pin group */
static int samsung_pinconf_group_set(struct pinctrl_dev *pctldev,
			unsigned group, unsigned long *configs,
			unsigned num_configs)
{
	struct samsung_pinctrl_drv_data *drvdata;
	const unsigned int *pins;
	unsigned int cnt;

	drvdata = pinctrl_dev_get_drvdata(pctldev);
	pins = drvdata->pin_groups[group].pins;

	for (cnt = 0; cnt < drvdata->pin_groups[group].num_pins; cnt++)
		samsung_pinconf_set(pctldev, pins[cnt], configs, num_configs);

	return 0;
}

/* get the pin config settings for a specified pin group */
static int samsung_pinconf_group_get(struct pinctrl_dev *pctldev,
				unsigned int group, unsigned long *config)
{
	struct samsung_pinctrl_drv_data *drvdata;
	const unsigned int *pins;

	drvdata = pinctrl_dev_get_drvdata(pctldev);
	pins = drvdata->pin_groups[group].pins;
	samsung_pinconf_get(pctldev, pins[0], config);
	return 0;
}

#ifdef CONFIG_DEBUG_FS
/* GPIO register names */
static char *gpio_regs[] = {"CON", "DAT", "PUD", "DRV", "CON_PDN", "PUD_PDN"};

static void pin_to_reg_bank(struct samsung_pinctrl_drv_data *drvdata,
			unsigned pin, void __iomem **reg, u32 *offset,
			struct samsung_pin_bank **bank);

/* common debug show function */
static void samsung_pin_dbg_show_by_type(struct samsung_pin_bank *bank,
				void __iomem *reg_base, u32 pin_offset,
				struct seq_file *s, unsigned pin,
				enum pincfg_type cfg_type)
{
	const struct samsung_pin_bank_type *type;
	u32 data, width, mask, shift, cfg_reg;

	type = bank->type;

	if (!type->fld_width[cfg_type])
		return;

	width = type->fld_width[cfg_type];
	cfg_reg = type->reg_offset[cfg_type];
	mask = (1 << width) - 1;
	shift = pin_offset * width;

	data = readl(reg_base + cfg_reg);

	data >>= shift;
	data &= mask;

	seq_printf(s, " %s(0x%x)", gpio_regs[cfg_type], data);
}


/* show whole PUD, DRV, CON_PDN and PUD_PDN register status */
static void samsung_pinconf_dbg_show(struct pinctrl_dev *pctldev,
				struct seq_file *s, unsigned pin)
{
	struct samsung_pinctrl_drv_data *drvdata;
	struct samsung_pin_bank *bank;
	void __iomem *reg_base;
	u32 pin_offset;
	unsigned long flags;
	enum pincfg_type cfg_type;

	drvdata = pinctrl_dev_get_drvdata(pctldev);

	if (!drvdata->resume)
		return;

	pin_to_reg_bank(drvdata, pin - drvdata->pin_base, &reg_base,
					&pin_offset, &bank);

	raw_spin_lock_irqsave(&bank->slock, flags);

	for (cfg_type = PINCFG_TYPE_PUD; cfg_type <= PINCFG_TYPE_PUD_PDN
					; cfg_type++) {
		samsung_pin_dbg_show_by_type(bank, reg_base,
					pin_offset, s, pin, cfg_type);
	}

	raw_spin_unlock_irqrestore(&bank->slock, flags);
}

/* show group's PUD, DRV, CON_PDN and PUD_PDN register status */
static void samsung_pinconf_group_dbg_show(struct pinctrl_dev *pctldev,
				struct seq_file *s, unsigned group)
{
	struct samsung_pinctrl_drv_data *drvdata;
	const unsigned int *pins;
	int i;

	drvdata = pinctrl_dev_get_drvdata(pctldev);
	pins = drvdata->pin_groups[group].pins;

	for (i = 0; i < drvdata->pin_groups[group].num_pins; i++) {
		seq_printf(s, "\n\t%s:", pin_get_name(pctldev, pins[i]));
		samsung_pinconf_dbg_show(pctldev, s, pins[i]);
	}
}
#endif

/* list of pinconfig callbacks for pinconfig vertical in the pinctrl code */
static const struct pinconf_ops samsung_pinconf_ops = {
	.pin_config_get		= samsung_pinconf_get,
	.pin_config_set		= samsung_pinconf_set,
	.pin_config_group_get	= samsung_pinconf_group_get,
	.pin_config_group_set	= samsung_pinconf_group_set,
#ifdef CONFIG_DEBUG_FS
	.pin_config_dbg_show	= samsung_pinconf_dbg_show,
	.pin_config_group_dbg_show = samsung_pinconf_group_dbg_show,
#endif
};

/*
 * The samsung_gpio_set_vlaue() should be called with "bank->slock" held
 * to avoid race condition.
 */
static void samsung_gpio_set_value(struct gpio_chip *gc,
					  unsigned offset, int value)
{
	struct samsung_pin_bank *bank = gpiochip_get_data(gc);
	const struct samsung_pin_bank_type *type = bank->type;
	void __iomem *reg;
	u32 data, pin;

	reg = bank->pctl_base + bank->pctl_offset;
	pin = bank->grange.pin_base + offset - bank->drvdata->pin_base;

	data = readl(reg + type->reg_offset[PINCFG_TYPE_DAT]);
	data &= ~(1 << offset);
	if (value)
		data |= 1 << offset;
	writel(data, reg + type->reg_offset[PINCFG_TYPE_DAT]);

	bank->drvdata->pin_groups[pin].state[PINCFG_TYPE_DAT] = value;
}

/* gpiolib gpio_set callback function */
static void samsung_gpio_set(struct gpio_chip *gc, unsigned offset, int value)
{
	struct samsung_pin_bank *bank = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&bank->slock, flags);
	samsung_gpio_set_value(gc, offset, value);
	raw_spin_unlock_irqrestore(&bank->slock, flags);
}

/* gpiolib gpio_get callback function */
static int samsung_gpio_get(struct gpio_chip *gc, unsigned offset)
{
	void __iomem *reg;
	u32 data;
	struct samsung_pin_bank *bank = gpiochip_get_data(gc);
	const struct samsung_pin_bank_type *type = bank->type;

	reg = bank->pctl_base + bank->pctl_offset;

	data = readl(reg + type->reg_offset[PINCFG_TYPE_DAT]);
	data >>= offset;
	data &= 1;
	return data;
}

/*
 * The samsung_gpio_set_direction() should be called with "bank->slock" held
 * to avoid race condition.
 * The calls to gpio_direction_output() and gpio_direction_input()
 * leads to this function call.
 */
static int samsung_gpio_set_direction(struct gpio_chip *gc,
					     unsigned offset, bool input)
{
	const struct samsung_pin_bank_type *type;
	struct samsung_pin_bank *bank;
	struct samsung_pinctrl_drv_data *drvdata;
	void __iomem *reg;
	u32 pin, data, mask, shift;

	bank = gpiochip_get_data(gc);
	type = bank->type;
	drvdata = bank->drvdata;

	reg = bank->pctl_base + bank->pctl_offset
			+ type->reg_offset[PINCFG_TYPE_FUNC];
	pin = bank->grange.pin_base + offset - drvdata->pin_base;

	mask = (1 << type->fld_width[PINCFG_TYPE_FUNC]) - 1;
	shift = offset * type->fld_width[PINCFG_TYPE_FUNC];
	if (shift >= 32) {
		/* Some banks have two config registers */
		shift -= 32;
		reg += 4;
	}

	data = readl(reg);
	data &= ~(mask << shift);
	if (!input) {
		data |= PIN_CON_FUNC_OUTPUT << shift;
		drvdata->pin_groups[pin].state[PINCFG_TYPE_FUNC] = PIN_CON_FUNC_OUTPUT;
	} else
		drvdata->pin_groups[pin].state[PINCFG_TYPE_FUNC] = PIN_CON_FUNC_INPUT;
	writel(data, reg);

	return 0;
}

/* gpiolib gpio_direction_input callback function. */
static int samsung_gpio_direction_input(struct gpio_chip *gc, unsigned offset)
{
	struct samsung_pin_bank *bank = gpiochip_get_data(gc);
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&bank->slock, flags);
	ret = samsung_gpio_set_direction(gc, offset, true);
	raw_spin_unlock_irqrestore(&bank->slock, flags);
	return ret;
}

/* gpiolib gpio_direction_output callback function. */
static int samsung_gpio_direction_output(struct gpio_chip *gc, unsigned offset,
							int value)
{
	struct samsung_pin_bank *bank = gpiochip_get_data(gc);
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&bank->slock, flags);
	samsung_gpio_set_value(gc, offset, value);
	ret = samsung_gpio_set_direction(gc, offset, false);
	raw_spin_unlock_irqrestore(&bank->slock, flags);

	return ret;
}

/*
 * gpiolib gpio_to_irq callback function. Creates a mapping between a GPIO pin
 * and a virtual IRQ, if not already present.
 */
static int samsung_gpio_to_irq(struct gpio_chip *gc, unsigned offset)
{
	struct samsung_pin_bank *bank = gpiochip_get_data(gc);
	unsigned int virq;

	if (!bank->irq_domain)
		return -ENXIO;

	virq = irq_create_mapping(bank->irq_domain, offset);

	return (virq) ? : -ENXIO;
}

static struct samsung_pin_group *samsung_pinctrl_create_groups(
				struct device *dev,
				struct samsung_pinctrl_drv_data *drvdata,
				unsigned int *cnt)
{
	struct pinctrl_desc *ctrldesc = &drvdata->pctl;
	struct samsung_pin_group *groups, *grp;
	const struct pinctrl_pin_desc *pdesc;
	int i;

	groups = devm_kcalloc(dev, ctrldesc->npins, sizeof(*groups),
				GFP_KERNEL);
	if (!groups)
		return ERR_PTR(-EINVAL);
	grp = groups;

	pdesc = ctrldesc->pins;
	for (i = 0; i < ctrldesc->npins; ++i, ++pdesc, ++grp) {
		grp->name = pdesc->name;
		grp->pins = &pdesc->number;
		grp->num_pins = 1;
	}

	*cnt = ctrldesc->npins;
	return groups;
}

static int samsung_pinctrl_create_function(struct device *dev,
				struct samsung_pinctrl_drv_data *drvdata,
				struct device_node *func_np,
				struct samsung_pmx_func *func)
{
	int npins;
	int ret;
	int i;

	if (of_property_read_u32(func_np, "samsung,pin-function", &func->val))
		return 0;

	npins = of_property_count_strings(func_np, "samsung,pins");
	if (npins < 1) {
		dev_err(dev, "invalid pin list in %pOFn node", func_np);
		return -EINVAL;
	}

	func->name = func_np->full_name;

	func->groups = devm_kcalloc(dev, npins, sizeof(char *), GFP_KERNEL);
	if (!func->groups)
		return -ENOMEM;

	for (i = 0; i < npins; ++i) {
		const char *gname;

		ret = of_property_read_string_index(func_np, "samsung,pins",
							i, &gname);
		if (ret) {
			dev_err(dev,
				"failed to read pin name %d from %pOFn node\n",
				i, func_np);
			return ret;
		}

		func->groups[i] = gname;
	}

	func->num_groups = npins;
	return 1;
}

static struct samsung_pmx_func *samsung_pinctrl_create_functions(
				struct device *dev,
				struct samsung_pinctrl_drv_data *drvdata,
				unsigned int *cnt)
{
	struct samsung_pmx_func *functions, *func;
	struct device_node *dev_np = dev->of_node;
	struct device_node *cfg_np;
	unsigned int func_cnt = 0;
	int ret;

	/*
	 * Iterate over all the child nodes of the pin controller node
	 * and create pin groups and pin function lists.
	 */
	for_each_child_of_node(dev_np, cfg_np) {
		struct device_node *func_np;

		if (!of_get_child_count(cfg_np)) {
			if (!of_find_property(cfg_np,
			    "samsung,pin-function", NULL))
				continue;
			++func_cnt;
			continue;
		}

		for_each_child_of_node(cfg_np, func_np) {
			if (!of_find_property(func_np,
			    "samsung,pin-function", NULL))
				continue;
			++func_cnt;
		}
	}

	functions = devm_kcalloc(dev, func_cnt, sizeof(*functions),
					GFP_KERNEL);
	if (!functions)
		return ERR_PTR(-ENOMEM);
	func = functions;

	/*
	 * Iterate over all the child nodes of the pin controller node
	 * and create pin groups and pin function lists.
	 */
	func_cnt = 0;
	for_each_child_of_node(dev_np, cfg_np) {
		struct device_node *func_np;

		if (!of_get_child_count(cfg_np)) {
			ret = samsung_pinctrl_create_function(dev, drvdata,
							cfg_np, func);
			if (ret < 0) {
				of_node_put(cfg_np);
				return ERR_PTR(ret);
			}
			if (ret > 0) {
				++func;
				++func_cnt;
			}
			continue;
		}

		for_each_child_of_node(cfg_np, func_np) {
			ret = samsung_pinctrl_create_function(dev, drvdata,
						func_np, func);
			if (ret < 0) {
				of_node_put(func_np);
				of_node_put(cfg_np);
				return ERR_PTR(ret);
			}
			if (ret > 0) {
				++func;
				++func_cnt;
			}
		}
	}

	*cnt = func_cnt;
	return functions;
}

/*
 * Parse the information about all the available pin groups and pin functions
 * from device node of the pin-controller. A pin group is formed with all
 * the pins listed in the "samsung,pins" property.
 */

static int samsung_pinctrl_parse_dt(struct platform_device *pdev,
				    struct samsung_pinctrl_drv_data *drvdata)
{
	struct device *dev = &pdev->dev;
	struct samsung_pin_group *groups;
	struct samsung_pmx_func *functions;
	unsigned int grp_cnt = 0, func_cnt = 0;

	groups = samsung_pinctrl_create_groups(dev, drvdata, &grp_cnt);
	if (IS_ERR(groups)) {
		dev_err(dev, "failed to parse pin groups\n");
		return PTR_ERR(groups);
	}

	functions = samsung_pinctrl_create_functions(dev, drvdata, &func_cnt);
	if (IS_ERR(functions)) {
		dev_err(dev, "failed to parse pin functions\n");
		return PTR_ERR(functions);
	}

	drvdata->pin_groups = groups;
	drvdata->nr_groups = grp_cnt;
	drvdata->pmx_functions = functions;
	drvdata->nr_functions = func_cnt;

	return 0;
}

/* register the pinctrl interface with the pinctrl subsystem */
static int samsung_pinctrl_register(struct platform_device *pdev,
				    struct samsung_pinctrl_drv_data *drvdata)
{
	struct pinctrl_desc *ctrldesc = &drvdata->pctl;
	struct pinctrl_pin_desc *pindesc, *pdesc;
	struct samsung_pin_bank *pin_bank;
	char *pin_names;
	int pin, bank, ret;

	ctrldesc->name = "samsung-pinctrl";
	ctrldesc->owner = THIS_MODULE;
	ctrldesc->pctlops = &samsung_pctrl_ops;
	ctrldesc->pmxops = &samsung_pinmux_ops;
	ctrldesc->confops = &samsung_pinconf_ops;

	pindesc = devm_kcalloc(&pdev->dev,
			       drvdata->nr_pins, sizeof(*pindesc),
			       GFP_KERNEL);
	if (!pindesc)
		return -ENOMEM;
	ctrldesc->pins = pindesc;
	ctrldesc->npins = drvdata->nr_pins;

	/* dynamically populate the pin number and pin name for pindesc */
	for (pin = 0, pdesc = pindesc; pin < ctrldesc->npins; pin++, pdesc++)
		pdesc->number = pin + drvdata->pin_base;

	/*
	 * allocate space for storing the dynamically generated names for all
	 * the pins which belong to this pin-controller.
	 */
	pin_names = devm_kzalloc(&pdev->dev,
				 array3_size(sizeof(char), PIN_NAME_LENGTH,
					     drvdata->nr_pins),
				 GFP_KERNEL);
	if (!pin_names)
		return -ENOMEM;

	/* for each pin, the name of the pin is pin-bank name + pin number */
	for (bank = 0; bank < drvdata->nr_banks; bank++) {
		pin_bank = &drvdata->pin_banks[bank];
		for (pin = 0; pin < pin_bank->nr_pins; pin++) {
			sprintf(pin_names, "%s-%d", pin_bank->name, pin);
			pdesc = pindesc + pin_bank->pin_base + pin;
			pdesc->name = pin_names;
			pin_names += PIN_NAME_LENGTH;
		}
	}

	ret = samsung_pinctrl_parse_dt(pdev, drvdata);
	if (ret)
		return ret;

	drvdata->pctl_dev = devm_pinctrl_register(&pdev->dev, ctrldesc,
						  drvdata);
	if (IS_ERR(drvdata->pctl_dev)) {
		dev_err(&pdev->dev, "could not register pinctrl driver\n");
		return PTR_ERR(drvdata->pctl_dev);
	}

	for (bank = 0; bank < drvdata->nr_banks; ++bank) {
		pin_bank = &drvdata->pin_banks[bank];
		pin_bank->grange.name = pin_bank->name;
		pin_bank->grange.id = bank;
		pin_bank->grange.pin_base = drvdata->pin_base
						+ pin_bank->pin_base;
		pin_bank->grange.base = pin_bank->grange.pin_base;
		pin_bank->grange.npins = pin_bank->nr_pins;
		pin_bank->grange.gc = &pin_bank->gpio_chip;
		pinctrl_add_gpio_range(drvdata->pctl_dev, &pin_bank->grange);
	}

	return 0;
}

/* unregister the pinctrl interface with the pinctrl subsystem */
static int samsung_pinctrl_unregister(struct platform_device *pdev,
				      struct samsung_pinctrl_drv_data *drvdata)
{
	struct samsung_pin_bank *bank = drvdata->pin_banks;
	int i;

	for (i = 0; i < drvdata->nr_banks; ++i, ++bank)
		pinctrl_remove_gpio_range(drvdata->pctl_dev, &bank->grange);

	return 0;
}

static const struct gpio_chip samsung_gpiolib_chip = {
	.request = gpiochip_generic_request,
	.free = gpiochip_generic_free,
	.set = samsung_gpio_set,
	.get = samsung_gpio_get,
	.direction_input = samsung_gpio_direction_input,
	.direction_output = samsung_gpio_direction_output,
	.to_irq = samsung_gpio_to_irq,
	.owner = THIS_MODULE,
};

/* register the gpiolib interface with the gpiolib subsystem */
static int samsung_gpiolib_register(struct platform_device *pdev,
				    struct samsung_pinctrl_drv_data *drvdata)
{
	struct samsung_pin_bank *bank = drvdata->pin_banks;
	struct gpio_chip *gc;
	int ret;
	int i;

	for (i = 0; i < drvdata->nr_banks; ++i, ++bank) {
		bank->gpio_chip = samsung_gpiolib_chip;

		gc = &bank->gpio_chip;
		gc->base = bank->grange.base;
		gc->ngpio = bank->nr_pins;
		gc->parent = &pdev->dev;
		gc->fwnode = bank->fwnode;
		gc->label = bank->name;

		ret = devm_gpiochip_add_data(&pdev->dev, gc, bank);
		if (ret) {
			dev_err(&pdev->dev, "failed to register gpio_chip %s, error code: %d\n",
							gc->label, ret);
			return ret;
		}
	}

	return 0;
}

static int samsung_pinctrl_debug_set(struct samsung_pinctrl_drv_data *drvdata)
{
	struct samsung_pin_bank *bank;
	void __iomem *reg_base;
	u32 pin_offset;
	unsigned long flags;
	enum pincfg_type cfg_type;
	const struct samsung_pin_bank_type *type;
	u32 data, width, mask, shift, cfg_reg;
	int i;

	/*
	 * We assume temporarily that no resume drvdata is dynamic power-controlled block.
	 */
	if (!drvdata->resume && !drvdata->suspend)
		return -ENOTSUPP;

	for (i = 0; i < drvdata->pctl_dev->desc->npins; i++) {
		pin_to_reg_bank(drvdata, i, &reg_base,
				&pin_offset, &bank);
		drvdata->pin_groups[i].state_num = 0;
		for (cfg_type = 0; cfg_type < PINCFG_TYPE_NUM; cfg_type++) {
			type = bank->type;
			if (!type->fld_width[cfg_type])
				continue;

			raw_spin_lock_irqsave(&bank->slock, flags);
			width = type->fld_width[cfg_type];
			cfg_reg = type->reg_offset[cfg_type];
			mask = (1 << width) - 1;
			shift = pin_offset * width;

			data = readl(reg_base + cfg_reg);

			data >>= shift;
			data &= mask;

			drvdata->pin_groups[i].state[cfg_type] = data;
			drvdata->pin_groups[i].state_num++;
			raw_spin_unlock_irqrestore(&bank->slock, flags);
		}
	}

	return 0;
}

static const struct samsung_pin_ctrl *
samsung_pinctrl_get_soc_data_for_of_alias(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	const struct samsung_pinctrl_of_match_data *of_data;
	int id;

	id = of_alias_get_id(node, "pinctrl");
	if (id < 0) {
		dev_err(&pdev->dev, "failed to get alias id\n");
		return NULL;
	}

	of_data = of_device_get_match_data(&pdev->dev);
	if (!of_data || id >= of_data->num_ctrl) {
		dev_err(&pdev->dev, "invalid alias id %d\n", id);
		return NULL;
	}

	return &(of_data->ctrl[id]);
}

static void samsung_banks_node_put(struct samsung_pinctrl_drv_data *d)
{
	struct samsung_pin_bank *bank;
	unsigned int i;

	bank = d->pin_banks;
	for (i = 0; i < d->nr_banks; ++i, ++bank)
		fwnode_handle_put(bank->fwnode);
}

/*
 * Iterate over all driver pin banks to find one matching the name of node,
 * skipping optional "-gpio" node suffix. When found, assign node to the bank.
 */
static void samsung_banks_node_get(struct device *dev, struct samsung_pinctrl_drv_data *d)
{
	const char *suffix = "-gpio-bank";
	struct samsung_pin_bank *bank;
	struct fwnode_handle *child;
	/* Pin bank names are up to 4 characters */
	char node_name[20];
	unsigned int i;
	size_t len;

	bank = d->pin_banks;
	for (i = 0; i < d->nr_banks; ++i, ++bank) {
		strscpy(node_name, bank->name, sizeof(node_name));
		len = strlcat(node_name, suffix, sizeof(node_name));
		if (len >= sizeof(node_name)) {
			dev_err(dev, "Too long pin bank name '%s', ignoring\n",
				bank->name);
			continue;
		}

		for_each_gpiochip_node(dev, child) {
			struct device_node *np = to_of_node(child);

			if (of_node_name_eq(np, node_name))
				break;
			if (of_node_name_eq(np, bank->name))
				break;
		}

		if (child)
			bank->fwnode = child;
		else
			dev_warn(dev, "Missing node for bank %s - invalid DTB\n",
				 bank->name);
		/* child reference dropped in samsung_drop_banks_of_node() */
	}
}

/* retrieve the soc specific data */
static const struct samsung_pin_ctrl *
samsung_pinctrl_get_soc_data(struct samsung_pinctrl_drv_data *d,
			     struct platform_device *pdev)
{
	const struct samsung_pin_bank_data *bdata;
	const struct samsung_pin_ctrl *ctrl;
	struct samsung_pin_bank *bank;
	struct resource *res;
	void __iomem *virt_base[SAMSUNG_PINCTRL_NUM_RESOURCES];
	unsigned int i;

	ctrl = samsung_pinctrl_get_soc_data_for_of_alias(pdev);
	if (!ctrl)
		return ERR_PTR(-ENOENT);

	d->suspend = ctrl->suspend;
	d->resume = ctrl->resume;
	d->nr_banks = ctrl->nr_banks;
	d->pin_banks = devm_kcalloc(&pdev->dev, d->nr_banks,
					sizeof(*d->pin_banks), GFP_KERNEL);
	if (!d->pin_banks)
		return ERR_PTR(-ENOMEM);

	if (ctrl->nr_ext_resources + 1 > SAMSUNG_PINCTRL_NUM_RESOURCES)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < ctrl->nr_ext_resources + 1; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			dev_err(&pdev->dev, "failed to get mem%d resource\n", i);
			return ERR_PTR(-EINVAL);
		}
		virt_base[i] = devm_ioremap(&pdev->dev, res->start,
						resource_size(res));
		if (!virt_base[i]) {
			dev_err(&pdev->dev, "failed to ioremap %pR\n", res);
			return ERR_PTR(-EIO);
		}
	}

	bank = d->pin_banks;
	bdata = ctrl->pin_banks;
	for (i = 0; i < ctrl->nr_banks; ++i, ++bdata, ++bank) {
		bank->type = bdata->type;
		bank->pctl_offset = bdata->pctl_offset;
		bank->nr_pins = bdata->nr_pins;
		bank->eint_func = bdata->eint_func;
		bank->eint_type = bdata->eint_type;
		bank->eint_mask = bdata->eint_mask;
		bank->eint_offset = bdata->eint_offset;
		bank->eint_num = bdata->eint_num;
		bank->fltcon_offset = bdata->fltcon_offset;
		bank->name = bdata->name;
		bank->sysreg_cmgp_offs = bdata->sysreg_cmgp_offs;
		bank->sysreg_cmgp_bit = bdata->sysreg_cmgp_bit;

		raw_spin_lock_init(&bank->slock);
		bank->drvdata = d;
		bank->pin_base = d->nr_pins;
		d->nr_pins += bank->nr_pins;

		bank->eint_base = virt_base[0];
		bank->pctl_base = virt_base[bdata->pctl_res_idx];
	}
	/*
	 * Legacy platforms should provide only one resource with IO memory.
	 * Store it as virt_base because legacy driver needs to access it
	 * through samsung_pinctrl_drv_data.
	 */
	d->virt_base = virt_base[0];

	samsung_banks_node_get(&pdev->dev, d);

	d->pin_base = pin_base;
	pin_base += d->nr_pins;

	return ctrl;
}

#if IS_ENABLED(CONFIG_PINCTRL_SEC_GPIO_DVS)
static unsigned int sum_of_nr_pins;
#endif /* CONFIG_PINCTRL_SEC_GPIO_DVS */

static int samsung_pinctrl_probe(struct platform_device *pdev)
{
	struct samsung_pinctrl_drv_data *drvdata;
	const struct samsung_pin_ctrl *ctrl;
	struct device *dev = &pdev->dev;
	int ret;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	ctrl = samsung_pinctrl_get_soc_data(drvdata, pdev);
	if (IS_ERR(ctrl)) {
		dev_err(&pdev->dev, "driver data not available\n");
		return PTR_ERR(ctrl);
	}
	drvdata->dev = dev;

	ret = platform_get_irq_optional(pdev, 0);
	if (ret < 0 && ret != -ENXIO)
		goto err_put_banks;
	if (ret > 0)
		drvdata->irq = ret;

	if (ctrl->retention_data) {
		drvdata->retention_ctrl = ctrl->retention_data->init(drvdata,
							  ctrl->retention_data);
		if (IS_ERR(drvdata->retention_ctrl)) {
			ret = PTR_ERR(drvdata->retention_ctrl);
			goto err_put_banks;
		}
	}

	ret = samsung_pinctrl_register(pdev, drvdata);
	if (ret)
		goto err_put_banks;

	if (ctrl->eint_gpio_init)
		ctrl->eint_gpio_init(drvdata);
	if (ctrl->eint_wkup_init)
		ctrl->eint_wkup_init(drvdata);

	ret = samsung_gpiolib_register(pdev, drvdata);
	if (ret)
		goto err_unregister;

	platform_set_drvdata(pdev, drvdata);

	/* Add to the global list */
	list_add_tail(&drvdata->node, &drvdata_list);

	samsung_pinctrl_debug_set(drvdata);
#if IS_ENABLED(CONFIG_PINCTRL_SEC_GPIO_DVS)
	sum_of_nr_pins += drvdata->nr_pins;
#endif /* CONFIG_PINCTRL_SEC_GPIO_DVS */
	return 0;

err_unregister:
	samsung_pinctrl_unregister(pdev, drvdata);
err_put_banks:
	samsung_banks_node_put(drvdata);
	return ret;
}

#ifdef CONFIG_PM

/**
 * samsung_pinctrl_suspend_dev - save pinctrl state for suspend for a device
 *
 * Save data for all banks handled by this device.
 */
static void samsung_pinctrl_suspend_dev(
	struct samsung_pinctrl_drv_data *drvdata)
{
	int i;
	int ret;

	if (!drvdata->suspend)
		return;

	if (!IS_ERR(drvdata->pctl_dev->p)) {
		/* This is ignore to disable mux configuration. */
		drvdata->pctl_dev->p->state = NULL;
	}

	ret = pinctrl_force_sleep(drvdata->pctl_dev);
	if (ret)
	        dev_err(drvdata->dev, "could not set sleep pinstate %d\n", ret);

	if (!drvdata->suspend)
		return;

	for (i = 0; i < drvdata->nr_banks; i++) {
		struct samsung_pin_bank *bank = &drvdata->pin_banks[i];
		void __iomem *reg = bank->pctl_base + bank->pctl_offset;
		const u8 *offs = bank->type->reg_offset;
		const u8 *widths = bank->type->fld_width;
		enum pincfg_type type;

		/* Registers without a powerdown config aren't lost */
		if (!widths[PINCFG_TYPE_CON_PDN])
			continue;

		for (type = 0; type < PINCFG_TYPE_NUM; type++)
			if (widths[type])
				bank->pm_save[type] = readl(reg + offs[type]);

		if (widths[PINCFG_TYPE_FUNC] * bank->nr_pins > 32) {
			/* Some banks have two config registers */
			bank->pm_save[PINCFG_TYPE_NUM] =
				readl(reg + offs[PINCFG_TYPE_FUNC] + 4);
			pr_debug("Save %s @ %p (con %#010x %08x)\n",
				 bank->name, reg,
				 bank->pm_save[PINCFG_TYPE_FUNC],
				 bank->pm_save[PINCFG_TYPE_NUM]);
		} else {
			pr_debug("Save %s @ %p (con %#010x)\n", bank->name,
				 reg, bank->pm_save[PINCFG_TYPE_FUNC]);
		}
	}

	drvdata->suspend(drvdata);
	if (drvdata->retention_ctrl && drvdata->retention_ctrl->enable)
		drvdata->retention_ctrl->enable(drvdata);
}

/**
 * samsung_pinctrl_resume_dev - restore pinctrl state from suspend for a device
 *
 * Restore one of the banks that was saved during suspend.
 *
 * We don't bother doing anything complicated to avoid glitching lines since
 * we're called before pad retention is turned off.
 */
static void samsung_pinctrl_resume_dev(struct samsung_pinctrl_drv_data *drvdata)
{
	int i;

	if (!drvdata->resume)
		return;

	drvdata->resume(drvdata);

	for (i = 0; i < drvdata->nr_banks; i++) {
		struct samsung_pin_bank *bank = &drvdata->pin_banks[i];
		void __iomem *reg = bank->pctl_base + bank->pctl_offset;
		const u8 *offs = bank->type->reg_offset;
		const u8 *widths = bank->type->fld_width;
		enum pincfg_type type;

		/* Registers without a powerdown config aren't lost */
		if (!widths[PINCFG_TYPE_CON_PDN])
			continue;

		if (widths[PINCFG_TYPE_FUNC] * bank->nr_pins > 32) {
			/* Some banks have two config registers */
			pr_debug("%s @ %p (con %#010x %08x => %#010x %08x)\n",
				 bank->name, reg,
				 readl(reg + offs[PINCFG_TYPE_FUNC]),
				 readl(reg + offs[PINCFG_TYPE_FUNC] + 4),
				 bank->pm_save[PINCFG_TYPE_FUNC],
				 bank->pm_save[PINCFG_TYPE_NUM]);
			writel(bank->pm_save[PINCFG_TYPE_NUM],
			       reg + offs[PINCFG_TYPE_FUNC] + 4);
		} else {
			pr_debug("%s @ %p (con %#010x => %#010x)\n", bank->name,
				 reg, readl(reg + offs[PINCFG_TYPE_FUNC]),
				 bank->pm_save[PINCFG_TYPE_FUNC]);
		}
		for (type = 0; type < PINCFG_TYPE_NUM; type++)
			if (widths[type])
				writel(bank->pm_save[type], reg + offs[type]);
	}

	/* For changing state without writing register. */
	if (!IS_ERR(drvdata->pctl_dev->p) && !IS_ERR(drvdata->pctl_dev->hog_default))
		drvdata->pctl_dev->p->state = drvdata->pctl_dev->hog_default;

	if (drvdata->retention_ctrl && drvdata->retention_ctrl->disable)
		drvdata->retention_ctrl->disable(drvdata);
}

/**
 * samsung_pinctrl_suspend - save pinctrl state for suspend
 *
 * Save data for all banks across all devices.
 */
static int samsung_pinctrl_suspend(void)
{
	struct samsung_pinctrl_drv_data *drvdata;

	list_for_each_entry(drvdata, &drvdata_list, node) {
		samsung_pinctrl_suspend_dev(drvdata);
	}

	return 0;
}

/**
 * samsung_pinctrl_resume - restore pinctrl state for suspend
 *
 * Restore data for all banks across all devices.
 */
static void samsung_pinctrl_resume(void)
{
	struct samsung_pinctrl_drv_data *drvdata;

	list_for_each_entry_reverse(drvdata, &drvdata_list, node) {
		samsung_pinctrl_resume_dev(drvdata);
	}
}

u32 exynos_eint_to_pin_num(int eint)
{
	struct samsung_pinctrl_drv_data *drvdata;
	struct samsung_pin_bank *pbank;
	int i, offset = 0;

	drvdata = list_first_entry(&drvdata_list,
			struct samsung_pinctrl_drv_data, node);

	for (i = 0; i < drvdata->nr_banks; i++) {
		pbank = &drvdata->pin_banks[i];
		if (!strncmp(pbank->name, "gpa0", strlen(pbank->name)))
			break;

		offset += pbank->nr_pins;
	}

	return drvdata->pin_base + eint + offset;
}
EXPORT_SYMBOL(exynos_eint_to_pin_num);

#else
#define samsung_pinctrl_suspend		NULL
#define samsung_pinctrl_resume		NULL
#endif

static struct syscore_ops samsung_pinctrl_syscore_ops = {
	.suspend	= samsung_pinctrl_suspend,
	.resume		= samsung_pinctrl_resume,
};

static struct exynos_s2i_ops samsung_pinctrl_s2i_ops = {
	.suspend	= samsung_pinctrl_suspend,
	.resume		= samsung_pinctrl_resume,
};

static const struct of_device_id samsung_pinctrl_dt_match[] = {
#ifdef CONFIG_PINCTRL_EXYNOS_ARM
	{ .compatible = "samsung,exynos3250-pinctrl",
		.data = &exynos3250_of_data },
	{ .compatible = "samsung,exynos4210-pinctrl",
		.data = &exynos4210_of_data },
	{ .compatible = "samsung,exynos4x12-pinctrl",
		.data = &exynos4x12_of_data },
	{ .compatible = "samsung,exynos5250-pinctrl",
		.data = &exynos5250_of_data },
	{ .compatible = "samsung,exynos5260-pinctrl",
		.data = &exynos5260_of_data },
	{ .compatible = "samsung,exynos5410-pinctrl",
		.data = &exynos5410_of_data },
	{ .compatible = "samsung,exynos5420-pinctrl",
		.data = &exynos5420_of_data },
	{ .compatible = "samsung,s5pv210-pinctrl",
		.data = &s5pv210_of_data },
#endif
#ifdef CONFIG_PINCTRL_EXYNOS_ARM64
	{ .compatible = "samsung,exynos5433-pinctrl",
		.data = &exynos5433_of_data },
	{ .compatible = "samsung,exynos7-pinctrl",
		.data = &exynos7_of_data },
	{ .compatible = "samsung,exynos7885-pinctrl",
		.data = &exynos7885_of_data },
	{ .compatible = "samsung,exynos850-pinctrl",
		.data = &exynos850_of_data },
	{ .compatible = "samsung,exynosautov9-pinctrl",
		.data = &exynosautov9_of_data },
	{ .compatible = "tesla,fsd-pinctrl",
		.data = &fsd_of_data },
	{ .compatible = "samsung,s5e9935-pinctrl",
		.data = &s5e9935_of_data },
	{ .compatible = "samsung,s5e9945-pinctrl",
		.data = &s5e9945_of_data },
        { .compatible = "samsung,s5e8845-pinctrl",
                .data = &s5e8845_of_data },
#endif
#ifdef CONFIG_PINCTRL_S3C64XX
	{ .compatible = "samsung,s3c64xx-pinctrl",
		.data = &s3c64xx_of_data },
#endif
#ifdef CONFIG_PINCTRL_S3C24XX
	{ .compatible = "samsung,s3c2412-pinctrl",
		.data = &s3c2412_of_data },
	{ .compatible = "samsung,s3c2416-pinctrl",
		.data = &s3c2416_of_data },
	{ .compatible = "samsung,s3c2440-pinctrl",
		.data = &s3c2440_of_data },
	{ .compatible = "samsung,s3c2450-pinctrl",
		.data = &s3c2450_of_data },
#endif
	{},
};

static struct platform_driver samsung_pinctrl_driver = {
	.probe		= samsung_pinctrl_probe,
	.driver = {
		.name	= "samsung-pinctrl",
		.of_match_table = samsung_pinctrl_dt_match,
		.suppress_bind_attrs = true,
	},
};
MODULE_DEVICE_TABLE(of, samsung_pinctrl_dt_match);

static int __init samsung_pinctrl_drv_register(void)
{
	/*
	 * Register syscore ops for save/restore of registers across suspend.
	 * It's important to ensure that this driver is running at an earlier
	 * initcall level than any arch-specific init calls that install syscore
	 * ops that turn off pad retention (like exynos_pm_resume).
	 */
	register_syscore_ops(&samsung_pinctrl_syscore_ops);
	register_exynos_s2i_ops(&samsung_pinctrl_s2i_ops);

	return platform_driver_register(&samsung_pinctrl_driver);
}
postcore_initcall(samsung_pinctrl_drv_register);

static void __exit samsung_pinctrl_drv_unregister(void)
{
	platform_driver_unregister(&samsung_pinctrl_driver);
}
module_exit(samsung_pinctrl_drv_unregister);

#if IS_ENABLED(CONFIG_PINCTRL_SEC_GPIO_DVS)

#define GET_RESULT_GPIO(a, b, c)	\
	((a<<4 & 0xF0) | (b<<1 & 0xE) | (c & 0x1))

static struct gpiomap_result_t gpiomap_result;

static u32 gpiodvs_get_by_type(struct samsung_pin_bank *bank,
				void __iomem *reg_base, u32 pin_offset,
				enum pincfg_type cfg_type)
{
	const struct samsung_pin_bank_type *type;
	u32 data, width, mask, shift, cfg_reg;

	type = bank->type;

	if (!type->fld_width[cfg_type])
		return 0;

	width = type->fld_width[cfg_type];
	cfg_reg = type->reg_offset[cfg_type];
	mask = (1 << width) - 1;
	shift = pin_offset * width;

	data = readl(reg_base + cfg_reg);

	data >>= shift;
	data &= mask;

	return data;
}

static u8 gpiodvs_combine_data(u32 *data, unsigned char phonestate)
{
	u8 temp_io, temp_pdpu, temp_lh;
	u32 data_pdpu;

	/* GPIO DVS
	 * FUNC - input: 1, output: 2 eint:3 func: 0
	 * PUD - no-pull: 0, pull-down: 1, pull-up: 2 error: 7
	 * DATA - high: 1, low: 0
	 */
	if (phonestate == PHONE_INIT) {
		switch (data[PINCFG_TYPE_FUNC]) {
		case 0x0:	/* input */
			temp_io = 1;
			break;
		case 0x1:	/* output */
			temp_io = 2;
			break;
		case 0xf:	/* eint */
			temp_io = 3;
			break;
		default:	/* func */
			temp_io = 0;
			break;
		}

		data_pdpu = data[PINCFG_TYPE_PUD];
		temp_lh = data[PINCFG_TYPE_DAT];
	} else {
		switch (data[PINCFG_TYPE_CON_PDN]) {
		case 0x0:	/* output low */
			temp_io = 2;
			temp_lh = 0;
			break;
		case 0x1:	/* output high*/
			temp_io = 2;
			temp_lh = 1;
			break;
		case 0x2:	/* input */
			temp_io = 1;
			temp_lh = data[PINCFG_TYPE_DAT];
			break;
		case 0x3:	/* previous state */
			temp_io = 4;
			temp_lh = data[PINCFG_TYPE_DAT];
			break;
		default:	/* func */
			pr_err("%s: invalid con pdn: %u\n", __func__,
					data[PINCFG_TYPE_CON_PDN]);
			temp_io = 0;
			temp_lh = 0;
			break;
		}

		data_pdpu = data[PINCFG_TYPE_PUD_PDN];
	}

	switch (data_pdpu) {
	case 0:
	case 2:
		temp_pdpu = 0;
		break;
	case 1:
		temp_pdpu = 1;
		break;
	case 3:
		temp_pdpu = 2;
		break;
	default:
		temp_pdpu = 7;
		break;
	}

	return GET_RESULT_GPIO(temp_io, temp_pdpu, temp_lh);
}

static bool should_skip_gpio_group(const char *bank_name, const char *skip_grps)
{
	const char *cp = skip_grps;

	while (cp) {
		if (!strncmp(bank_name, cp, strcspn(cp, " ")))
			return true;

		cp = strpbrk(cp, " ");
		if (cp)
			cp++;
	}

	return false;
}

static void gpiodvs_check_init_gpio(struct samsung_pinctrl_drv_data *drvdata,
					unsigned int pin, const char *skip_grps)
{
	static unsigned int init_gpio_idx;
	struct samsung_pin_bank *bank;
	void __iomem *reg_base;
	u32 pin_offset;
	unsigned long flags;
	enum pincfg_type pt;
	u32 data[PINCFG_TYPE_NUM];

	pin_to_reg_bank(drvdata, pin - drvdata->pin_base, &reg_base,
					&pin_offset, &bank);

	/* Some of GPIO groups should not be accessed while non-secure state or
	 * power domain was disabled.
	 */
	if (should_skip_gpio_group(bank->name, skip_grps)) {
		init_gpio_idx++;
		goto out;
	}

	raw_spin_lock_irqsave(&bank->slock, flags);
	for (pt = PINCFG_TYPE_FUNC; pt <= PINCFG_TYPE_PUD; pt++)
		data[pt] = gpiodvs_get_by_type(bank, reg_base, pin_offset, pt);
	raw_spin_unlock_irqrestore(&bank->slock, flags);

	gpiomap_result.init[init_gpio_idx++] =
		gpiodvs_combine_data(data, PHONE_INIT);
out:
	pr_debug("%s: init[%u]=0x%02x\n", __func__, init_gpio_idx - 1,
			gpiomap_result.init[init_gpio_idx - 1]);
}

static void gpiodvs_check_sleep_gpio(struct samsung_pinctrl_drv_data *drvdata,
					unsigned int pin, const char *skip_grps)
{
	static unsigned int sleep_gpio_idx;
	struct samsung_pin_bank *bank;
	void __iomem *reg_base;
	u32 pin_offset;
	unsigned long flags;
	enum pincfg_type pt;
	u32 data[PINCFG_TYPE_NUM];
	const u8 *widths;
	const unsigned int sleep_type_mask = BIT(PINCFG_TYPE_DAT) |
		BIT(PINCFG_TYPE_CON_PDN) | BIT(PINCFG_TYPE_PUD_PDN);

	pin_to_reg_bank(drvdata, pin - drvdata->pin_base, &reg_base,
					&pin_offset, &bank);


	/* Some of GPIO groups should not access during non-secure state or
	 * power domain is disabled.
	 */
	if (should_skip_gpio_group(bank->name, skip_grps)) {
		sleep_gpio_idx++;
		goto out;
	}

	widths = bank->type->fld_width;
	if (widths[PINCFG_TYPE_CON_PDN]) {
		raw_spin_lock_irqsave(&bank->slock, flags);
		for (pt = PINCFG_TYPE_DAT; pt <= PINCFG_TYPE_PUD_PDN; pt++) {
			if (sleep_type_mask & BIT(pt))
				data[pt] = gpiodvs_get_by_type(bank, reg_base,
						pin_offset, pt);
		}
		raw_spin_unlock_irqrestore(&bank->slock, flags);

		gpiomap_result.sleep[sleep_gpio_idx++] =
			gpiodvs_combine_data(data, PHONE_SLEEP);
	} else {
		/* Alive part */
		raw_spin_lock_irqsave(&bank->slock, flags);
		for (pt = PINCFG_TYPE_FUNC; pt <= PINCFG_TYPE_PUD; pt++)
			data[pt] = gpiodvs_get_by_type(bank, reg_base,
					pin_offset, pt);
		raw_spin_unlock_irqrestore(&bank->slock, flags);

		gpiomap_result.sleep[sleep_gpio_idx++] =
			gpiodvs_combine_data(data, PHONE_INIT);
	}
out:
	pr_debug("%s: sleep[%u]=0x%02x\n", __func__, sleep_gpio_idx - 1,
			gpiomap_result.sleep[sleep_gpio_idx - 1]);
}

static void gpiodvs_check_gpio_regs(struct samsung_pinctrl_drv_data *drvdata,
				unsigned char phonestate, const char *skip_grps)
{
	int i, j;

	for (i = 0; i < drvdata->nr_groups; i++) {
		const unsigned int *pins = drvdata->pin_groups[i].pins;

		for (j = 0; j < drvdata->pin_groups[i].num_pins; j++) {
			if (phonestate == PHONE_INIT)
				gpiodvs_check_init_gpio(drvdata, pins[j],
						skip_grps);
			else
				gpiodvs_check_sleep_gpio(drvdata, pins[j],
						skip_grps);
		}
	}
}

static void check_gpio_status(unsigned char phonestate, const char *skip_grps)
{
	struct samsung_pinctrl_drv_data *drvdata;

	list_for_each_entry(drvdata, &drvdata_list, node) {
		gpiodvs_check_gpio_regs(drvdata, phonestate, skip_grps);
	}
}

static unsigned int samsung_pinctrl_get_nr_gpio(void)
{
	return sum_of_nr_pins;
}

static struct gpio_dvs_t s5e8845_secgpio_dvs = {
	.result = &gpiomap_result,
	.check_gpio_status = check_gpio_status,
	.skip_grps = "gpv0", /* DMIC(VTS) */
};

const struct secgpio_dvs_data s5e8845_secgpio_dvs_data = {
	.gpio_dvs = &s5e8845_secgpio_dvs,
	.get_nr_gpio = samsung_pinctrl_get_nr_gpio,
};
EXPORT_SYMBOL_GPL(s5e8845_secgpio_dvs_data);
#endif /* CONFIG_PINCTRL_SEC_GPIO_DVS */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Youngmin Nam <youngmin.nam@samsung.com>");
MODULE_DESCRIPTION("Samsung Exynos GPIO driver");
