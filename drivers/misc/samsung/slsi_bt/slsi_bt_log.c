#include <linux/module.h>
#include <linux/ctype.h>

#ifdef CONFIG_SCSC_LOG_COLLECTION
#include <scsc/scsc_log_collector.h>
#endif

#include "slsi_bt_log.h"
#include "slsi_bt_property.h"
#include "hci_pkt.h"
#include "hci_h4.h"

#include "../scsc/scsc_mx_impl.h"
#include "../scsc/mxlog.h"
#include "../scsc/mxlog_transport.h"
#include "slsi_bt_controller.h"

static void *property_data;
static struct firm_log_filter fw_filter;

static unsigned int dev_host_tr_data_log_filter = BTTR_LOG_FILTER_DEFAULT;
module_param(dev_host_tr_data_log_filter, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(dev_host_tr_data_log_filter, "Host transport layer log filter");

/* slsi_btlog_enables enables log options of bt fw.
 * For now, Nov 2021, It only sets at the service start. BT UART will move
 * BSMHCP to property type of command. Then, It should be update to set any
 * time.
 */
static struct kernel_param_ops slsi_btlog_enables_ops;
module_param_cb(btlog_enables, &slsi_btlog_enables_ops, NULL, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(btlog_enables,
	"Set the enables for btlog sources in Bluetooth firmware (127..0)");

/* Legacy. Recommend to use btlog_enables. */
static struct kernel_param_ops slsi_mxlog_filter_ops;
module_param_cb(mxlog_filter, &slsi_mxlog_filter_ops, NULL, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(mxlog_filter,
	 "Set the enables for btlog sources in Bluetooth firmware (31..0)");

static void update_fw_logfilter(void)
{
#ifdef CONFIG_SLSI_BT_USE_UART_INTERFACE
	struct hci_trans *htr = (struct hci_trans *)property_data;

	slsi_bt_property_set_logmask(htr, fw_filter.uint32, 4);
#else
	slsi_bt_controller_update_fw_log_filter(fw_filter.uint64);
#endif
}

#ifdef CONFIG_SLSI_BT_FWLOG_SNOOP
/***
 * BTFW SNOOP
 */
static unsigned int fwsnoop_notify_packets = 1;
module_param(fwsnoop_notify_packets, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(fwsnoop_notify_packets,
		"Set the auto notification counter");

static struct sk_buff_head fwlogdump_q;
static struct mutex fwlogdump_q_lock;
static bool fwlog_snoop_enable = false;

/*
 * |  0  |  1 |  2 |  3 |  4 |  5 |  6 |  7 |  8 |  9 ....
 * | dump header                                 | dump data
 * | H4  | ACL_DATA Header   | L2CAP Header      | payload
 * | TYPE| CONN ID | LENGTH  | LENGTH  |   CID   | payload
 * | ACLD| 0x2EEF  | length1 | length2 | Null id | dump data
 */
static const unsigned char fwlogdump_h[9] = { HCI_ACL_DATA_PKT, 0xEF, 0x2E };

static inline void _put_u16(unsigned char *p, unsigned short d)
{
	/* Little endian to serialize*/
	*p++ = d & 0xFF;
	*p = (d>>8) & 0xFF;
}

static inline struct sk_buff *fwlogdump_new_skb(size_t size)
{
	struct sk_buff *skb = alloc_hci_h4_pkt_skb(size);

	if (skb) {
		skb_put_data(skb, fwlogdump_h, sizeof(fwlogdump_h));

		/* CID is NULL ID */
		_put_u16(skb->data+7, 0);
	}
	return skb;
}

static void inline fwlogdump_update_header(struct sk_buff *skb)
{
	if (skb && skb->len > sizeof(fwlogdump_h)) {
		/* Update the length of payloader in ACL header */
		_put_u16(skb->data+3, skb->len-5);

		/* Update the length of payloader in L2CAP header */
		_put_u16(skb->data+5, skb->len-9);
	}
}

static bool _slsi_bt_fwlogdump_dump(unsigned char *data, size_t datalen)
{
	const size_t dump_data_length = 1021-4;
	const size_t max_data_len = sizeof(fwlogdump_h) + dump_data_length;
	const size_t auto_notify_len = fwsnoop_notify_packets;
	struct sk_buff *skb = skb_peek_tail(&fwlogdump_q);
	int ret = false;

	if (skb && (max_data_len-(skb->len+1) < datalen)) {
		/* queue is enough to send to host */
		if (skb_queue_len(&fwlogdump_q) >= auto_notify_len)
			ret = true;
		skb = NULL;
	}
	skb = skb ? skb_dequeue_tail(&fwlogdump_q) :
		    fwlogdump_new_skb(max_data_len);
	if (skb) {
		skb_put_u8(skb, datalen&0xFF);
		skb_put_data(skb, data, datalen);
		fwlogdump_update_header(skb);

		skb_queue_tail(&fwlogdump_q, skb);
	}
	else
		BT_WARNING("allocation for dump log is failed\n");

	return ret;
}

void slsi_bt_fwlog_dump(unsigned char *data, size_t datalen)
{
	struct hci_trans *htr = (struct hci_trans *)property_data;
	bool notify = false;

	if (htr && fwlog_snoop_enable) {
		mutex_lock(&fwlogdump_q_lock);
		notify = _slsi_bt_fwlogdump_dump(data, datalen);
		mutex_unlock(&fwlogdump_q_lock);

		if (notify)
			htr->recv_skb(htr, NULL);
	}
}

static void _slsi_bt_fwlogdump_queue_tail(struct sk_buff_head *q, int min)
{
	struct sk_buff *skb = NULL;

	while (skb_queue_len(&fwlogdump_q) > min) {
		skb = skb_dequeue(&fwlogdump_q);
		skb_queue_tail(q, skb);
	}
}

void slsi_bt_fwlog_snoop_queue_tail(const void *queue, bool complete_only)
{
	struct sk_buff_head *q = (struct sk_buff_head *)queue;

	if (queue && fwlog_snoop_enable) {
		mutex_lock(&fwlogdump_q_lock);
		if (complete_only) /* Do not push incomplete paket */
			_slsi_bt_fwlogdump_queue_tail(q, 1);
		else if (!skb_queue_empty(&fwlogdump_q))
			skb_queue_splice_tail_init(&fwlogdump_q, q);
		mutex_unlock(&fwlogdump_q_lock);
	}
}

void slsi_bt_fwlog_snoop_enable(unsigned int filter[4])
{
	fwlog_snoop_enable = true;

	if (fw_filter.en0_l == 0) {
		/* Caution the order of 4 bytes filter is reserved */
		fw_filter.en1_h = filter[0];
		fw_filter.en1_l = filter[1];
		fw_filter.en0_h = filter[2];
		fw_filter.en0_l = filter[3];
		update_fw_logfilter();
		BT_DBG("fwsnoop enabled\n");
	} else
		BT_DBG("fwsnoop enabled but filter is not updated\n");
}

void slsi_bt_fwlog_snoop_disable(void)
{
	fwlog_snoop_enable = false;
	BT_DBG("fwsnoop disabled\n");
}
#endif /* CONFIG_SLSI_BT_FWLOG_SNOOP */

/**
 * MXLOG_LOG_EVENT_IND
 *
 * MXLOG via BCSP interface specification
 *
 * value encoding is:
 *
 *  | 1st |2nd|3rd|4th|5th|6th|7th|8th|9th|10th ~ 13rd|each 4bytes|...
 *  ------------------------------------------------------------------
 *  |level|   time stamp  |index of format|argument 1 |argument N |.
 *  ------------------------------------------------------------------
 *
 */
void slsi_bt_mxlog_log_event(const unsigned char len, const unsigned char *val)
{
	struct scsc_mx *mx = (struct scsc_mx *)slsi_bt_controller_get_mx();
	struct mxlog_transport *mtrans;
	const int lv = 0, msg = 1;
	const int min = 9;

	if (mx == NULL) {
		BT_WARNING("failed to get mx handler\n");
		return;
	} else if (len < min || val == NULL) {
		BT_WARNING("Invalid data\n");
		return;
	}

#if defined(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	mtrans = scsc_mx_get_mxlog_transport_wpan(mx);
#else
	mtrans = scsc_mx_get_mxlog_transport(mx);
#endif
	if (mtrans) {
		unsigned int header, tmp;
		unsigned char phase, level;

		mutex_lock(&mtrans->lock);
		if (mtrans->header_handler_fn == NULL ||
		    mtrans->channel_handler_fn == NULL) {
			BT_WARNING("mxlog transport is not opened.\n");
			mutex_unlock(&mtrans->lock);
			return;
		}

		header = (SYNC_VALUE_PHASE_5 << 16) | (val[lv] << 8);
		mtrans->header_handler_fn(header, &phase, &level, &tmp);
		mtrans->channel_handler_fn(phase,
					   (void *)&val[msg],
					   (size_t)len - 1,
					   level,
					   mtrans->channel_handler_data);
		mutex_unlock(&mtrans->lock);
	} else
		BT_WARNING("failed to get mxlog transport.\n");
}

void slsi_bt_log_data_hex(const char *func, unsigned int tag,
		const unsigned char *data, size_t len)
{
	char buf[SLSI_BT_LOG_DATA_BUF_MAX + 1];
	char *tags, isrx;
	size_t idx = 0, offset = 0;
	const int head = 10,
		  tail = 10;

	if(!((dev_host_tr_data_log_filter & tag) & 0xF0))
		return;

	switch (tag) {
	case BTTR_TAG_USR_TX:
	case BTTR_TAG_USR_RX:  tags = "User"; break;
	case BTTR_TAG_H4_TX:
	case BTTR_TAG_H4_RX:   tags = "H4"; break;
	case BTTR_TAG_BCSP_TX:
	case BTTR_TAG_BCSP_RX: tags = "BCSP"; break;
	case BTTR_TAG_SLIP_TX:
	case BTTR_TAG_SLIP_RX: tags = "SLIP"; break;
	default:
		tags = "Unknown";
		break;
	}

	if (len <= head + tail) {
		while (idx < len && offset + 4 < SLSI_BT_LOG_DATA_BUF_MAX)
			offset += snprintf(buf + offset,
					SLSI_BT_LOG_DATA_BUF_MAX - offset,
					"%02x ", data[idx++]);
	} else {
		while (idx < head && offset + 4 < SLSI_BT_LOG_DATA_BUF_MAX)
			offset += snprintf(buf + offset,
					SLSI_BT_LOG_DATA_BUF_MAX - offset,
					"%02x ", data[idx++]);
		offset += snprintf(buf + offset,
					SLSI_BT_LOG_DATA_BUF_MAX - offset,
					" ..(%d bytes).. ",
					(int)len - head - tail);
		idx = len - tail;
		while (idx < len && offset + 4 < SLSI_BT_LOG_DATA_BUF_MAX)
			offset += snprintf(buf + offset,
					SLSI_BT_LOG_DATA_BUF_MAX - offset,
					"%02x ", data[idx++]);
	}

	isrx = (tag & 0x1);
	if (offset + 5 < SLSI_BT_LOG_DATA_BUF_MAX)
		TR_DATA(isrx, "%20s %s-%s(%3zu): %s\n", func, tags,
				isrx ? "RX" : "TX", len, buf);
	else
		TR_DATA(isrx, "%20s %s-%s(%3zu): %s...\n", func, tags,
				isrx ? "RX" : "TX", len, buf);
}

/* btlog string
 *
 * The string must be null-terminated, and may also include a single
 * newline before its terminating null. The string shall be given
 * as a hexadecimal number, but the first character may also be a
 * plus sign. The maximum number of Hexadecimal characters is 32
 * (128bits)
 */
static inline bool hex_prefix_check(const char *s)
{
	if (s[0] == '0' && tolower(s[1]) == 'x' && isxdigit(s[2]))
		return true;
	return false;
}

static int btlog_enables_set(const char *val, const struct kernel_param *kp)
{
	const int max_size = strlen("+0x\n")+1+32; /* 128 bit */
	const int hexprefix = strlen("0x");
	const int split_size = 16;

	unsigned long long filter[2];
	size_t len = 0, split = 0;
	int ret = 0;

	if (val == NULL)
		return -EINVAL;

	len = strnlen(val, max_size);
	if (len > 0 && len < max_size) {
		if (val[0] == '+') { /* support regacy */
			val++;
			len--;
		}
		if (val[len-1] == '\n')
			len--;
	} else if (len >= max_size)
		return -ERANGE;

	if (len <= hexprefix || !hex_prefix_check(val))
		return -EINVAL;

	if (len - hexprefix > split_size) {
		char buf[20]; /* It's bigger than split_size + hexprefix + 1 */
		split = len - split_size;
		strncpy(buf, val, split);
		ret = kstrtou64(buf, 16, &filter[1]);
		if (ret != 0)
			return ret;
	}
	ret = kstrtou64(val+split, 16, &filter[0]);
	if (ret != 0)
		return ret;

	// update confirmed filter
	memcpy(fw_filter.uint64, filter, sizeof(filter));
	update_fw_logfilter();
	return ret;
}


static int btlog_enables_get(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "btlog_enables = 0x%08x%08x%08x%08x\n",
			fw_filter.en1_h ,fw_filter.en1_l,
			fw_filter.en0_h, fw_filter.en0_l);
}

static struct kernel_param_ops slsi_btlog_enables_ops = {
	.set = btlog_enables_set,
	.get = btlog_enables_get,
};

static int mxlog_filter_set(const char *val, const struct kernel_param *kp)
{
	int ret;
	u32 value;

	ret = kstrtou32(val, 0, &value);
	if (!ret) {
		fw_filter.uint32[0] = value;
		update_fw_logfilter();
	}

	return ret;
}

static int mxlog_filter_get(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "mxlog_filter=0x%08x\n", fw_filter.uint32[0]);
}

static struct kernel_param_ops slsi_mxlog_filter_ops = {
	.set = mxlog_filter_set,
	.get = mxlog_filter_get,
};

#ifdef CONFIG_SCSC_LOG_COLLECTION
static struct bt_hcf {
	unsigned char *data;
	size_t size;
} bt_hcf = { NULL, 0 };

static int bt_hcf_collect(struct scsc_log_collector_client *client, size_t size)
{
	if (bt_hcf.data) {
		BT_DBG("Collecting BT config file\n");
		return scsc_log_collector_write(bt_hcf.data, bt_hcf.size, 1);
	}
	return 0;
}

static struct scsc_log_collector_client bt_collect_hcf_client = {
	.name = "bt_hcf",
	.type = SCSC_LOG_CHUNK_BT_HCF,
	.collect_init = NULL,
	.collect = bt_hcf_collect,
	.collect_end = NULL,
	.prv = &bt_hcf,
};

void bt_hcf_collect_store(void *hcf, size_t size)
{
	if (hcf && size > 0) {
		if (bt_hcf.data)
			kfree(bt_hcf.data);
		bt_hcf.data = kmalloc(size, GFP_KERNEL);
		memcpy(bt_hcf.data, hcf, size);
		bt_hcf.size = size;
		scsc_log_collector_register_client(&bt_collect_hcf_client);
	}
}

void bt_hcf_collect_free(void)
{
	/* Deinit HCF log collection */
	if (bt_hcf.data) {
		scsc_log_collector_unregister_client(&bt_collect_hcf_client);
		kfree(bt_hcf.data);
		bt_hcf.data = NULL;
		bt_hcf.size = 0;
	}
}

void bt_hcf_collection_request(void)
{
	scsc_log_collector_schedule_collection(
			SCSC_LOG_HOST_BT, SCSC_LOG_HOST_BT_REASON_HCI_ERROR);
}
#endif

struct firm_log_filter slsi_bt_log_filter_get(void)
{
	return fw_filter;
}

void slsi_bt_log_set_transport(void *data)
{
	property_data = data;
	if (data)
		slsi_bt_controller_update_fw_log_filter(fw_filter.uint64);

#ifdef CONFIG_SLSI_BT_FWLOG_SNOOP
	if (data) {
		BT_DBG("fwlogdump init\n");
		skb_queue_head_init(&fwlogdump_q);
		mutex_init(&fwlogdump_q_lock);
	} else {
		BT_DBG("fwlogdump deinit\n");
		skb_queue_purge(&fwlogdump_q);
		mutex_destroy(&fwlogdump_q_lock);
	}
#endif
}
