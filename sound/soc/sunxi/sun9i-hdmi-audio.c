// SPDX-License-Identifier: GPL-2.0
//
// sun9i hdmi audio sound card
//
// Copyright (C) 2021 Jernej Skrabec <jernej.skrabec@gmail.com>

#include <linux/module.h>
#include <linux/of_platform.h>

#include <sound/soc.h>
#include <sound/soc-dai.h>

static int sun9i_hdmi_audio_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	unsigned int mclk;

	mclk = params_rate(params) * 128;

	return snd_soc_dai_set_sysclk(asoc_rtd_to_cpu(rtd, 0), 0, mclk,
				      SND_SOC_CLOCK_OUT);
}

static const struct snd_soc_ops sun9i_hdmi_audio_ops = {
	.hw_params = sun9i_hdmi_audio_hw_params,
};

static int sun9i_hdmi_audio_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret;

	/* TODO: switch to custom api once it's implemented in sun4i-i2s */
	ret = snd_soc_dai_set_tdm_slot(asoc_rtd_to_cpu(rtd, 0), 0, 0, 2, 32);
	if (ret) {
		dev_err(asoc_rtd_to_cpu(rtd, 0)->dev,
			"setting tdm link slots failed\n");
		return ret;
	}

	return 0;
}

static int sun9i_hdmi_audio_parse_dai(struct device_node *node,
				      struct snd_soc_dai_link_component *dlc)
{
	struct of_phandle_args args;
	int ret;

	if (!node)
		return 0;

	ret = of_parse_phandle_with_args(node, "sound-dai",
					 "#sound-dai-cells", 0, &args);
	if (ret)
		return ret;

	ret = snd_soc_get_dai_name(&args, &dlc->dai_name);
	if (ret < 0) {
		of_node_put(args.np);

		return ret;
	}

	dlc->of_node = args.np;

	return 0;
}

static int sun9i_hdmi_audio_probe(struct platform_device *pdev)
{
	struct snd_soc_dai_link_component *dlc;
	struct device *dev = &pdev->dev;
	struct snd_soc_dai_link *link;
	struct snd_soc_card *card;
	struct device_node *child;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	link = devm_kzalloc(dev, sizeof(*link), GFP_KERNEL);
	if (!link)
		return -ENOMEM;

	dlc = devm_kzalloc(dev, sizeof(*dlc) * 3, GFP_KERNEL);
	if (!dlc)
		return -ENOMEM;

	child = of_get_child_by_name(dev->of_node, "codec");
	if (!child)
		return -ENODEV;

	ret = sun9i_hdmi_audio_parse_dai(child, &dlc[1]);
	of_node_put(child);
	if (ret)
		return ret;

	child = of_get_child_by_name(dev->of_node, "cpu");
	if (!child) {
		ret = -ENODEV;
		goto out_err;
	}

	ret = sun9i_hdmi_audio_parse_dai(child, &dlc[0]);
	of_node_put(child);
	if (ret)
		goto out_err;

	dlc[2].of_node = dlc[0].of_node;

	platform_set_drvdata(pdev, card);

	link->cpus = &dlc[0];
	link->codecs = &dlc[1];
	link->platforms = &dlc[2];

	link->num_cpus = 1;
	link->num_codecs = 1;
	link->num_platforms = 1;

	link->playback_only = 1;

	link->name = "SUN9I-HDMI";
	link->stream_name = "SUN9I-HDMI PCM";

	link->dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_IF | SND_SOC_DAIFMT_CBS_CFS;

	link->ops = &sun9i_hdmi_audio_ops;
	link->init = sun9i_hdmi_audio_dai_init;

	card->dai_link = link;
	card->num_links = 1;
	card->owner = THIS_MODULE;
	card->dev = dev;
	card->name = "sun9i-hdmi";

	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		goto out_err;

	return 0;

out_err:
	of_node_put(dlc[0].of_node);
	of_node_put(dlc[1].of_node);

	return ret;
}

static int sun9i_hdmi_audio_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	of_node_put(card->dai_link->cpus->of_node);
	of_node_put(card->dai_link->codecs->of_node);

	return 0;
}

static const struct of_device_id sun9i_hdmi_audio_match[] = {
	{ .compatible = "allwinner,sun9i-a80-hdmi-audio" },
	{}
};
MODULE_DEVICE_TABLE(of, sun9i_hdmi_audio_match);

static struct platform_driver sun9i_hdmi_audio_driver = {
	.probe = sun9i_hdmi_audio_probe,
	.remove = sun9i_hdmi_audio_remove,
	.driver = {
		.name = "sun9i-hdmi-audio",
		.of_match_table = sun9i_hdmi_audio_match,
	},
};
module_platform_driver(sun9i_hdmi_audio_driver);

MODULE_DESCRIPTION("sun9i HDMI Audio Sound Card");
MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@gmail.com>");
MODULE_LICENSE("GPL v2");
