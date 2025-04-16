#include <linux/skbuff.h>
#include "hci_trans.h"
#include "hci_pkt.h"
#include "slsi_bt_property.h"
#include "slsi_bt_log.h"

typedef int (*vs_handler)(struct hci_trans *htr, struct sk_buff *skb);
#define UNKNOWN_TAG (0)

enum mxlog_tags {
	MXLOG_LOG_EVENT_IND   = 0x01,
	MXLOG_LOGMASK_SET_REQ = 0x02,
};

static inline unsigned char get_tag(struct sk_buff *skb)
{
	const int idx = 0;

	if (skb && skb->len > idx)
		return skb->data[idx];
	return UNKNOWN_TAG;
}

static inline unsigned char get_len(struct sk_buff *skb)
{
	const int idx = 1;

	if (skb && skb->len > idx)
		return skb->data[idx];
	return 0;
}

static inline unsigned char *get_value(struct sk_buff *skb)
{
	const int idx = 2;

	if (skb && skb->len > idx)
		return &skb->data[2];
	return NULL;
}

static int vs_channel_mxlog_handler(struct hci_trans *htr, struct sk_buff *skb)
{
	struct sk_buff *resp_skb = NULL;
	unsigned char tag, len, *value;
	int ret = 0;

	while ((len = get_len(skb)) > 0) {
		tag = get_tag(skb);
		value = get_value(skb);
		if (value == NULL || len > skb->len - 2) {
			ret = -EINVAL;
			break;
		}

		switch (tag) {
		case MXLOG_LOG_EVENT_IND:
			slsi_bt_mxlog_log_event(len, value);
			slsi_bt_fwlog_dump(value, len);
			resp_skb = NULL;
			break;

		default:
			BT_WARNING("Unknown tag property tag: %u\n", tag);
			resp_skb = alloc_hci_pkt_skb(2);
			if (resp_skb) {
				skb_put_data(resp_skb, skb->data, 2);
				SET_HCI_PKT_TYPE(resp_skb, HCI_PROPERTY_PKT);
			}
		}

		if (resp_skb) {
			ret = hci_trans_send_skb(htr, resp_skb);
			if (ret)
				break;
		}

		skb_pull(skb, len + 2);
	}
	kfree_skb(skb);
	return ret;
}

static vs_handler vse_ch_handlers[] = {
	NULL,
	vs_channel_mxlog_handler,
};

static inline unsigned char get_channel(struct sk_buff *skb)
{
	if (skb && skb->len > 0 && skb->data[0] < ARRAY_SIZE(vse_ch_handlers))
		return skb->data[0];
	return SLSI_BTP_VS_CHANNEL_UNKNOWN;
}

static int property_recv_hook(struct hci_trans *htr, struct sk_buff *skb)
{
	unsigned char channel;

	if (!skb || GET_HCI_PKT_TYPE(skb) != HCI_PROPERTY_PKT)
		return hci_trans_recv_skb(htr, skb);

	/* TODO: remove */
	if (GET_HCI_PKT_TR_TYPE(skb) == HCI_TRANS_H4) {
		skb_pull(skb, 1);
		SET_HCI_PKT_TR_TYPE(skb, HCI_TRANS_HCI);
	}

	channel = get_channel(skb);
	if (channel != SLSI_BTP_VS_CHANNEL_UNKNOWN &&
	    vse_ch_handlers[channel]) {
		skb_pull(skb, 1); // remove channel
		return vse_ch_handlers[channel](htr, skb);
	}
	kfree_skb(skb);
	BT_WARNING("Unsupported channel: %u\n", channel);
	return -EINVAL;
}

#ifdef CONFIG_SLSI_BT_FWLOG_SNOOP
static struct sk_buff *vse_fwlog_snoop_new_skb(void)
{
	const unsigned char vse_fwlog_snoop[] = {
		/* event code   */ HCI_EVENT_COMMAND_COMPLETE,
		/* length       */ 0x04,
		/* num of cmd   */ 0x01,
		/* command code */ 0xF6, 0xFD, //HCI_VSC_FWLOG_BTSNOOP,
		/* Status       */ HCI_PKT_STATUS_COMPLETE
	};
	struct sk_buff *skb = alloc_hci_pkt_skb(sizeof(vse_fwlog_snoop));

	if (skb) {
		skb_put_data(skb, vse_fwlog_snoop, sizeof(vse_fwlog_snoop));
		SET_HCI_PKT_TYPE(skb, HCI_EVENT_PKT);
	}
	return skb;
}

static int vsc_fwlog_snoop_handler(struct hci_trans *htr, struct sk_buff *skb)
{
	struct fw_snoop_handler_packet_struct {
		unsigned short opcode;
		unsigned char  length;
		unsigned char  enabled;
		unsigned int   filters[4];
	} *packet = NULL;
	int packet_size = sizeof(struct fw_snoop_handler_packet_struct);
	int ret = 0;

	if (skb && skb->len == packet_size) {
		if (packet_size != skb->len) {
			BT_WARNING("invalud size of fwlog_snoop command\n");
			return ret;
		}
		packet = (struct fw_snoop_handler_packet_struct *)skb->data;

		BT_DBG("VSC fwlog btsnoop enabled: %d\n", packet->enabled);
		if (packet->enabled)
			slsi_bt_fwlog_snoop_enable(packet->filters);
		else
			slsi_bt_fwlog_snoop_disable();

		kfree_skb(skb);

		// fallback
		skb = vse_fwlog_snoop_new_skb();
		ret = hci_trans_recv_skb(htr, skb);
	}
	return ret;
}
#endif

struct command_hook{
	char            *name;
	unsigned short  command;
	vs_handler      handler;
};

struct command_hook hook_commands[] = {
#ifdef CONFIG_SLSI_BT_FWLOG_SNOOP
	{
		.name    = "HCI_VSC_FWLOG_BTSNOOP",
		.command = HCI_VSC_FWLOG_BTSNOOP,
		.handler = vsc_fwlog_snoop_handler,
	},
#endif
};

static vs_handler get_hook_handler(struct sk_buff *skb)
{
	if (!skb || GET_HCI_PKT_TR_TYPE(skb) != HCI_TRANS_HCI)
		return NULL;

	if (GET_HCI_PKT_TYPE(skb) == HCI_COMMAND_PKT) {
		unsigned short i, command = 0;

		command = hci_pkt_get_command(skb->data, skb->len);
		for (i = 0; i < ARRAY_SIZE(hook_commands); i++)
			if (command == hook_commands[i].command)
				return hook_commands[i].handler;
	}
	return NULL;
}

static int property_write_hook(struct hci_trans *htr, struct sk_buff *skb)
{
	vs_handler handler = get_hook_handler(skb);

	if (skb && handler) {
		int ret = skb->len;

		BT_DBG("write hook\n");
		if (handler(htr, skb))
			return ret;
	}
	return hci_trans_send_skb(htr, skb);
}

static int property_proc_show(struct hci_trans *htr, struct seq_file *m)
{
	int i;

	seq_printf(m, "\n  %s:\n", hci_trans_get_name(htr));
	seq_printf(m, "    registered VSCs (%lu)\n", ARRAY_SIZE(hook_commands));
	for (i = 0; i < ARRAY_SIZE(hook_commands); i++)
		seq_printf(m, "      - %s (0x%02X%02X)\n",
			hook_commands[i].name,
			hook_commands[i].command & 0xFF,
			(hook_commands[i].command >> 8) & 0xFF);

	return 0;
}

int slsi_bt_property_init(struct hci_trans *htr, bool reverse)
{
	if (htr == NULL)
		return -EINVAL;

	htr->send_skb = property_write_hook;
	htr->recv_skb = property_recv_hook;
	htr->proc_show = property_proc_show;

	return 0;
}

int slsi_bt_property_set_logmask(struct hci_trans *htr,
		const unsigned int *val, const unsigned char len)
{
	const int size = (sizeof(unsigned int)*len)+1+2; /* channel + TL */
	struct sk_buff *skb = NULL;

	if (htr == NULL || val == NULL)
		return -EINVAL;

	skb = alloc_hci_pkt_skb(size);
	if (skb == NULL)
		return -ENOMEM;

	if (len >= 2)
		BT_DBG("set logmask: 0x%016x%016x\n", val[0], val[1]);
	else
		BT_DBG("set logmask: 0x%016x\n", val[0]);

	SET_HCI_PKT_TYPE(skb, HCI_PROPERTY_PKT);
	SET_HCI_PKT_TR_TYPE(skb, HCI_TRANS_HCI);

	skb_put_u8(skb, SLSI_BTP_VS_CHANNEL_MXLOG);
	skb_put_u8(skb, MXLOG_LOGMASK_SET_REQ);
	skb_put_u8(skb, sizeof(unsigned int)*len);
	skb_put_data(skb, val, sizeof(unsigned int)*len);

	BT_DBG("MXLOG_LOGMASK_SET_REQ\n");
	return hci_trans_send_skb(htr, skb);
}
