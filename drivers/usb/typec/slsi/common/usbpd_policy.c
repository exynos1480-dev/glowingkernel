/*
*	USB PD Driver - Policy Engine
*/

#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/usb/typec/slsi/common/usbpd.h>
#include <linux/usb/typec/slsi/common/usbpd_log.h>
#include <linux/usb/typec/common/pdic_core.h>
#include <linux/delay.h>
#include <linux/completion.h>

#include <linux/muic/common/muic.h>
#if IS_ENABLED(CONFIG_MUIC_NOTIFIER)
#include <linux/muic/common/muic_notifier.h>
#endif /* CONFIG_MUIC_NOTIFIER */

#include <linux/usb_notify.h>

#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
#include <linux/usb/typec/slsi/common/usbpd_ext.h>
#endif

#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
#include "../../../../battery/common/sec_charging_common.h"
#if IS_ENABLED(CONFIG_BATTERY_NOTIFIER)
#include <linux/battery/battery_notifier.h>
#else
#include <linux/battery/sec_pd.h>
#endif
#else //disabled CONFIG_BATTERY_SAMSUNG
#include <linux/battery/sec_battery_common.h>
#include <linux/battery/sec_pd.h>
#endif

#define CHECK_MSG(pd, msg, ret) do {\
	if (ms > 5)\
		if (pd->phy_ops.get_status(pd, msg))\
			return ret;\
	} while (0);

#define CHECK_MSG_RETURN(pd, msg, ret) do {\
	if (pd->phy_ops.get_status(pd, msg))\
		return ret;\
	} while (0);

#define CHECK_VDM(pd, msg, ret) do {\
	if (pd->phy_ops.get_status(pd, msg)) {\
		policy->got_ufp_vdm = 1;\
		return ret;\
	}\
	} while (0);


#define CHECK_CMD(pd, event, ret, delay) do {\
		if (pd->manager.cmd & event && ms >= delay) {\
			pd->manager.cmd &= ~event; \
			return ret;\
		} \
	} while (0);

#define CHECK_MSG_BREAK(msg, ret_state)\
	if (PDIC_OPS_PARAM_FUNC(get_status, pd_data, msg)) {\
		ret = ret_state;\
		break;\
	}

#define SOFT_RESET() do {\
		int pr = 0;\
		PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &pr);\
			usbpd_info("%s, line(%d) MSG_ERROR, goto SoftReset\n", __func__, __LINE__);\
		if (pr == USBPD_SOURCE)\
			ret = PE_SRC_Send_Soft_Reset;\
		else\
			ret = PE_SNK_Send_Soft_Reset;\
		} while(0);\

policy_state usbpd_policy_src_startup(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Startup;
	long long ms = 0;
	int vbus_check = 0;

	/**********************************************
	Actions on entry:
	Reset CapsCounter
	Reset Protocol Layer
	Start SwapSourceStartTimer (only after Swap)
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* CapsCounter Reset */
	pd_data->counter.caps_counter = 0;

	/* PD Protocol Initialization */
	if (policy->cap_mismatch) {
		usbpd_manager_remove_new_cap(pd_data);
	}
	usbpd_init_protocol(pd_data);
	PDIC_OPS_FUNC(soft_reset, pd_data);

	/* For tSrcrecover after PE_SRC_Transition_to_default */
	if (policy->txhardresetflag == 1) {
		policy->txhardresetflag = 0;

		usbpd_timer1_start(pd_data);
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_SRC_Startup;
				break;
			}
			ms = usbpd_check_time1(pd_data);
			if (ms >= 200)
				break;
		}
	}

	/* Check Valid VBUS */
	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			dev_info(pd_data->dev, "%s plug is not valid\n", __func__);
			ret = PE_SRC_Startup;
			break;
		}

		vbus_check = PDIC_OPS_FUNC(vbus_on_check, pd_data);

		if (vbus_check < 0 || vbus_check > 0) {
			dev_info(pd_data->dev, "%s vbus_check = %d\n", __func__, vbus_check);
			/* Delay for Charger IRQ */
			msleep(30);

			/* Configuration Channel On */
			PDIC_OPS_PARAM_FUNC(set_pd_control, pd_data, USBPD_CC_ON);
			ret = PE_SRC_Send_Capabilities;
			break;
		}

		ms = usbpd_check_time1(pd_data);
		if (ms >= 1000) {
			dev_info(pd_data->dev, "%s Timer Over\n", __func__);
			break;
		}
	}

	return ret;
}

policy_state usbpd_policy_src_discovery(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Initialize and run SourceCapabilityTimer
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Delay*/
	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SRC_Discovery;
			break;
		}
		ms = usbpd_check_time1(pd_data);
		if (ms >= tTypeCSendSourceCap)
			break;
	}

	if (ret != PE_SRC_Discovery)
	/* Caps Counter Check */
		ret = (pd_data->counter.caps_counter <= USBPD_nCapsCount) ? PE_SRC_Send_Capabilities : PE_SRC_Disabled;

	return ret;
}

policy_state usbpd_policy_src_send_capabilities(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Request present source capabilities from Device Policy Manager
	Send PD Capabilities message
	Increment CapsCounter (optional)
	If GoodCRC received:
	- stop NoResponseTimer
	- reset HardResetCounter and CapsCounter
	- initialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Source Capabilities PDO Read & Write */
	policy->tx_msg_header.word = pd_data->source_msg_header.word;
	policy->tx_data_obj[0].object = pd_data->source_data_obj[0].object;
	policy->tx_data_obj[1].object = pd_data->source_data_obj[1].object;

	/* Interrupt Status Bit Clear  */
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_REQUEST);
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_GET_SNK_CAP);

	/* Add Caps Counter */
	pd_data->counter.caps_counter++;

	/* Send Message */
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		if (policy->pd_support) {
			SOFT_RESET();
		} else
			ret = PE_SRC_Discovery;
	} else {
		dev_info(pd_data->dev, "%s got GoodCRC.\n", __func__);
		/* Start Timer */
		usbpd_timer1_start(pd_data);	// Setting 25ms is actual 25 ~ 29ms
		/* Wait Message or State */
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_SRC_Send_Capabilities;
				break;
			}

			if (policy->rx_hardreset) {
				ret = 0;
				break;
			}

			CHECK_MSG_BREAK(MSG_REQUEST, PE_SRC_Negotiate_Capability);
			/* TD.PD.SRC.E14 Atomic Message Sequence */
			CHECK_MSG_BREAK(MSG_GET_SNK_CAP, PE_SRC_Send_Soft_Reset);
			CHECK_MSG_BREAK(MSG_PING, PE_SRC_Send_Soft_Reset);

			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
					dev_info(pd_data->dev, "%s tSenderResponse Timer over\n", __func__);
					CHECK_MSG_BREAK(MSG_GET_SNK_CAP, PE_SRC_Send_Soft_Reset);

					ret = (pd_data->counter.hard_reset_counter > USBPD_nHardResetCount)
						? Error_Recovery : PE_SRC_Hard_Reset;
					break;
			}

			if (ms >= 100) {
				ret = PE_SRC_Discovery;
				break;
			}
		}
	}

	if (ret == PE_SRC_Negotiate_Capability) {
		pd_data->counter.hard_reset_counter = 0;
		pd_data->counter.caps_counter = 0;
		dev_info(pd_data->dev, "%s got Request.\n", __func__);
	}

	return ret;
}

policy_state usbpd_policy_src_negotiate_capability(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Negotiate_Capability;
	int data_role = 0;

	/**********************************************
	Actions on entry:
	Get Device Policy Manager evaluation of sink request:
	- Can be met
	- Can??t be met
	- Could be met later from Power Reserve
	If the sink request for Operating Current or Operating Power can be met,
	but the sink still requires more power Capability Mismatch this
	information will be passed to Device Policy Manager
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->source_request_obj.object = policy->rx_data_obj[0].object;

#if IS_ENABLED(CONFIG_PDIC_PD30)
	/* Check Specification Revision */
	if(pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0)
		PDIC_OPS_PARAM_FUNC(set_revision, pd_data, USBPD_PD3_0);
	else
		PDIC_OPS_PARAM_FUNC(set_revision, pd_data, USBPD_PD2_0);
#endif


	/* Get Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	mutex_lock(&pd_data->accept_mutex);
	/* Analysis Received Request Message */
	if (!(usbpd_manager_match_request(pd_data) == 0) && policy->send_reject == 0) {
		ret = PE_SRC_Capability_Response; /* Reject */
	} else {
		msg_header_type *tx_h = &policy->tx_msg_header;

		if (!usbpd_send_ctrl_msg(pd_data, tx_h, USBPD_Accept, data_role, USBPD_SOURCE)) {
			SOFT_RESET();
		} else {
			usbpd_info("%s, accept Sent\n", __func__);
			if (policy->cap_mismatch && pd_data->source_get_sink_obj.object == 0)
				usbpd_manager_inform_event(pd_data, MANAGER_CAP_MISMATCH);
			ret = PE_SRC_Transition_Supply; /* Accept */
		}
	}
	mutex_unlock(&pd_data->accept_mutex);

	return ret;
}

policy_state usbpd_policy_src_transition_supply(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Ready;
	long long ms = 0;
	int data_role = 0;

	/**********************************************
	Actions on entry:
	Initialize and run SourceActivityTimer (see Section 8.3.3.5)
	If GotoMin send GotoMin message
	Else send Accept message (within tReceiverResponse)
	Wait tSrcTransition and request Device Policy Manager to transition Power Supply

	Actions on exit:
	Send PS_RDY message
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);
	/* Get Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Send Message */
	// Move to PE_SRC_Nego, accept_mutex;
	// usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Accept, USBPD_DFP, USBPD_SOURCE);

	/* Start Timer */
	usbpd_timer1_start(pd_data);

	/* Wait Message or State */
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SRC_Transition_Supply;
			break;
		}
		ms = usbpd_check_time1(pd_data);

		if (ms > tSrcTransition)
			break;
	}

	if (ret == PE_SRC_Transition_Supply)
		return ret;

	usbpd_info("%s, cur [%d, %s] -> requested [%d, %s]\n", __func__,
			policy->selected_pdo_num, pdo_type_to_str[policy->selected_pdo_type],
			policy->requested_pdo_num, pdo_type_to_str[policy->requested_pdo_type]);

	/* if (reqeuested_pdo_num is changed) or (request_pdo_num's pdo_type is changed) */
	if ((policy->selected_pdo_num != policy->requested_pdo_num) ||
			(policy->selected_pdo_type != policy->requested_pdo_type)) {
		/* need to change output voltage */
		usbpd_manager_response_req_pdo(pd_data, policy->requested_pdo_type, policy->requested_pdo_num);
	}
	policy->selected_pdo_type = policy->requested_pdo_type;
	policy->selected_pdo_num = policy->requested_pdo_num;


	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_PS_RDY, USBPD_DFP, USBPD_SOURCE)) {
		SOFT_RESET();
	} else
		ret = PE_SRC_Ready;

	return ret;
}

policy_state usbpd_policy_src_ready(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	long long ms = 0;
	bool SinkTxOK = false;

	/**********************************************
	Actions on entry:
	Initialize and run SourceActivityTimer (see Section 8.3.3.5)
	Initialize and run DiscoverIdentityTimer
	**********************************************/

	/* Get Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s, %s, %s\n", __func__, data_role ? "DFP" : "UFP",
			((pd_data->specification_revision == USBPD_PD3_0)?"PD3.0":"PD2.0"));

	policy->pd_src_ready = 1;

#if IS_ENABLED (CONFIG_TYPEC)
	if (policy->pd_support == 0) {
		policy->pd_support = 1;
		PDIC_OPS_PARAM_FUNC(set_pwr_opmode, pd_data, TYPEC_PWR_MODE_PD);
	}
#endif

#if IS_ENABLED(CONFIG_PDIC_PD30)
	/* when PD3.0, set sink Tx Ok(RP 330uA) */
	if (pd_data->specification_revision >= USBPD_PD3_0) {
		PDIC_OPS_PARAM_FUNC(set_rp_control, pd_data, PLUG_CTRL_RP330);
		SinkTxOK = true;
	}
	else
#endif
		PDIC_OPS_PARAM_FUNC(set_rp_control, pd_data, PLUG_CTRL_RP180);

	/* Start Timer */
	usbpd_timer1_start(pd_data);

	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset)
			return 0;

		if (policy->plug_valid == 0)
			return PE_SRC_Ready;

		if (policy->rx_hardreset) {
			policy->rx_hardreset = 0;
			return PE_SRC_Hard_Reset_Received;
		}

		ms = usbpd_check_time1(pd_data);

#if IS_ENABLED(CONFIG_PDIC_PD30)
		/* Specification Revision */
		if(pd_data->specification_revision == USBPD_PD3_0){
			/* PD 3.0 : Control Message */
			CHECK_MSG(pd_data, MSG_GET_SOURCE_CAP_EXTENDED, PE_SRC_Give_Source_Cap_Ext);
			CHECK_MSG(pd_data, MSG_GET_STATUS, PE_Give_Status);
			CHECK_MSG(pd_data, MSG_FR_SWAP, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_PPS_STATUS, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_COUNTRY_CODES, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_SINK_CAP_EXTENDED, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_REVISION, PE_Give_Revision);
			CHECK_MSG(pd_data, MSG_GET_SOURCE_INFO, PE_SRC_Give_Source_Info);

			/* PD 3.0 : Data Message */
			CHECK_MSG(pd_data, MSG_BATTERY_STATUS, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_ALERT, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_COUNTRY_INFO, PE_SRC_Send_Not_Supported);

			/* PD 3.0 : Extended Message */
			CHECK_MSG(pd_data, MSG_SOURCE_CAPABILITIES_EXTENDED, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_STATUS, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_BATTERY_CAP, PE_Give_Battery_Cap);
			CHECK_MSG(pd_data, MSG_GET_BATTERY_STATUS, PE_Give_Battery_Status);
			CHECK_MSG(pd_data, MSG_BATTERY_CAPABILITIES, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_MANUFACTURER_INFO, PE_Give_Manufacturer_Info);
			CHECK_MSG(pd_data, MSG_MANUFACTURER_INFO, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_SECURITY_REQUEST, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_SECURITY_RESPONSE, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_FIRMWARE_UPDATE_REQUEST, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_FIRMWARE_UPDATE_RESPONSE, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_PPS_STATUS, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_COUNTRY_INFO, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_COUNTRY_CODES, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_SINK_CAPABILITIES_EXTENDED, PE_SRC_Send_Not_Supported);

			/* When PD3.0 state, NOT_SUPPORT message is transmitted instead of REJECT. */
			CHECK_MSG(pd_data, MSG_VCONN_SWAP, PE_SRC_Send_Not_Supported);

			/* Unrecognized Message Received in Ready State */
			CHECK_MSG(pd_data, MSG_RESERVED, PE_SRC_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_UVDM_MSG_NOT_SAMSUNG, PE_SRC_Send_Not_Supported);
		}
#endif
		if (pd_data->specification_revision == USBPD_PD2_0) {
			CHECK_MSG(pd_data, MSG_RESERVED, PE_DR_Send_Reject);
			CHECK_MSG(pd_data, MSG_UVDM_MSG_NOT_SAMSUNG, PE_SRC_Ready);
		}

		/* Wait Message or State */
		CHECK_MSG(pd_data, MSG_GET_SRC_CAP, PE_SRC_Send_Capabilities);
		CHECK_MSG(pd_data, MSG_REQUEST, PE_SRC_Negotiate_Capability);
		CHECK_MSG(pd_data, MSG_PR_SWAP, PE_PRS_SRC_SNK_Evaluate_Swap);
		CHECK_MSG(pd_data, MSG_DR_SWAP, PE_DRS_Evaluate_Port);
		CHECK_MSG(pd_data, MSG_VCONN_SWAP, PE_VCS_Evaluate_Swap);
		CHECK_MSG(pd_data, MSG_GET_SNK_CAP, PE_DR_SRC_Give_Sink_Cap);
		CHECK_MSG(pd_data, MSG_BIST, PE_BIST_Carrier_Mode);
		CHECK_MSG(pd_data, MSG_ACCEPT, PE_SRC_Send_Soft_Reset);

		/* Wait VDM */
		if (data_role == USBPD_UFP) {
			CHECK_VDM(pd_data, VDM_DISCOVER_IDENTITY, PE_UFP_VDM_Get_Identity);
			CHECK_VDM(pd_data, VDM_DISCOVER_SVID, PE_UFP_VDM_Get_SVIDs);
			CHECK_VDM(pd_data, VDM_DISCOVER_MODE, PE_UFP_VDM_Get_Modes);
			CHECK_VDM(pd_data, VDM_ENTER_MODE, PE_UFP_VDM_Evaluate_Mode_Entry);
			CHECK_VDM(pd_data, VDM_EXIT_MODE, PE_UFP_VDM_Mode_Exit);
			CHECK_VDM(pd_data, VDM_ATTENTION, PE_DFP_VDM_Attention_Request);
			CHECK_VDM(pd_data, VDM_DP_STATUS_UPDATE, PE_UFP_VDM_Evaluate_Status);
			CHECK_VDM(pd_data, VDM_DP_CONFIGURE, PE_UFP_VDM_Evaluate_Configure);
			CHECK_VDM(pd_data, UVDM_MSG, PE_DFP_UVDM_Receive_Message);
		} else {
			CHECK_MSG(pd_data, VDM_DISCOVER_IDENTITY, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_DISCOVER_SVID, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_DISCOVER_MODE, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_ENTER_MODE, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_EXIT_MODE, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_ATTENTION, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_DP_STATUS_UPDATE, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_DP_CONFIGURE, PE_DFP_VDM_EVALUATE);
		}

		/* Command Check from AP */
		if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
			if (pd_data->manager.cmd && SinkTxOK) {
				usbpd_info("%s, Set SinkTxNG, start tSinkTx\n", __func__);
				SinkTxOK = false;
				PDIC_OPS_PARAM_FUNC(set_rp_control, pd_data, PLUG_CTRL_RP180);
				usbpd_timer1_start(pd_data);
			}
			if (pd_data->source_get_sink_obj.object == 0)
				CHECK_CMD(pd_data, MANAGER_REQ_GET_SNKCAP, PE_SRC_Get_Sink_Cap, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_GOTOMIN, PE_SRC_Transition_Supply, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_SRCCAP_CHANGE, PE_SRC_Send_Capabilities, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_PR_SWAP, PE_PRS_SRC_SNK_Send_Swap, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_DR_SWAP, PE_DRS_Evaluate_Send_Port, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VCONN_SWAP, PE_VCS_Send_Swap, 18);

			CHECK_CMD(pd_data, MANAGER_REQ_UVDM_SEND_MESSAGE, PE_DFP_UVDM_Send_Message, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_IDENTITY, PE_DFP_VDM_Identity_Request, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_SVID, PE_DFP_VDM_SVIDs_Request, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_MODE, PE_DFP_VDM_Modes_Request, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VDM_ENTER_MODE, PE_DFP_VDM_Mode_Entry_Request, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VDM_EXIT_MODE, PE_DFP_VDM_Mode_Exit_Request, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VDM_STATUS_UPDATE, PE_DFP_VDM_Status_Update, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VDM_DisplayPort_Configure, PE_DFP_VDM_DisplayPort_Configure, 18);
			CHECK_CMD(pd_data, MANAGER_REQ_VDM_ATTENTION, PE_UFP_VDM_Attention_Request, 18);
		} else {
#if IS_ENABLED(CONFIG_S2M_SUPPORT_DELAYED_REQUEST_MESSAGE_UFP)
			if ((policy->got_ufp_vdm && ms >= tStartAmsMargin) || (!policy->got_ufp_vdm)) {
				policy->got_ufp_vdm = 0;
#endif
				if (pd_data->source_get_sink_obj.object == 0)
					CHECK_CMD(pd_data, MANAGER_REQ_GET_SNKCAP, PE_SRC_Get_Sink_Cap, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_GOTOMIN, PE_SRC_Transition_Supply, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_SRCCAP_CHANGE, PE_SRC_Send_Capabilities, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_PR_SWAP, PE_PRS_SRC_SNK_Send_Swap, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_DR_SWAP, PE_DRS_Evaluate_Send_Port, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VCONN_SWAP, PE_VCS_Send_Swap, 5);

				CHECK_CMD(pd_data, MANAGER_REQ_UVDM_SEND_MESSAGE, PE_DFP_UVDM_Send_Message, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_IDENTITY, PE_DFP_VDM_Identity_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_SVID, PE_DFP_VDM_SVIDs_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_MODE, PE_DFP_VDM_Modes_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_ENTER_MODE, PE_DFP_VDM_Mode_Entry_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_EXIT_MODE, PE_DFP_VDM_Mode_Exit_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_STATUS_UPDATE, PE_DFP_VDM_Status_Update, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_DisplayPort_Configure, PE_DFP_VDM_DisplayPort_Configure, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_ATTENTION, PE_UFP_VDM_Attention_Request, 5);
#if IS_ENABLED(CONFIG_S2M_SUPPORT_DELAYED_REQUEST_MESSAGE_UFP)
			}
#endif
		}
#if IS_ENABLED(CONFIG_S2M_SUPPORT_DELAYED_REQUEST_MESSAGE_UFP)
		if (ms >= (policy->got_ufp_vdm ? tStartAmsMargin : 0) + 20)
			break;
#else
		if (ms >= 20)
			break;
#endif
	}

	if (data_role == USBPD_DFP)
		usbpd_manager_vdm_request_enabled(pd_data);

	return PE_SRC_Ready;
}

policy_state usbpd_policy_src_disabled(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Disable Power Delivery
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return PE_SRC_Disabled;
}

policy_state usbpd_policy_src_capability_response(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int data_role = 0;

	/**********************************************
	Actions on entry:
	Send Reject message if request can't be met
	Send Wait message if request could be met later from the Power
	Reserve and present Contract is still valid
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Reject, data_role, USBPD_SOURCE)) {
		SOFT_RESET();
	} else
		ret = policy->pd_src_ready ? PE_SRC_Ready : PE_SRC_Discovery;

	policy->send_reject += 1;

	return ret;
}

policy_state usbpd_policy_src_hard_reset(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Transition_to_default;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Generate Hard Reset signalling
	Start PSHardResetTimer
	Increment HardResetCounter
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Message ID Clear */
	usbpd_init_counters(pd_data);

	/* Policy Initialization */
	usbpd_init_policy(pd_data);

	/* Send Hardreset */
	PDIC_OPS_FUNC(hard_reset, pd_data);

	/* Configuration Channel On */
	PDIC_OPS_PARAM_FUNC(set_pd_control, pd_data, USBPD_CC_OFF);

	/* Delay : Setting 25 is actual 57.3ms */
	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SRC_Hard_Reset;
			break;
		}
		ms = usbpd_check_time1(pd_data);
		if (ms >= tPSHardReset)
			break;
	}

	if (ret == PE_SRC_Hard_Reset)
		return ret;

	/* Add Hardreset Counter */
	pd_data->counter.hard_reset_counter++;

	/* Set Tx HardReset Flag After SRC_HADRESET */
	policy->txhardresetflag = 1;

	return PE_SRC_Transition_to_default;
}

policy_state usbpd_policy_src_hard_reset_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Transition_to_default;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Start PSHardResetTimer
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Message ID Clear */
	usbpd_init_counters(pd_data);

	/* Policy Initialization */
	usbpd_init_policy(pd_data);

	/* Delay */
	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SRC_Hard_Reset_Received;
			break;
		}
		ms = usbpd_check_time1(pd_data);
		if (ms >= tPSHardReset)
			break;
	}

	if (ret == PE_SRC_Hard_Reset_Received)
		return ret;

	/* Set Tx HardReset Flag After SRC_HADRESET */
	policy->txhardresetflag = 1;

	return PE_SRC_Transition_to_default;
}

policy_state usbpd_policy_src_transition_to_default(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Startup;
	long long ms = 0;
	int data_role = 0;

	/**********************************************
	Actions on entry:
	Request Device Policy Manager to request power
	supply Hard Resets to vSafe5V via vSafe0V
	Reset local HW
	If Type-C request Device Policy Manager to set
	Port Data Role to DFP and turn off VCONN
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* VBUS Turn off */
	PDIC_OPS_PARAM_FUNC(set_otg_control, pd_data, 0);

	/* Delay */
	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SRC_Transition_to_default;
			break;
		}
		ms = usbpd_check_time1(pd_data);
		if (ms >= tSrcRecover)
			break;
	}

	if (ret == PE_SRC_Transition_to_default)
		return ret;

	/* initial reset */
	PDIC_OPS_FUNC(driver_reset, pd_data);
	usbpd_init_manager_val(pd_data);
	usbpd_init_policy(pd_data);

	PDIC_OPS_PARAM_FUNC(set_otg_control, pd_data, 1);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	if (data_role != USBPD_DFP)
		PDIC_OPS_PARAM_FUNC(set_data_role, pd_data, USBPD_DFP);
	/*
	Request Device Policy Manager to request power
	supply Hard Resets to vSafe5V via vSafe0V

	If(Type-C request Device Policy Manager to set Port Data Role to DFP)
		turn off VCONN
	*/

	/*
	Request Device Policy Manager to turn on VCONN
	Initialize and start NoResponseTimer
	Inform Protocol Layer Hard Reset complete
	*/

	/* confirm VBUS ON : done by set_otg_control */
	return PE_SRC_Startup;
}

policy_state usbpd_policy_src_give_source_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Give_Source_Cap;
	int data_role = 0;
	long long ms = 0;

	/**********************************************
	Action on entry :
	Request source capabilities from Device Policy Manager
	Send Capabilities message
	**********************************************/

	/* PD State Inform for AP */

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Source_Capabilities;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = USBPD_SOURCE;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].power_data_obj.max_current = 500 / 10;
	policy->tx_data_obj[0].power_data_obj.voltage = 5000 / 50;
	policy->tx_data_obj[0].power_data_obj.peak_current = 0;
	policy->tx_data_obj[0].power_data_obj.unchunked_extended_message_supported = 0;
	policy->tx_data_obj[0].power_data_obj.data_role_swap = 1;
	policy->tx_data_obj[0].power_data_obj.usb_comm_capable = 1;
	policy->tx_data_obj[0].power_data_obj.externally_powered = 0;
	policy->tx_data_obj[0].power_data_obj.usb_suspend_support = 1;
	policy->tx_data_obj[0].power_data_obj.dual_role_power = 1;
	policy->tx_data_obj[0].power_data_obj.supply = 0;

	/* Send Message */
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else {
		/* Start Timer */
		usbpd_timer1_start(pd_data);
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_SRC_Give_Source_Cap;
				break;
			}

			CHECK_MSG_BREAK(MSG_REQUEST, PE_SRC_Negotiate_Capability);
			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
				ret = PE_SRC_Hard_Reset;
				break;
			}
		}
	}

	return ret;
}

policy_state usbpd_policy_src_get_sink_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int ret = PE_SRC_Get_Sink_Cap;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Send Get_Sink_Cap message
	Initialize and run
	SenderResponseTimer
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Get_Sink_Cap, data_role, USBPD_SOURCE)) {
		SOFT_RESET();
	} else {
		/* Wait Message */
		usbpd_timer1_start(pd_data);
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0)
				return PE_SRC_Get_Sink_Cap;

			CHECK_MSG_BREAK(MSG_SNK_CAP, PE_SRC_Ready);

			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
				break;
			}
		}
	}

	if (ret == PE_SRC_Ready) {
		pd_data->source_get_sink_obj.object = policy->rx_data_obj[0].object;
		usbpd_manager_match_sink_cap(pd_data);
	}

	/* goto PE_SRC_Ready when tSenderResponse Timeout */
	ret = PE_SRC_Ready;

	return ret;
}

policy_state usbpd_policy_src_wait_new_capabilities(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Wait for new Source Capabilities
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return PE_SRC_Send_Capabilities;
}

policy_state usbpd_policy_src_send_soft_reset(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Send_Soft_Reset;
	int data_role = 0;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Reset Protocol Layer
	Send Soft Reset message
	Initialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform for AP */

	/* USB PD Protocol Initialization */
	usbpd_init_protocol(pd_data);

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Message ID Clear */
	usbpd_init_counters(pd_data);
	pd_data->counter.hard_reset_counter = 0;

	/* Self SoftReset */
	PDIC_OPS_FUNC(soft_reset, pd_data);

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Soft_Reset, data_role, USBPD_SOURCE))
		ret = PE_SRC_Hard_Reset;
	else {
		/* Start Timer */
		usbpd_timer1_start(pd_data);
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_SRC_Send_Soft_Reset;
				break;
			}

			CHECK_MSG(pd_data, MSG_ACCEPT, PE_SRC_Send_Capabilities);

			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
				ret = PE_SRC_Hard_Reset;
				break;
			}
		}
	}


	return ret;
}

policy_state usbpd_policy_src_soft_reset(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Soft_Reset;
	int data_role = 0;
	/**********************************************
	Actions on entry:
	Reset Protocol Layer
	Send Accept message
	**********************************************/
	mutex_lock(&pd_data->softreset_mutex);

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s, %s\n", __func__,
			pd_data->specification_revision == USBPD_PD3_0 ? "PD3.0":"PD2.0");

	/* USB PD Counters Initialization */
	usbpd_init_counters(pd_data);
	pd_data->counter.hard_reset_counter = 0;
	policy->rx_softreset = 0;

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Interrupt Status Bit Clear  */

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Accept, data_role, USBPD_SOURCE))
			ret = PE_SRC_Hard_Reset;
	else
		ret = PE_SRC_Send_Capabilities;

	mutex_unlock(&pd_data->softreset_mutex);

	return ret;
}

policy_state usbpd_policy_snk_startup(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Reset Protocol Layer
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(set_rp_control, pd_data, PLUG_CTRL_RP80);

	/* PD Protocol Initialization */
	usbpd_init_protocol(pd_data);

	return PE_SNK_Discovery;
}

policy_state usbpd_policy_snk_discovery(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int vbus_check = 0;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Wait for VBUS
	**********************************************/

	/* TODO: wait vbus */
	/* if coming from HardReset
	   && NoResponseTimer timeout
	   && HardResetCounter <= nHardResetCount,
	   return(PE_SNK_Hard_Reset) */

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Start Timer */
	usbpd_timer1_start(pd_data);

	/* Wait Message or State */
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SNK_Discovery;
			break;
		}
		ms = usbpd_check_time1(pd_data);

		vbus_check = PDIC_OPS_FUNC(vbus_on_check, pd_data);
		if (vbus_check < 0 || vbus_check > 0) {
			/* prevent hardreset by no request after src_to_snk pr_swap */
			if (!policy->pd_support)
				PDIC_OPS_FUNC(pd_vbus_short_check, pd_data);
			ret = PE_SNK_Wait_for_Capabilities;
			break;
		}

		/* TimeOver Check */
		if (ms >= tNoResponse) {
			/* HardReset Count Check */
			if (pd_data->counter.hard_reset_counter <= USBPD_nHardResetCount) {
				ret = PE_SNK_Hard_Reset;
				break;
			} else {
				ret = PE_SNK_Discovery;
				break;
			}
		}
	}

	return ret;
}

policy_state usbpd_policy_snk_wait_for_capabilities(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Wait_for_Capabilities;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Initialize and run SinkWaitCapTimer
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Configuration Channel On */
	PDIC_OPS_PARAM_FUNC(set_pd_control, pd_data, USBPD_CC_ON);

	/* Start Timer */
	usbpd_timer1_start(pd_data);

	/* Wait Message or State */
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SNK_Wait_for_Capabilities;
			break;
		}
		/* Rx Source Capabilities */
		CHECK_MSG(pd_data, MSG_SRC_CAP, PE_SNK_Evaluate_Capability);

		/* TimeOver Check */
		ms = usbpd_check_time1(pd_data);
		if (ms >= tTypeCSinkWaitCap) {
#if !IS_ENABLED(CONFIG_SEC_FACTORY)
			/* HardReset Count Check */
			if (pd_data->counter.hard_reset_counter <= USBPD_nHardResetCount) {
				ret = PE_SNK_Hard_Reset;
				break;
			} else {
				ret =  PE_SNK_Wait_for_Capabilities;
				break;
			}
#else
			ret = PE_SNK_Wait_for_Capabilities;
			break;
#endif
		}
	}

	return ret;
}

policy_state usbpd_policy_snk_evaluate_capability(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int sink_request_obj_num = 0;
	int ret = PE_SNK_Evaluate_Capability;

	/**********************************************
	Actions on entry:
	Reset HardResetCounter to zero.
	Ask Device Policy Manager to evaluate the options based on supplied
	capabilities, any Power Reserve that it needs, and respond indicating
	the selected capability and, optionally, a Capability Mismatch
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

#if IS_ENABLED(CONFIG_PDIC_PD30)
	/* Check Specification Revision */
	if(pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0)
		PDIC_OPS_PARAM_FUNC(set_revision, pd_data, USBPD_PD3_0);
	else
		PDIC_OPS_PARAM_FUNC(set_revision, pd_data, USBPD_PD2_0);
#endif

	/* Select PDO */
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG) && IS_ENABLED(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	if (pd_data->pd_noti.sink_status.selected_pdo_num == 0) {
		union power_supply_propval val;

		val.intval = 1;
		psy_do_property("battery", set, POWER_SUPPLY_EXT_PROP_SRCCAP, val);

		pd_data->pd_noti.sink_status.selected_pdo_num = 1;
		if (policy->sink_cap_received) {
			policy->send_sink_cap = 1;
			policy->sink_cap_received = 0;
		}
	}
#endif

	/* Select Object Position */
	sink_request_obj_num = usbpd_manager_evaluate_capability(pd_data);
	usbpd_info("%s, check_pps_clk (%d)\n", __func__, policy->need_check_pps_clk);
	if (policy->need_check_pps_clk)
		PDIC_OPS_PARAM_FUNC(ops_check_pps_irq, pd_data, 1);

	/* Branch */
	if (sink_request_obj_num > 0)
		ret = PE_SNK_Select_Capability;
	else
		ret = PE_SNK_Hard_Reset;

	return ret;
}

policy_state usbpd_policy_snk_select_capability(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Select_Capability;
	int data_role = 0;
	long long ms = 0;
	int retry = 0;
	int sent = 0;
	data_obj_type *obj;
#if IS_ENABLED(CONFIG_S2M_PDIC_MANUAL_RETRY)
	int manual_retry = 0;
	int msg_err = 0;
#endif

	/**********************************************
	Actions on entry:
	Send Request based on Device Policy Manager response:
	- Request from present capabilities
	- Optionally Indicate that other capabilities would be preferred (Capability Mismatch)
	Initialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->pd_support) {
		//usleep_range(5000,5100);
		if (PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_SRC_CAP)) {
			ret = PE_SNK_Evaluate_Capability;
			return ret;
		}
	}

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Message Header Setting */
	policy->tx_msg_header.msg_type = USBPD_Request;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = USBPD_SINK;
	policy->tx_msg_header.num_data_objs = 1; /* Initial Select PDO = 1 */

	/* Select PDO */
	policy->tx_data_obj[0] = usbpd_manager_select_capability(pd_data);

	/* Check Specification Revision */
#if 0
	if (policy->got_pps_apdo)
		pd_data->specification_revision = USBPD_PD3_0;
	else
		pd_data->specification_revision = USBPD_PD2_0;
#endif

	/* FPDO <-> VPDO, prevent cc detach W/A*/
//	PDIC_OPS_PARAM_FUNC(pd_instead_of_vbus, pd_data, 0);

	/* Charger W/A */
	obj = &policy->tx_data_obj[0];
	if (usbpd_manager_get_selected_voltage(pd_data, obj->request_data_object.object_position) >= 6900)
		PDIC_OPS_PARAM_FUNC(set_chg_lv_mode, pd_data, 9);
	else
		PDIC_OPS_PARAM_FUNC(set_chg_lv_mode, pd_data, 5);

	/* Clear Interrupt Status */
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_ACCEPT);
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_PSRDY);

	PDIC_OPS_FUNC(ops_check_pps_irq_tx_req, pd_data);


#if IS_ENABLED(CONFIG_S2M_PDIC_MANUAL_RETRY)
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_GOODCRC);
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_ERROR);
	PDIC_OPS_PARAM_FUNC(ops_manual_retry, pd_data, 1);
#endif
	/* Send Message*/
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
#if !IS_ENABLED(CONFIG_S2M_PDIC_MANUAL_RETRY)
		SOFT_RESET();
#endif
	} else
		sent = 1;

	/* Start Timer*/
	usbpd_timer1_start(pd_data);

	/* Wait Message or State */
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SNK_Select_Capability;
			break;
		}
#if IS_ENABLED(CONFIG_S2M_PDIC_MANUAL_RETRY)
		if (pd_data->phy_ops.get_status(pd_data, MSG_GOODCRC)) {
			sent = 1;
			PDIC_OPS_PARAM_FUNC(ops_manual_retry, pd_data, 0);
		}

		if (pd_data->phy_ops.get_status(pd_data, MSG_ERROR))
			msg_err = 1;

		if (msg_err) {
			if (pd_data->phy_ops.get_status(pd_data, MSG_ALERT)) {
				usbpd_info("%s, Collision Occur\n", __func__);
				usbpd_manager_inform_event(pd_data, MANAGER_NEW_POWER_SRC);
				PDIC_OPS_PARAM_FUNC(ops_manual_retry, pd_data, 0);
				return PE_SNK_Source_Alert_Received;
			} else if (ms >= 20) {
				if (manual_retry > 2)
					break;
				usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
				usbpd_timer1_start(pd_data);
				manual_retry++;
				msg_err = 0;
			}
		}
#endif

		CHECK_MSG_BREAK(MSG_GET_SNK_CAP, PE_SNK_Send_Soft_Reset);
		CHECK_MSG_BREAK(MSG_ACCEPT, PE_SNK_Transition_Sink);
		CHECK_MSG_BREAK(MSG_REJECT, (pd_data->pd_noti.sink_status.selected_pdo_num == 0) ?
				PE_SNK_Wait_for_Capabilities : PE_SNK_Ready);
		CHECK_MSG_BREAK(MSG_WAIT, (pd_data->pd_noti.sink_status.selected_pdo_num == 0) ?
				PE_SNK_Wait_for_Capabilities : PE_SNK_Ready);

 		/* TimeOver Check */
		ms = usbpd_check_time1(pd_data);
 		if (ms >= tSenderResponse) {
			if (sent) {
				ret = PE_SNK_Hard_Reset;
				break;
			} else {
				if (retry < 5) {
					dev_info(pd_data->dev, "%s, send request againg(%d)\n", __func__, retry);
					/* Send Message*/
					sent = usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
					/* Start Timer*/
					usbpd_timer1_start(pd_data);

					retry++;
					continue;
				} else {
					ret = PE_SNK_Soft_Reset;
					break;
				}
			}
 		}
 	}
#if IS_ENABLED(CONFIG_S2M_PDIC_MANUAL_RETRY)
	PDIC_OPS_PARAM_FUNC(ops_manual_retry, pd_data, 0);
#endif

	return ret;
}

policy_state usbpd_policy_snk_transition_sink(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret  = PE_SNK_Transition_Sink;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Initialize and run PSTransitionTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Policy Engine State Setting */
	policy->state = PE_SNK_Transition_Sink;

	/* Start Timer */
	usbpd_timer1_start(pd_data);

	/* Wait Message or State */
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SNK_Transition_Sink;
			break;
		}

		CHECK_MSG_BREAK(MSG_PSRDY, PE_SNK_Ready);
		/* PD Certification(Ellisys) : TD.PD.SNK.E10 GetSinkCap in place of PS_RDY */
		CHECK_MSG_BREAK(MSG_GET_SNK_CAP, PE_SNK_Hard_Reset);

		/* TimeOver Check */
		ms = usbpd_check_time1(pd_data);
		if (ms >= tPSTransition) {
			ret = PE_SNK_Hard_Reset;
			break;
		}
	}

	if (ret == PE_SNK_Ready)
		usbpd_manager_short_check(pd_data);

	return ret;
}

policy_state usbpd_policy_snk_ready(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Initialize and run SinkActivityTimer2
	Initialize and run SinkRequestTimer3 (on receiving Wait)
	Initialize and run DiscoverIdentityTimer5
	**********************************************/

	/* Data Role Check */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* PD State Inform to AP */
	if(pd_data->protocol_rx.msg_header.msg_type != USBPD_Vendor_Defined){
		dev_info(pd_data->dev, "%s, %s, %s\n", __func__, data_role ? "DFP" : "UFP",
				((pd_data->specification_revision == USBPD_PD3_0)?"PD3.0":"PD2.0"));
	}

#if IS_ENABLED (CONFIG_TYPEC)
	if (policy->pd_support == 0) {
		policy->pd_support = 1;
		PDIC_OPS_PARAM_FUNC(set_pwr_opmode, pd_data, TYPEC_PWR_MODE_PD);
	}
#endif

	/* Check PD instead of vbus */
	PDIC_OPS_PARAM_FUNC(pd_instead_of_vbus, pd_data, 1);

	/* Start Timer */
	usbpd_timer1_start(pd_data);

	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_REJECT);
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_ACCEPT);
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_PSRDY);

	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset)
			return 0;

		if (policy->plug_valid == 0)
			return PE_SNK_Ready;

		if (policy->rx_hardreset) {
			policy->rx_hardreset = 0;
			return PE_SNK_Transition_to_default;
		}


		ms = usbpd_check_time1(pd_data);
#if IS_ENABLED(CONFIG_PDIC_PD30)
		/* Specification Revision */
		if(pd_data->specification_revision == USBPD_PD3_0){
			/* PD 3.0 : Control Message */
			CHECK_MSG(pd_data, MSG_GET_SOURCE_CAP_EXTENDED, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_STATUS, PE_Give_Status);
			CHECK_MSG(pd_data, MSG_FR_SWAP, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_PPS_STATUS, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_COUNTRY_CODES, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_SINK_CAP_EXTENDED, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_REVISION, PE_Give_Revision);
			CHECK_MSG(pd_data, MSG_GET_SOURCE_INFO, PE_DR_SNK_Give_Source_Info);

			/* PD 3.0 : Data Message */
			CHECK_MSG(pd_data, MSG_BATTERY_STATUS, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_ALERT, PE_SNK_Source_Alert_Received);
			CHECK_MSG(pd_data, MSG_GET_COUNTRY_INFO, PE_SNK_Send_Not_Supported);

			/* PD 3.0 : Extended Message */
			CHECK_MSG(pd_data, MSG_SOURCE_CAPABILITIES_EXTENDED, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_STATUS, PE_SNK_Get_Source_Status);
			CHECK_MSG(pd_data, MSG_GET_BATTERY_CAP, PE_Give_Battery_Cap);
			CHECK_MSG(pd_data, MSG_GET_BATTERY_STATUS, PE_Give_Battery_Status);
			CHECK_MSG(pd_data, MSG_BATTERY_CAPABILITIES, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_GET_MANUFACTURER_INFO, PE_Give_Manufacturer_Info);
			CHECK_MSG(pd_data, MSG_MANUFACTURER_INFO, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_SECURITY_REQUEST, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_SECURITY_RESPONSE, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_FIRMWARE_UPDATE_REQUEST, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_FIRMWARE_UPDATE_RESPONSE, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_PPS_STATUS, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_COUNTRY_INFO, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_COUNTRY_CODES, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_SINK_CAPABILITIES_EXTENDED, PE_SNK_Send_Not_Supported);

			/* When PD3.0 state, NOT_SUPPORT message is transmitted instead of REJECT. */
			CHECK_MSG(pd_data, MSG_VCONN_SWAP, PE_SNK_Send_Not_Supported);

			/* Unrecognized Message Received in Ready State */
			CHECK_MSG(pd_data, MSG_RESERVED, PE_SNK_Send_Not_Supported);
			CHECK_MSG(pd_data, MSG_UVDM_MSG_NOT_SAMSUNG, PE_SNK_Send_Not_Supported);
		}
#endif
		if (pd_data->specification_revision == USBPD_PD2_0) {
			CHECK_MSG(pd_data, MSG_RESERVED, PE_DR_Send_Reject);
			CHECK_MSG(pd_data, MSG_UVDM_MSG_NOT_SAMSUNG, PE_SNK_Ready);
		}

		/* Message Check */
		CHECK_MSG(pd_data, MSG_SRC_CAP, PE_SNK_Evaluate_Capability);
		CHECK_MSG(pd_data, MSG_GET_SNK_CAP, PE_SNK_Give_Sink_Cap);
		CHECK_MSG(pd_data, MSG_PR_SWAP, PE_PRS_SNK_SRC_Evaluate_Swap);
		CHECK_MSG(pd_data, MSG_DR_SWAP, PE_DRS_Evaluate_Port);
		CHECK_MSG(pd_data, MSG_VCONN_SWAP, PE_VCS_Evaluate_Swap);
		CHECK_MSG(pd_data, MSG_GET_SRC_CAP, PE_DR_SNK_Give_Source_Cap);
		CHECK_MSG(pd_data, MSG_BIST, PE_BIST_Carrier_Mode);
		CHECK_MSG(pd_data, MSG_ACCEPT, PE_SNK_Send_Soft_Reset);

		/* Wait VDM */
		if (data_role == USBPD_UFP) {
			/*
			 * got_ufp_vdm variable is always defined, if config is not defined, variable is not used
			 */
			CHECK_VDM(pd_data, VDM_DISCOVER_IDENTITY, PE_UFP_VDM_Get_Identity);
			CHECK_VDM(pd_data, VDM_DISCOVER_SVID, PE_UFP_VDM_Get_SVIDs);
			CHECK_VDM(pd_data, VDM_DISCOVER_MODE, PE_UFP_VDM_Get_Modes);
			CHECK_VDM(pd_data, VDM_ENTER_MODE, PE_UFP_VDM_Evaluate_Mode_Entry);
			CHECK_VDM(pd_data, VDM_EXIT_MODE, PE_UFP_VDM_Mode_Exit);
			CHECK_VDM(pd_data, VDM_ATTENTION, PE_DFP_VDM_Attention_Request);
			CHECK_VDM(pd_data, VDM_DP_STATUS_UPDATE, PE_UFP_VDM_Evaluate_Status);
			CHECK_VDM(pd_data, VDM_DP_CONFIGURE, PE_UFP_VDM_Evaluate_Configure);
			CHECK_VDM(pd_data, UVDM_MSG, PE_DFP_UVDM_Receive_Message);
		} else {
			CHECK_MSG(pd_data, VDM_DISCOVER_IDENTITY, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_DISCOVER_SVID, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_DISCOVER_MODE, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_ENTER_MODE, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_EXIT_MODE, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_ATTENTION, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_DP_STATUS_UPDATE, PE_DFP_VDM_EVALUATE);
			CHECK_MSG(pd_data, VDM_DP_CONFIGURE, PE_DFP_VDM_EVALUATE);
		}

		/* Command Check from AP */
		if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
			if (pd_data->manager.cmd) {
				int rp_level = 0;

				PDIC_OPS_PARAM_FUNC(get_rp_level, pd_data, &rp_level);
				if (rp_level == RP_CURRENT_LEVEL3) {
					CHECK_CMD(pd_data, MANAGER_REQ_NEW_POWER_SRC, PE_SNK_Select_Capability, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_GET_SRC_CAP, PE_SNK_Get_Source_Cap, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_PR_SWAP, PE_PRS_SNK_SRC_Send_Swap, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_DR_SWAP, PE_DRS_Evaluate_Send_Port, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_VCONN_SWAP, PE_VCS_Send_Swap, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_IDENTITY, PE_DFP_VDM_Identity_Request, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_SVID, PE_DFP_VDM_SVIDs_Request, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_MODE, PE_DFP_VDM_Modes_Request, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_VDM_ATTENTION, PE_UFP_VDM_Attention_Request, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_VDM_ENTER_MODE, PE_DFP_VDM_Mode_Entry_Request, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_VDM_STATUS_UPDATE, PE_DFP_VDM_Status_Update, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_VDM_DisplayPort_Configure, PE_DFP_VDM_DisplayPort_Configure, 0);
					CHECK_CMD(pd_data, MANAGER_REQ_UVDM_SEND_MESSAGE, PE_DFP_UVDM_Send_Message, 0);
				} else
					usbpd_info("%s, SinkTxNG, cmd(%d) is delayed\n",
						__func__, pd_data->manager.cmd);
			}
		} else {
#if IS_ENABLED(CONFIG_S2M_SUPPORT_DELAYED_REQUEST_MESSAGE_UFP)
			if ((policy->got_ufp_vdm && ms >= tStartAmsMargin) || (!policy->got_ufp_vdm)) {
				policy->got_ufp_vdm = 0;
#endif
				CHECK_CMD(pd_data, MANAGER_REQ_NEW_POWER_SRC, PE_SNK_Select_Capability, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_GET_SRC_CAP, PE_SNK_Get_Source_Cap, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_PR_SWAP, PE_PRS_SNK_SRC_Send_Swap, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_DR_SWAP, PE_DRS_Evaluate_Send_Port, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VCONN_SWAP, PE_VCS_Send_Swap, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_IDENTITY, PE_DFP_VDM_Identity_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_SVID, PE_DFP_VDM_SVIDs_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_DISCOVER_MODE, PE_DFP_VDM_Modes_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_ATTENTION, PE_UFP_VDM_Attention_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_ENTER_MODE, PE_DFP_VDM_Mode_Entry_Request, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_STATUS_UPDATE, PE_DFP_VDM_Status_Update, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_VDM_DisplayPort_Configure, PE_DFP_VDM_DisplayPort_Configure, 5);
				CHECK_CMD(pd_data, MANAGER_REQ_UVDM_SEND_MESSAGE, PE_DFP_UVDM_Send_Message, 5);

#if IS_ENABLED(CONFIG_S2M_SUPPORT_DELAYED_REQUEST_MESSAGE_UFP)
			}
#endif
		}

#if IS_ENABLED(CONFIG_S2M_SUPPORT_DELAYED_REQUEST_MESSAGE_UFP)
		if (ms >= (policy->got_ufp_vdm ? tStartAmsMargin : 0) + 20)
			break;
#else
		if (ms >= 20)
			break;
#endif
	}

	if (data_role == USBPD_DFP)
		usbpd_manager_vdm_request_enabled(pd_data);

	/* Ellisys PPS Test, Make it look like PPS*/
	if (pd_data->pps_pd == 1)
		usbpd_manager_pps_request(pd_data);

	return PE_SNK_Ready;
}

policy_state usbpd_policy_snk_hard_reset(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Generate Hard Reset signalling.
	Increment HardResetCounter.
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	usbpd_info("%s, time out sec = %lld, nsec = %ld\n",
		__func__, pd_data->check_time.tv_sec, pd_data->check_time.tv_nsec);
	PDIC_OPS_FUNC(hard_reset, pd_data);
	PDIC_OPS_PARAM_FUNC(set_pd_control, pd_data, USBPD_CC_OFF);
	/* increase hard reset counter */
	pd_data->counter.hard_reset_counter++;

	return PE_SNK_Transition_to_default;
}

policy_state usbpd_policy_snk_transition_to_default(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
#if IS_ENABLED(CONFIG_PDIC_PD30)
	struct usbpd_manager_data *manager = &pd_data->manager;
#endif
	int ret = PE_SNK_Startup;
	long long ms = 0;
	int data_role = 0;

	/**********************************************
	Hard reset signalling received

	Actions on entry:
	Request Device Policy Manager to request power sink transition to default
	Reset local HW
	If Type-C set Port Data Role to UFP and turn off VCONN
	 **********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s, pps(%d)\n", __func__, policy->need_check_pps_clk);

	if (policy->need_check_pps_clk) {
		usbpd_info("%s,  pps + hardReset need to reduce CLK!!\n", __func__);
		PDIC_OPS_PARAM_FUNC(ops_check_pps_irq_reduce_clk, pd_data, true);
	} else {
		usbpd_info("%s,  !pps + hardReset restore CLK!!\n", __func__);
		PDIC_OPS_PARAM_FUNC(ops_check_pps_irq_reduce_clk, pd_data, false);
	}

#if IS_ENABLED(CONFIG_PDIC_PD30)
	PDIC_OPS_FUNC(send_hard_reset_dc, pd_data);
#endif
	/* Message ID Clear */
	usbpd_init_counters(pd_data);

	/* Policy Initialization */
	usbpd_init_policy(pd_data);

	/* Driver Reset */
	PDIC_OPS_FUNC(driver_reset, pd_data);
	usbpd_init_manager_val(pd_data);
	usbpd_init_policy(pd_data);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	if (data_role != USBPD_UFP)
		PDIC_OPS_PARAM_FUNC(set_data_role, pd_data, USBPD_UFP);

#if IS_ENABLED(CONFIG_PDIC_PD30)
	mutex_lock(&manager->pdo_mutex);
	PDIC_OPS_PARAM_FUNC(pps_enable, pd_data, PPS_DISABLE);
	mutex_unlock(&manager->pdo_mutex);
#endif

	/* Vconn Off */
	usbpd_manager_turn_off_vconn(pd_data);

	/* Wait 200ms */
	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_SNK_Transition_to_default;
			break;
		}
		ms = usbpd_check_time1(pd_data);
		if (ms >= 300)
			break;
	}

	return ret;
}

policy_state usbpd_policy_snk_give_sink_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Give_Sink_Cap;
	int data_role = 0;
	int power_role = 0;
	/**********************************************
	Get Sink Cap Message received

	Actions on entry:
	Get present sink capabilities from Device Policy Manager
	Send Capabilities message (based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Sink Cap Message Setting */
	policy->tx_msg_header.word = pd_data->sink_msg_header.word;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_data_obj[0].object = pd_data->sink_data_obj[0].object;
	policy->tx_data_obj[1].object = pd_data->sink_data_obj[1].object;

	/* Clear Interrupt Status */

	/* Send Message */
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_snk_get_source_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Get_Source_Cap;
	int power_role = 0, data_role = 0;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Send Get_Source_Cap message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Send Message*/
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Get_Source_Cap, data_role, USBPD_SINK)) {
		SOFT_RESET();
	} else {
		ret = PE_SNK_Ready;
		/* Wait Message or State */
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_SNK_Get_Source_Cap;
				break;
			}

			CHECK_MSG_BREAK(MSG_SRC_CAP, PE_SNK_Evaluate_Capability);

			/* TimeOver Check */
			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
				ret = PE_SNK_Ready;
				break;
			}
		}

	}

	return ret;
}

policy_state usbpd_policy_snk_send_soft_reset(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Send_Soft_Reset;
	int data_role = 0;
	long long ms = 0;
	/**********************************************
	Actions on entry:
	Reset Protocol Layer
	Send Soft Reset message
	Initialize and run SenderResponseTimer
	 **********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s, pps(%d)\n", __func__, policy->need_check_pps_clk);

	if (policy->need_check_pps_clk) {
		usbpd_info("%s,  pps + hardReset need to reduce CLK!!\n", __func__);
		PDIC_OPS_PARAM_FUNC(ops_check_pps_irq_reduce_clk, pd_data, true);
	} else {
		usbpd_info("%s,  !pps + hardReset restore CLK!!\n", __func__);
		PDIC_OPS_PARAM_FUNC(ops_check_pps_irq_reduce_clk, pd_data, false);
	}

	/* USB PD Protocol Initialization */
	usbpd_init_protocol(pd_data);

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Message ID Clear */
	usbpd_init_counters(pd_data);
	pd_data->counter.hard_reset_counter = 0;

	/* Self SoftReset */
	PDIC_OPS_FUNC(soft_reset, pd_data);

	/* Clear Status */

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Soft_Reset, data_role, USBPD_SINK))
		ret = PE_SNK_Hard_Reset;
	else {
		/* Start Timer */
		usbpd_timer1_start(pd_data);

		/* Wait Message or State */
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_SNK_Send_Soft_Reset;
				break;
			}

			CHECK_MSG(pd_data, MSG_ACCEPT, PE_SNK_Wait_for_Capabilities);

			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
				ret = PE_SNK_Hard_Reset;
				break;
			}
		}
	}

	return ret;
}

policy_state usbpd_policy_snk_soft_reset(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int ret = PE_SNK_Soft_Reset;

	/**********************************************
	Soft Reset message received

	Actions on entry:
	Reset Protocol Layer
	Send Accept message
	 **********************************************/

	mutex_lock(&pd_data->softreset_mutex);
	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s, %s, pps(%d)\n", __func__,
			pd_data->specification_revision == USBPD_PD3_0 ? "PD3.0":"PD2.0",
			policy->need_check_pps_clk);

	if (policy->need_check_pps_clk) {
		usbpd_info("%s,  pps + SoftReset need to reduce CLK!!\n", __func__);
		PDIC_OPS_PARAM_FUNC(ops_check_pps_irq_reduce_clk, pd_data, true);
	} else {
		usbpd_info("%s,  !pps + SoftReset restore CLK!!\n", __func__);
		PDIC_OPS_PARAM_FUNC(ops_check_pps_irq_reduce_clk, pd_data, false);
	}

	/* USB PD Counters Initialization */
	usbpd_init_counters(pd_data);
	pd_data->counter.hard_reset_counter = 0;
	policy->rx_softreset = 0;

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Interrupt Status Bit Clear  */

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Accept, data_role, USBPD_SINK))
			ret = PE_SNK_Hard_Reset;
	else
		ret = PE_SNK_Wait_for_Capabilities;

	mutex_unlock(&pd_data->softreset_mutex);

	return ret;
}

policy_state usbpd_policy_drs_evaluate_port(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->modal_operation) {
		PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

		ret = (power_role == USBPD_SOURCE) ? PE_SRC_Hard_Reset : PE_SNK_Hard_Reset;
	} else {
		PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

		ret = (data_role == USBPD_DFP) ?
			PE_DRS_DFP_UFP_Evaluate_DR_Swap : PE_DRS_UFP_DFP_Evaluate_DR_Swap;
	}

	return ret;
}

policy_state usbpd_policy_drs_evaluate_send_port(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->modal_operation) {
		PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

		ret = (power_role == USBPD_SOURCE) ? PE_SRC_Hard_Reset : PE_SNK_Hard_Reset;
	} else {
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
		ret = (data_role == USBPD_DFP) ?
			PE_DRS_DFP_UFP_Send_DR_Swap : PE_DRS_UFP_DFP_Send_DR_Swap;
	}

	return ret;
}

policy_state usbpd_policy_drs_dfp_ufp_evaluate_dr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	bool drs_ok;
	int ret = 0;

	/**********************************************
	DR_Swap message received & not in Modal Operation

	Actions on entry:
	Get evaluation of Data Role Swap
	request from Device Policy Manager
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	drs_ok = usbpd_manager_data_role_swap(pd_data);

	ret = drs_ok ? PE_DRS_DFP_UFP_Accept_DR_Swap : PE_DRS_DFP_UFP_Reject_DR_Swap;

	return ret;
}

policy_state usbpd_policy_drs_dfp_ufp_accept_dr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Accept message
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	usleep_range(3000, 3010);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	mutex_lock(&pd_data->accept_mutex);
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Accept, USBPD_DFP, power_role)) {
		SOFT_RESET();
	} else
		ret = PE_DRS_DFP_UFP_Change_to_UFP;

	PDIC_OPS_PARAM_FUNC(set_data_role, pd_data, USBPD_UFP);
	mutex_unlock(&pd_data->accept_mutex);

	return ret;
}

policy_state usbpd_policy_drs_dfp_ufp_change_to_ufp(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Request Device Policy Manager to
	change port to UFP
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_drs_dfp_ufp_send_dr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;
	long long ms = 0;
	/**********************************************
	Actions on entry:
	Send Swap DR message
	Initialize and run SenderResponseTimer
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_DR_Swap, USBPD_DFP, power_role)) {
		SOFT_RESET();
	} else {
		usbpd_timer1_start(pd_data);
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_DRS_DFP_UFP_Send_DR_Swap;
				break;
			}

			CHECK_MSG_BREAK(MSG_ACCEPT, PE_DRS_DFP_UFP_Change_to_UFP);
			CHECK_MSG_BREAK(MSG_REJECT, (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready));
			CHECK_MSG_BREAK(MSG_WAIT, (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready));

			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse)
				break;
		}
	}

	if (ret == PE_DRS_DFP_UFP_Change_to_UFP)
		PDIC_OPS_PARAM_FUNC(set_data_role, pd_data, USBPD_UFP);

	return ret;
}

policy_state usbpd_policy_drs_dfp_ufp_reject_dr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Reject or Wait message as appropriate
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Reject, USBPD_DFP, power_role)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_drs_ufp_dfp_evaluate_dr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	bool drs_ok;

	/**********************************************
	Actions on entry:
	Get evaluation of Data Role Swap
	request from Device Policy Manager
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	drs_ok = usbpd_manager_data_role_swap(pd_data);

	return drs_ok ? PE_DRS_UFP_DFP_Accept_DR_Swap : PE_DRS_UFP_DFP_Reject_DR_Swap;
}

policy_state usbpd_policy_drs_ufp_dfp_accept_dr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Accept message
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	mutex_lock(&pd_data->accept_mutex);
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Accept, USBPD_UFP, power_role)) {
		SOFT_RESET();
	} else {
		usbpd_manager_restart_discover_msg(pd_data);
		PDIC_OPS_PARAM_FUNC(set_data_role, pd_data, USBPD_DFP);
	}
	mutex_unlock(&pd_data->accept_mutex);

	return PE_DRS_UFP_DFP_Change_to_DFP;
}

policy_state usbpd_policy_drs_ufp_dfp_change_to_dfp(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Request Device Policy Manager to change port to DFP
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_drs_ufp_dfp_send_dr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Send Swap DR message
	Initialize and run SenderResponseTimer
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_DR_Swap, USBPD_UFP, power_role)) {
		SOFT_RESET();
	} else {
		usbpd_timer1_start(pd_data);
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_DRS_UFP_DFP_Send_DR_Swap;
				break;
			}
			CHECK_MSG_BREAK(MSG_ACCEPT, PE_DRS_UFP_DFP_Change_to_DFP);
			CHECK_MSG_BREAK(MSG_REJECT, (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready));
			CHECK_MSG_BREAK(MSG_WAIT, (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready));

			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse)
				break;
		}
	}

	if (ret == PE_DRS_UFP_DFP_Change_to_DFP) {
		usbpd_manager_restart_discover_msg(pd_data);
		PDIC_OPS_PARAM_FUNC(set_data_role, pd_data, USBPD_DFP);
	}
	return ret;
}

policy_state usbpd_policy_drs_ufp_dfp_reject_dr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int data_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Reject or Wait message as appropriate
	**********************************************/

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Reject, data_role, USBPD_SINK)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_prs_src_snk_reject_pr_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = 0;
	/**********************************************
	Actions on entry:
	Send Reject or Wait message as appropriate
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);


	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Reject, data_role, USBPD_SOURCE)) {
		SOFT_RESET();
	} else
		ret = PE_SRC_Ready;

	return ret;
}

policy_state usbpd_policy_prs_src_snk_evaluate_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	bool prs_ok;

	/**********************************************
	Actions on entry:
	Get evaluation of swap request from Device Policy Manager
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* DPM Check to support roleswap */
	prs_ok = usbpd_manager_power_role_swap(pd_data);

	/* Branch */
	return prs_ok ? PE_PRS_SRC_SNK_Accept_Swap : PE_PRS_SRC_SNK_Reject_PR_Swap;
}

policy_state usbpd_policy_prs_src_snk_send_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = PE_SRC_Ready;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Send PR_Swap message
	Initialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_PR_Swap, data_role, USBPD_SOURCE)) {
		SOFT_RESET();
	} else {
		/* Start Timer */
		usbpd_timer1_start(pd_data);

		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_PRS_SRC_SNK_Send_Swap;
				break;
			}

			CHECK_MSG_BREAK(MSG_ACCEPT, PE_PRS_SRC_SNK_Transition_off);
			CHECK_MSG_BREAK(MSG_REJECT, PE_SRC_Ready);
			CHECK_MSG_BREAK(MSG_WAIT, PE_SRC_Ready);

			/* TimeOver Check */
			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse + 5) {
				ret = PE_SRC_Ready;
				break;
			}
		}
	}

	return ret;
}

policy_state usbpd_policy_prs_src_snk_accept_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int ret = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/**********************************************
	Actions on entry:
	Send Accept message
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Accept, data_role, USBPD_SOURCE)) {
		SOFT_RESET();
	} else
		ret = PE_PRS_SRC_SNK_Transition_off;

	return ret;

}

policy_state usbpd_policy_prs_src_snk_transition_to_off(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	struct usbpd_manager_data *manager = &pd_data->manager;
	int ret = PE_PRS_SRC_SNK_Assert_Rd;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Tell Device Policy Manager to turn off power supply
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Delay */
	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_PRS_SRC_SNK_Transition_off;
			break;
		}
		ms = usbpd_check_time1(pd_data);
		if (ms >= tSrcTransition)
			break;
	}

	if (ret == PE_PRS_SRC_SNK_Transition_off)
		return ret;

	PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_SOURCE);

	PDIC_OPS_PARAM_FUNC(pr_swap, pd_data, USBPD_SOURCE_OFF);

	manager->req_pdo_type = PDO_TYPE_FIXED;

	/* VBUS off */
	PDIC_OPS_PARAM_FUNC(set_otg_control, pd_data, 0);
	PDIC_OPS_PARAM_FUNC(set_rp_control, pd_data, PLUG_CTRL_RP80);

	usbpd_info("%s, %d\n", __func__, manager->acc_type);

	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_PRS_SRC_SNK_Transition_off;
			break;
		}
		ms = usbpd_check_time1(pd_data);
		if (ms >= 600)
			break;
	}
#if 0
	/* TODO: svid_0 == 0 confition check?
	 * based on 004 code 600ms no condition vs here 150ms w/ condition */
	/* skip delay when GEARVR is attached */
	if (manager->acc_type != PDIC_DOCK_HMT || manager->SVID_0 == 0)
		msleep(600);
#endif

	if (ret == PE_PRS_SRC_SNK_Transition_off)
		PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);

	return ret;
}

policy_state usbpd_policy_prs_src_snk_assert_rd(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Request DPM to assert Rd
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Asserted Rd */
	PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_SINK);

	return PE_PRS_SRC_SNK_Wait_Source_on;
}

policy_state usbpd_policy_prs_src_snk_wait_source_on(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int data_role = 0;
	long long ms = 0;
	/**********************************************
	Actions on entry:
	Send PS_RDY message
	Initialize and run PSSourceOnTimer
	**********************************************/

	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Clear PS_Ready Status */
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_PSRDY);

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_PS_RDY, data_role, USBPD_SINK)) {
		//PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);
		return Error_Recovery;
	}

	usbpd_timer1_start(pd_data);

	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_PRS_SRC_SNK_Wait_Source_on;
			break;
		}
		CHECK_MSG_BREAK(MSG_PSRDY, PE_SNK_Startup);

		ms = usbpd_check_time1(pd_data);
		if (ms >= tPSSourceOn) {
			//ret = Error_Recovery; /* GO to Error_recovery immediately */
			return Error_Recovery;
		}
	}

	if (ret == PE_SNK_Startup) {
		dev_info(pd_data->dev, "got PSRDY.\n");
		pd_data->counter.swap_hard_reset_counter = 0;

		/* Message ID Clear */
		usbpd_init_counters(pd_data);
		pd_data->counter.hard_reset_counter = 0;
		usleep_range(2000, 2100);
		PDIC_OPS_FUNC(soft_reset, pd_data);
	}

	PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);

	return ret;
}

policy_state usbpd_policy_prs_snk_src_reject_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Reject or Wait message as appropriate
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Reject, data_role, USBPD_SINK)) {
		SOFT_RESET();
	} else
		ret = PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_prs_snk_src_evaluate_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	bool prs_ok;
	int ret = PE_PRS_SNK_SRC_Evaluate_Swap;

	/**********************************************
	Actions on entry:
	Get evaluation of swap request from Device Policy Manager
	**********************************************/

	/* PD State Inform to AP */
	//dev_info(pd_data->dev, "%s\n", __func__);

	/* Power Role Swap Check */
	prs_ok = usbpd_manager_power_role_swap(pd_data);

	ret = prs_ok ? PE_PRS_SNK_SRC_Accept_Swap : PE_PRS_SNK_SRC_Reject_Swap;

	return ret;
}

policy_state usbpd_policy_prs_snk_src_send_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int ret = PE_SNK_Ready;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Send PR_Swap message
	Initialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
						USBPD_PR_Swap, data_role, USBPD_SINK);

	usbpd_timer1_start(pd_data);

	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_PRS_SNK_SRC_Send_Swap;
			break;
		}

		CHECK_MSG_BREAK(MSG_ACCEPT, PE_PRS_SNK_SRC_Transition_off);
		CHECK_MSG_BREAK(MSG_REJECT, PE_SNK_Ready);
		CHECK_MSG_BREAK(MSG_WAIT, PE_SNK_Ready);

		/* TimeOver Check */
		ms = usbpd_check_time1(pd_data);
		if (ms >= tSenderResponse) {
			ret = PE_SNK_Ready;
			break;
		}

	}

	if (ret == PE_PRS_SNK_SRC_Transition_off)
		PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_SINK);

	return ret;
}

policy_state usbpd_policy_prs_snk_src_accept_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;

	/**********************************************
	Actions on entry:
	Send Accept message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Clear PS_Ready Status */
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_PSRDY);

	/* Send Accept Message */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Accept, data_role, USBPD_SINK);
	PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_SINK);

	return PE_PRS_SNK_SRC_Transition_off;
}

policy_state usbpd_policy_prs_snk_src_transition_to_off(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	long long ms = 0;
	/**********************************************
	Actions on entry:
	Initialize and run PSSourceOffTimer
	Tell Device Policy Manager to turn off Power Sink.
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	msleep(50);

	PDIC_OPS_PARAM_FUNC(pr_swap, pd_data, USBPD_SINK_OFF);

	/* Start Timer 750ms */
	usbpd_timer1_start(pd_data);

	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			usbpd_info("%s, plug_valid == 0\n", __func__);
			PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);
			ret = PE_PRS_SNK_SRC_Transition_off;
			break;
		}
		CHECK_MSG_BREAK(MSG_PSRDY, PE_PRS_SNK_SRC_Assert_Rp);

		ms = usbpd_check_time1(pd_data);
		if (ms >= tPSSourceOff) {
			ret = Error_Recovery; /* GO to Error_recovery immediately */
			//PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);
			break;
		}
	}

	return ret;
}

policy_state usbpd_policy_prs_snk_src_assert_rp(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Request DPM to assert Rp
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_SOURCE);

	return PE_PRS_SNK_SRC_Source_on;
}

policy_state usbpd_policy_prs_snk_src_source_on(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int ret = 0;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Tell Device Policy Manager to turn on Source
	Initialize and run SourceActivityTimer (see Section 8.3.3.6.1.2)1
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(pr_swap, pd_data, USBPD_SOURCE_ON);

	/* VBUS on */
	PDIC_OPS_PARAM_FUNC(set_otg_control, pd_data, 1);

	/* Dealy */
	usbpd_timer1_start(pd_data);
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_PRS_SNK_SRC_Source_on;
			break;
		}
		ms = usbpd_check_time1(pd_data);
		if (ms >= 200)
			break;
	}

	if (ret == PE_PRS_SNK_SRC_Source_on)
		return ret;

	/* send PS_RDY */
	/* need to check */

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_PS_RDY, data_role, USBPD_SOURCE)) {
		return Error_Recovery; /* GO to Error_recovery immediately */
	} else {
		PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);
		usbpd_timer1_start(pd_data);
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_PRS_SNK_SRC_Source_on;
				break;
			}
			ms = usbpd_check_time1(pd_data);
			if (ms >= tSwapSourceStart)
				break;
		}

		if (ret == PE_PRS_SNK_SRC_Source_on)
			return ret;
		/*  TODO: 4) check GoodCRC : may need to be added for certification */

		/* Message ID Clear */
		usbpd_init_counters(pd_data);
		pd_data->counter.hard_reset_counter = 0;

		return PE_SRC_Startup;
	}

	PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);

	return PE_PRS_SNK_SRC_Source_on;
}

policy_state usbpd_policy_vcs_evaluate_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	bool vcs_ok;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Get evaluation of VCONN swap
	request from Device Policy Manager
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Request from DPM */
	vcs_ok = usbpd_manager_vconn_source_swap(pd_data);

	ret = vcs_ok ? PE_VCS_Accept_Swap : PE_VCS_Reject_VCONN_Swap;

	return ret;
}

policy_state usbpd_policy_vcs_accept_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int vconn_source = 0;
	int power_role = 0;
	int data_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Accept message
	**********************************************/

	PDIC_OPS_PARAM_FUNC(get_vconn_source, pd_data, &vconn_source);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Accept, data_role, power_role)) {
		SOFT_RESET();
	} else
		ret = vconn_source ? PE_VCS_Wait_for_VCONN : PE_VCS_Turn_On_VCONN;

	return ret;
}

policy_state usbpd_policy_vcs_send_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int vconn_source = 0;
	int power_role = 0;
	int ret = PE_VCS_Send_Swap;

	/**********************************************
	Actions on entry:
	Send VCONN_Swap message
	Initialize and run SenderResponseTimer
	**********************************************/


	/* PD State Inform for AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Get Vconn Source */
	PDIC_OPS_PARAM_FUNC(get_vconn_source, pd_data, &vconn_source);

	/* Get Power Role */
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Clear PS_Ready Status */
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_PSRDY);

	/* Send Vconn Swap */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_VCONN_Swap, USBPD_DFP, power_role))
		ret = (power_role == USBPD_SOURCE) ? PE_SRC_Hard_Reset : PE_SNK_Hard_Reset;
	else
		ret = vconn_source ? PE_VCS_Wait_for_VCONN : PE_VCS_Turn_On_VCONN;

	return ret;
}

policy_state usbpd_policy_vcs_wait_for_vconn(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_VCS_Wait_for_VCONN;
	long long ms = 0;
	/**********************************************
	Actions on entry:
	Start VCONNOnTimer
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	/* Start Timer */
	usbpd_timer1_start(pd_data);

	/* Wait Message or State */
	while (1) {
		if (policy->rx_softreset || policy->rx_hardreset) {
			ret = 0;
			break;
		}
		if (policy->plug_valid == 0) {
			ret = PE_VCS_Wait_for_VCONN;
			break;
		}
		CHECK_MSG_BREAK(MSG_PSRDY, PE_VCS_Turn_Off_VCONN);

		/* TimeOver Check */
		ms = usbpd_check_time1(pd_data);
		if (ms >= tVCONNSourceOn) {
			ret = (pd_data->counter.swap_hard_reset_counter > USBPD_nHardResetCount) ?
				Error_Recovery : PE_SNK_Hard_Reset;
			break;
		}
	}

	if (ret == PE_VCS_Turn_Off_VCONN)
		pd_data->counter.swap_hard_reset_counter = 0;

	return ret;
}

policy_state usbpd_policy_vcs_turn_off_vconn(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Tell Device Policy Manager to turn off VCONN
	**********************************************/

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(set_vconn_source, pd_data, USBPD_VCONN_OFF);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_vcs_turn_on_vconn(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Tell Device Policy Manager to turn on VCONN
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(set_vconn_source, pd_data, USBPD_VCONN_ON);

	return PE_VCS_Send_PS_RDY;
}

policy_state usbpd_policy_vcs_send_ps_rdy(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int data_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send PS_RDY message
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	usleep_range(5000,5100);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_PS_RDY, data_role, data_role)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_vcs_reject_vconn_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Reject or Wait message as appropriate
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Reject, USBPD_DFP, power_role)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_get_identity(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_UFP_VDM_Get_Identity;
	int rx_svid = 0;
	int rx_command_type = 0;
	int data_role = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Request Identity information from DPM
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	/* Get Device Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Read SVID */
	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	/* rx_command_type */
	rx_command_type = pd_data->protocol_rx.data_obj[0].structured_vdm.command_type;

	switch (rx_svid) {
	case PD_SID:
	case PD_SID_1:
		if (data_role == USBPD_DFP) {
			switch (rx_command_type) {
			case Initiator:
				ret = PE_UFP_VDM_Get_Identity_NAK;
				break;

			case Responder_NAK:
			case Responder_BUSY:
				ret = PE_DFP_VDM_Identity_NAKed;
				break;
			default:
				break;
			}
		} else
			ret = PE_UFP_VDM_Send_Identity;

		break;
	default:
		ret = PE_UFP_VDM_Get_Identity_NAK;
		break;
	}

	if (ret == PE_UFP_VDM_Get_Identity)
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_send_identity(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int i;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Discover Identity ACK
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 4;

	/* VDM Header */
	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_ACK;
	policy->tx_data_obj[0].structured_vdm.command = Discover_Identity;

	/* ID Header */
	policy->tx_data_obj[1].object = 0;
	policy->tx_data_obj[1].id_header_vdo.Data_Capable_USB_Host = 1;
	policy->tx_data_obj[1].id_header_vdo.Data_Capable_USB_Device = 1;
	policy->tx_data_obj[1].id_header_vdo.Product_Type = UFP_PDUSB_Peripheral;
	if (pd_data->specification_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[1].id_header_vdo.product_type_dfp = DFP_PDUSB_Host;
		policy->tx_data_obj[1].id_header_vdo.Connector_Type = Connector_Receptacle;
		policy->tx_msg_header.num_data_objs = 7;

		for (i = 4; i < 7; i++)
			policy->tx_data_obj[i].object = 0;

		policy->tx_data_obj[4].product_type_vdo_ufp.UFP_VDO_Version = 0b011;
		policy->tx_data_obj[4].product_type_vdo_ufp.Device_Capability = 0b0001;
		policy->tx_data_obj[6].product_type_vdo_dfp.DFP_VDO_Version = 0b010;
		policy->tx_data_obj[6].product_type_vdo_dfp.Host_Capability = 0b001;
	} else {
		policy->tx_data_obj[1].id_header_vdo.product_type_dfp = 0; //PD2.0 Reserved Bit
		policy->tx_data_obj[1].id_header_vdo.Connector_Type = 0;	//PD2.0 Reserved Bit
	}
	policy->tx_data_obj[1].id_header_vdo.USB_Vendor_ID = SAMSUNG_VENDOR_ID;

	/* Cert Stat */
	policy->tx_data_obj[2].object = 0;
	policy->tx_data_obj[2].cert_stat_vdo.xid = 0;

	/* Product VDO */
	policy->tx_data_obj[3].object = 0;
	policy->tx_data_obj[3].product_vdo.USB_Product_ID = 0x6860; /* Samsung Phone */
	policy->tx_data_obj[3].product_vdo.Device_Version = 0x0301; /* BCD Device */

	/* Ellisys SNK.3.Interruption by PD Message, Check GET_SNK_CAP and then return VDM */
	usleep_range(3000, 3100);
	CHECK_MSG_RETURN(pd_data, MSG_GET_SNK_CAP, PE_SNK_Give_Sink_Cap);
	CHECK_MSG_RETURN(pd_data, VDM_DISCOVER_SVID, PE_UFP_VDM_Get_SVIDs); /*Ellisys SNK.4.Interruption by VDM MSG */

	/* TODO: data object should be prepared from device manager */
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_get_identity_nak(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int data_role = 0;
	int rx_svid = 0;
	int ret = 0;

	/**********************************************
	Actions on entry: NAK
	Send Discover Identity NAK/BUSY Command
	response as requested
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_NAK;
	policy->tx_data_obj[0].structured_vdm.command = Discover_Identity;

	/* Ellisys SNK.3.Interruption by PD Message, Check GET_SNK_CAP and then return VDM */
	usleep_range(3000, 3100);
	CHECK_MSG_RETURN(pd_data, MSG_GET_SNK_CAP, PE_SNK_Give_Sink_Cap);
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_get_svids(struct policy_data *policy)
{
	uint16_t rx_svid = 0;

	/**********************************************
	Actions on entry:
	Request SVIDs information from DPM
	**********************************************/

	rx_svid = policy->rx_data_obj[0].structured_vdm.svid;
	usbpd_info("%s, Rx SVID : 0x%x\n", __func__, rx_svid);

	switch (rx_svid) {
	case PD_SID:
	case TypeC_DP_SUPPORT:
	case SAMSUNG_VENDOR_ID:
		if (policy->not_support_svid_ack)
			return PE_UFP_VDM_Get_SVIDs_NAK;
	else
		return PE_UFP_VDM_Send_SVIDs;
	default:
		return PE_UFP_VDM_Get_SVIDs_NAK;
	}
}

policy_state usbpd_policy_ufp_vdm_send_svids(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Discover SVIDs ACK
	**********************************************/
	dev_info(pd_data->dev, "khos %s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 2;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_ACK;
	policy->tx_data_obj[0].structured_vdm.command = Discover_SVIDs;

	policy->tx_data_obj[1].vdm_svid.svid_0 = SAMSUNG_VENDOR_ID;
	policy->tx_data_obj[1].vdm_svid.svid_1 = 0;

	/* TODO: data object should be prepared from device manager */

	/* Ellisys SNK.3.Interruption by PD Message, Check GET_SNK_CAP and then return VDM */
	usleep_range(3000, 3100);
	CHECK_MSG_RETURN(pd_data, MSG_GET_SNK_CAP, PE_SNK_Give_Sink_Cap);
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_get_svids_nak(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int rx_svid = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Discover SVIDs NAK/BUSY Command
	response as requested
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_NAK;
	policy->tx_data_obj[0].structured_vdm.command = Discover_SVIDs;

	/* Ellisys SNK.3.Interruption by PD Message, Check GET_SNK_CAP and then return VDM */
	usleep_range(3000, 3100);
	CHECK_MSG_RETURN(pd_data, MSG_GET_SNK_CAP, PE_SNK_Give_Sink_Cap);
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_get_modes(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Request Modes information from DPM
	**********************************************/

	if (usbpd_manager_get_modes(pd_data) == 0)
		return PE_UFP_VDM_Send_Modes;
	else
		return PE_UFP_VDM_Get_Modes_NAK;
}

policy_state usbpd_policy_ufp_vdm_send_modes(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	struct usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Discover Modes ACK
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 2;

	policy->tx_data_obj[0].object = 0;
	//policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.svid = manager->Standard_Vendor_ID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_ACK;
	policy->tx_data_obj[0].structured_vdm.command = Discover_Modes;

	/* TODO: data object should be prepared from device manager */

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_get_modes_nak(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int rx_svid = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Discover Modes NAK/BUSY Command
	response as requested
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_NAK;
	policy->tx_data_obj[0].structured_vdm.command = Discover_Modes;

	/* TODO: data object should be prepared from device manager */

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_evaluate_mode_entry(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	struct usbpd_manager_data *manager = &pd_data->manager;
	uint16_t rx_svid;
	int ret = PE_UFP_VDM_Evaluate_Mode_Entry;

	/**********************************************
	Actions on entry:
	Request DPM to evaluate request to enter a Mode
	**********************************************/

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;
	dev_info(pd_data->dev, "%s Rx SVID : 0x%x\n", __func__, rx_svid);

	switch (rx_svid) {
	case SAMSUNG_VENDOR_ID:
		manager->vpdo_received = 1;
		pdic_event_work(pd_data, PDIC_NOTIFY_DEV_ALL,
				PDIC_NOTIFY_ID_SVID_INFO, rx_svid/*Standard Vendor ID*/, 0, 0);
		manager->Standard_Vendor_ID = rx_svid;
		ret = PE_UFP_VDM_Mode_Entry_ACK;
		break;
	case TypeC_DP_SUPPORT:
		/* Certification: Ellisys: TD.PD.VDMU.E15.Applicability */
		ret = PE_UFP_VDM_Mode_Entry_ACK;
		break;
	default:
		ret = PE_UFP_VDM_Mode_Entry_NAK;
		break;
	}

	return ret;
}

policy_state usbpd_policy_ufp_vdm_mode_entry_ack(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int rx_svid = 0;
	int ret = PE_UFP_VDM_Mode_Entry_ACK;

	/**********************************************
	Actions on entry:
	Send Enter Mode ACK Command
	**********************************************/
	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;
	dev_info(pd_data->dev, "%s Rx SVID : 0x%x\n", __func__, rx_svid);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_ACK;
	policy->tx_data_obj[0].structured_vdm.command = Enter_Mode;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_mode_entry_nak(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int rx_svid = 0;
	int ret = PE_UFP_VDM_Mode_Entry_NAK;

	/**********************************************
	Actions on entry:
	Send Enter Mode NAK Command response as requested
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_NAK;
	policy->tx_data_obj[0].structured_vdm.command = Enter_Mode;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_mode_exit(struct policy_data *policy)
{
	uint16_t rx_svid;
	int ret = PE_UFP_VDM_Mode_Exit;

	/**********************************************
	Actions on entry:
	Request DPM to evaluate request to exit the requested Mode
	**********************************************/
	rx_svid = policy->rx_data_obj[0].structured_vdm.svid;
	usbpd_info("%s Rx SVID : 0x%x\n", __func__, rx_svid);

	switch (rx_svid) {
	case SAMSUNG_VENDOR_ID:
		ret = PE_UFP_VDM_Mode_Exit_ACK;
		break;
	case TypeC_DP_SUPPORT:
		ret = PE_UFP_VDM_Mode_Exit_ACK;
		break;
	default:
		ret = PE_UFP_VDM_Mode_Exit_NAK;
		break;
	}

	return ret;
}

policy_state usbpd_policy_ufp_vdm_mode_exit_ack(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int rx_svid = 0;
	int ret = PE_UFP_VDM_Mode_Exit_NAK;

	/**********************************************
	Actions on entry:
	Send Exit Mode ACK Command
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_ACK;
	policy->tx_data_obj[0].structured_vdm.command = Exit_Mode;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_mode_exit_nak(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int rx_svid = 0;
	int ret = PE_UFP_VDM_Mode_Exit_NAK;

	/**********************************************
	Actions on entry:
	Send Exit Mode NAK Command
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_NAK;
	policy->tx_data_obj[0].structured_vdm.command = Exit_Mode;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_attention_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = PE_UFP_VDM_Attention_Request;

	/**********************************************
	Actions on entry:
	Send Attention Command request
	**********************************************/
	//dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
/*	policy->tx_msg_header.num_data_objs = 1; number of objects*/

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
	policy->tx_data_obj[0].structured_vdm.command = Attention;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_evaluate_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Todo
	 *check DPM evaluate request to inform status
	*/

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_status_ack(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = PE_UFP_VDM_Status_ACK;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_ACK;
	policy->tx_data_obj[0].structured_vdm.command = DisplayPort_Status_Update;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_status_nak(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int rx_svid = 0;
	int ret = PE_UFP_VDM_Status_NAK;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_NAK;
	policy->tx_data_obj[0].structured_vdm.command = DisplayPort_Status_Update;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_evaluate_configure(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Todo
	 * check DPM evaluate request to inform status
	*/

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_configure_ack(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = PE_UFP_VDM_Configure_ACK;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_ACK;
	policy->tx_data_obj[0].structured_vdm.command = DisplayPort_Configure;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_ufp_vdm_configure_nak(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int rx_svid = 0;
	int ret = PE_UFP_VDM_Configure_NAK;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	rx_svid = pd_data->protocol_rx.data_obj[0].structured_vdm.svid;

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_UFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = rx_svid;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Responder_NAK;
	policy->tx_data_obj[0].structured_vdm.command = DisplayPort_Configure;

	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

/* the end ufp */

static policy_state usbpd_policy_dfp_vdm_evaluate(struct policy_data *policy)
{
	struct usbpd_data *pd_data= policy_to_usbpd(policy);
	int is_timeout = 0;
	u8 vdm_command = pd_data->protocol_rx.data_obj[0].structured_vdm.command;
	u8 vdm_type = pd_data->protocol_rx.data_obj[0].structured_vdm.command_type;
	int power_role = 0;
	long long ms = 0;
	int ret = PE_DFP_VDM_EVALUATE;
	
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	ms = usbpd_check_timer_vdm(pd_data);
	if (ms >= tVDMSenderResponse) {
		dev_info(pd_data->dev, "tVDMSenderResponse over\n");
		is_timeout = 1;
	}

	usbpd_info("%s, vdm [0x%x], vdm(0x%x), type(0x%x)\n", __func__,
			pd_data->protocol_rx.data_obj[0].object, vdm_command, vdm_type);

	switch (vdm_command) {
	case Discover_Identity:
		if (vdm_type == Initiator) {
			ret = PE_UFP_VDM_Get_Identity_NAK;
			break;
		} else if (is_timeout) {
			ret = PE_DFP_VDM_Identity_Request;
			break;
		} else if (vdm_type == Responder_ACK) {
			ret = PE_DFP_VDM_Identity_ACKed;
			break;
		} else {
			/* IF PD2.0, selfsoft reset for BIST Carrier 2 Test */
			/* Soft reset in not performed when vdm msg is received
			if(pd_data->specification_revision == USBPD_PD2_0) {
				usleep_range(2000, 2100);
				PDIC_OPS_FUNC(soft_reset, pd_data);
			}
			*/
			ret = PE_DFP_VDM_Identity_NAKed;
			break;
		}
	case Discover_SVIDs:
		if (is_timeout) {
			ret = PE_DFP_VDM_SVIDs_Request;
			break;
		} else if (vdm_type == Responder_ACK) {
			ret = PE_DFP_VDM_SVIDs_ACKed;
			break;
		} else {
			ret = PE_DFP_VDM_SVIDs_NAKed;
			break;
		}
	case Discover_Modes:
		if (is_timeout) {
			ret = PE_DFP_VDM_Modes_Request;
			break;
		} else if (vdm_type == Responder_ACK) {
			ret = PE_DFP_VDM_Modes_ACKed;
			break;
		} else {
			ret = PE_DFP_VDM_Modes_NAKed;
			break;
		}
	case Enter_Mode:
		if (is_timeout) {
			ret = PE_DFP_VDM_Mode_Entry_Request;
			break;
		} else if (vdm_type == Responder_ACK) {
			ret = PE_DFP_VDM_Mode_Entry_ACKed;
			break;
		} else {
			ret = PE_DFP_VDM_Mode_Entry_NAKed;
			break;
		}
	case Exit_Mode:
		if (is_timeout) {
			ret = PE_DFP_VDM_Mode_Exit_Request;
			break;
		} else if (vdm_type == Responder_ACK) {
			ret = PE_DFP_VDM_Mode_Exit_ACKed;
			break;
		} else {
			ret = PE_DFP_VDM_Mode_Exit_NAKed;
			break;
		}
	case DisplayPort_Status_Update:
		usbpd_info("%s : command(%d), command_type(%d), obj_pos(%d), version(%d), vdm_type(%d)\n",
				__func__, policy->rx_data_obj[0].structured_vdm.command,
				policy->rx_data_obj[0].structured_vdm.command_type,
				policy->rx_data_obj[0].structured_vdm.obj_pos,
				policy->rx_data_obj[0].structured_vdm.version,
				policy->rx_data_obj[0].structured_vdm.vdm_type);
		if (is_timeout) {
			ret = PE_DFP_VDM_Status_Update;
			break;
		} else if (vdm_type == Responder_ACK) {
			ret = PE_DFP_VDM_Status_Update_ACKed;
			break;
		} else {
			ret = PE_DFP_VDM_Status_Update_NAKed;
			break;
		}
	case DisplayPort_Configure:
		if (is_timeout) {
			ret = PE_DFP_VDM_DisplayPort_Configure;
			break;
		} else if (vdm_type == Responder_ACK) {
			ret = PE_DFP_VDM_DisplayPort_Configure_ACKed;
			break;
		} else {
			ret = PE_DFP_VDM_DisplayPort_Configure_NAKed;
			break;
		}
	}

	/* invalid vdm */
	if (ret == PE_DFP_VDM_EVALUATE)
		ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_identity_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Discover Identity request
	Start VDMResponseTimer
	**********************************************/
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_DFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
	policy->tx_data_obj[0].structured_vdm.command = Discover_Identity;

	pd_data->counter.discover_identity_counter++;

	/* Send Message */
	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	usbpd_timer_vdm_start(pd_data);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

static policy_state usbpd_policy_dfp_vdm_response(struct policy_data *policy,
					usbpd_manager_event_type event)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	usbpd_manager_inform_event(pd_data, event);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_identity_acked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Inform DPM of identity
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	usbpd_manager_inform_event(pd_data, MANAGER_DISCOVER_IDENTITY_ACKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_identity_naked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Inform DPM of result
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	/* Interrupt Status Bit Clear  */
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, MSG_ERROR);

	/* Clear Message Bit */
	PDIC_OPS_PARAM_FUNC(get_status, pd_data, VDM_DISCOVER_IDENTITY);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	usbpd_manager_inform_event(pd_data, MANAGER_DISCOVER_IDENTITY_NAKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_svids_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Discover SVIDs request
	Start VDMResponseTimer
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_DFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
	policy->tx_data_obj[0].structured_vdm.command = Discover_SVIDs;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	usbpd_timer_vdm_start(pd_data);

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_dfp_vdm_svids_acked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Inform DPM of SVIDs
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_DISCOVER_SVID_ACKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_svids_naked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Inform DPM of result
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_DISCOVER_SVID_NAKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_modes_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	struct usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Discover Modes request
	Start VDMResponseTimer
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_DFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = manager->SVID_0;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
	policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
	policy->tx_data_obj[0].structured_vdm.command = Discover_Modes;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	usbpd_timer_vdm_start(pd_data);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_modes_acked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	Actions on entry:
	Inform DPM of Modes
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	return usbpd_policy_dfp_vdm_response(policy,
				MANAGER_DISCOVER_MODE_ACKED);
}

policy_state usbpd_policy_dfp_vdm_modes_naked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Inform DPM of result
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	usbpd_manager_inform_event(pd_data, MANAGER_DISCOVER_MODE_NAKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_entry_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	struct usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Mode Entry request
	Start VDMModeEntryTimer
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_DFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = manager->SVID_0;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
	policy->tx_data_obj[0].structured_vdm.command = Enter_Mode;

	/* TODO: obj_pos , vdo should be set by device manager */
	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	usbpd_timer_vdm_start(pd_data);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_entry_acked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Request DPM to enter the mode
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_ENTER_MODE_ACKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_entry_naked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Inform DPM of reason for failure
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_ENTER_MODE_NAKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_exit_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Send Exit Mode request
	Start VDMModeExitTimer
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_DFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
	policy->tx_data_obj[0].structured_vdm.command = Exit_Mode;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	usbpd_timer_vdm_start(pd_data);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_exit_acked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Inform DPM of ACK
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_EXIT_MODE_ACKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_exit_naked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Inform DPM of NAK
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_EXIT_MODE_NAKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_attention_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Inform Device Policy Manager of Attention Command request
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_ATTENTION_REQUEST);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_status_update(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_DFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 2;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID_1;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
	policy->tx_data_obj[0].structured_vdm.command = DisplayPort_Status_Update;

	/* second object for vdo */
	policy->tx_data_obj[1].object = 0;
	policy->tx_data_obj[1].displayport_status.port_connected = 1;

	/* TODO: obj_pos , vdo should be set by device manager */

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	usbpd_timer_vdm_start(pd_data);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_status_update_acked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_STATUS_UPDATE_ACKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_status_update_naked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_STATUS_UPDATE_NAKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_displayport_configure(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
	policy->tx_msg_header.port_data_role = USBPD_DFP;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 2;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].structured_vdm.svid = PD_SID_1;
	policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
	if (pd_data->protocol_rx.msg_header.spec_revision >= USBPD_PD3_0) {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version2;
		//policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x01; /* Ellisys */
	} else {
		policy->tx_data_obj[0].structured_vdm.version = VDM_Version1;
		policy->tx_data_obj[0].structured_vdm.version_minor = 0x00;
	}
	policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
	policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
	policy->tx_data_obj[0].structured_vdm.command = DisplayPort_Configure;

	/* second object for vdo */
	policy->tx_data_obj[1].object = 0;
	policy->tx_data_obj[1].displayport_configurations.select_configuration = USB_U_AS_UFP_D;
	policy->tx_data_obj[1].displayport_configurations.displayport_protocol = DP_V_1_3;
	policy->tx_data_obj[1].displayport_configurations.ufp_u_pin_assignment = PIN_ASSIGNMENT_D;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	usbpd_timer_vdm_start(pd_data);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_displayport_configure_acked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_DisplayPort_Configure_ACKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_vdm_displayport_configure_naked(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = 0;
	int power_role = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	usbpd_manager_inform_event(pd_data, MANAGER_DisplayPort_Configure_NACKED);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_uvdm_send_message(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	struct usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	usbpd_send_msg(pd_data, &manager->uvdm_msg_header, manager->uvdm_data_obj);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dfp_uvdm_receive_message(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int ret = 0;

	/**********************************************
	**********************************************/

	dev_info(pd_data->dev, "%s\n", __func__);

	usbpd_manager_inform_event(pd_data, MANAGER_UVDM_RECEIVE_MESSAGE);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	ret = (power_role == USBPD_SOURCE ? PE_SRC_Ready : PE_SNK_Ready);

	return ret;
}

policy_state usbpd_policy_dr_src_get_source_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_DR_SRC_Get_Source_Cap;
	int data_role = 0;
	long long ms = 0;

	/**********************************************
	get source capabilities request from Device Policy Manager

	Actions on entry:
	Send Get_Source_Cap message
	Initialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Get_Source_Cap, data_role, USBPD_SOURCE)) {
		SOFT_RESET();
	} else {
		/* Start Timer */
		usbpd_timer1_start(pd_data);

		/* Wait Message or State */
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_DR_SRC_Get_Source_Cap;
				break;
			}

			CHECK_MSG_BREAK(MSG_SRC_CAP, PE_SRC_Ready);
			CHECK_MSG_BREAK(MSG_REJECT, PE_SRC_Ready);

			/* TimeOver Check */
			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
				ret = PE_SRC_Ready;
				break;
			}
		}
	}

	/**********************************************
	Actions on exit:
	Pass source capabilities/outcome to Device Policy Manager

	Source capabilities message received
	| SenderResponseTimer Timeout | Reject message received
	**********************************************/

	return ret;
}

policy_state usbpd_policy_dr_src_give_sink_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = PE_DR_SRC_Give_Sink_Cap;

	/**********************************************
	Actions on entry:
	Get present sink capabilities from Device Policy Manager
	Send Capabilities message (based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Sink Cap Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Sink_Capabilities;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 2;

	policy->tx_data_obj[0].object = pd_data->sink_data_obj[0].object;
	policy->tx_data_obj[1].object = pd_data->sink_data_obj[1].object;

	/* Clear Interrupt Status */

	/* Send Message */
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = PE_SRC_Ready;

	/* Sink Capabilities message sent */
	return ret;
}

policy_state usbpd_policy_dr_snk_get_sink_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_DR_SNK_Get_Sink_Cap;
	int data_role = 0;
	int power_role = 0;
	long long ms = 0;

	/**********************************************
	get sink capabilities request from Device Policy Manager

	Actions on entry:
	Send Get_Sink_Cap message
	Initialize and run
	SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Send Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Get_Sink_Cap, data_role, USBPD_SINK)) {
		SOFT_RESET();
	} else {
		/* Start Timer */
		usbpd_timer1_start(pd_data);

		/* Wait Message or State */
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_DR_SNK_Get_Sink_Cap;
				break;
			}

			CHECK_MSG_BREAK(MSG_SNK_CAP, PE_SNK_Ready);
			CHECK_MSG_BREAK(MSG_REJECT, PE_SNK_Ready);

			/* TimeOver Check */
			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
				ret = PE_SNK_Ready;
				break;
			}
		}
	}

	/**********************************************
	Actions on exit:
	Pass sink capabilities/outcome to
	Device Policy Manager

	Sink capabilities message received
	| SenderResponseTimer Timeout | Reject message received
	**********************************************/

	return ret;
}

policy_state usbpd_policy_dr_snk_give_source_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_DR_SNK_Give_Source_Cap;
	int data_role = 0;
	int power_role = 0;
	/**********************************************
	Actions on entry:
	Request source capabilities from
	Device Policy Manager
	Send Capabilities message
	**********************************************/

	/* PD State Inform to AP */
	//dev_info(pd_data->dev, "%s\n", __func__);

	/* Read Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Source_Capabilities;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = USBPD_SINK;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].power_data_obj.max_current = 500 / 10;
	policy->tx_data_obj[0].power_data_obj.voltage = 5000 / 50;
	policy->tx_data_obj[0].power_data_obj.peak_current = 0;
	policy->tx_data_obj[0].power_data_obj.rsvd = 0;
	policy->tx_data_obj[0].power_data_obj.unchunked_extended_message_supported = 0;
	policy->tx_data_obj[0].power_data_obj.data_role_swap = 1;
	policy->tx_data_obj[0].power_data_obj.usb_comm_capable = 1;
	policy->tx_data_obj[0].power_data_obj.externally_powered = 0;
	policy->tx_data_obj[0].power_data_obj.usb_suspend_support = 1;
	policy->tx_data_obj[0].power_data_obj.dual_role_power = 1;
	policy->tx_data_obj[0].power_data_obj.supply = 0;

	/* Send Message */
	if (!usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj)) {
		SOFT_RESET();
	} else
		ret = PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_bist_carrier_mode(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_BIST_Carrier_Mode;

	/**********************************************
	Actions on entry:
	Consume Frame
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/**********************************************
	Actions on exit:
    Detach
    or Hard Reset signaling received
	**********************************************/

	return ret;

}

policy_state usbpd_policy_src_send_not_supported(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Send_Not_Supported;
	int data_role = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Send Not_Supported MessagePower
	**********************************************/

	/* PD State Inform to AP */
	//dev_info(pd_data->dev, "%s\n", __func__);

	/* Get Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* tChunkingNotSupported 40~50ms */
	if ((pd_data->protocol_rx.msg_header.extended)
		&& (pd_data->protocol_rx.data_obj[0].extended_msg_header_type.chunked)
		&& (pd_data->protocol_rx.data_obj[0].extended_msg_header_type.data_size >= 26)) {
		msleep(30);
	}

	/* Send Not_Supported Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Not_Supported, data_role, USBPD_SOURCE)) {
		SOFT_RESET();
	} else
		ret = PE_SRC_Ready;

	return ret;
}

policy_state usbpd_policy_src_not_supported_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Not_Supported_Received;

	/**********************************************
	Actions on entry:
	Inform Device Policy Manager of Not_Supported Message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_src_chunk_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Chunk_Received;

	/**********************************************
	Actions on entry:
	Start ChunkingNotSupportedTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_snk_send_not_supported(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Send_Not_Supported;
	int data_role = 0;
	int power_role = 0;

	/**********************************************
	Actions on entry:
	Send Not_Supported Message
	**********************************************/

	/* PD State Inform to AP */
	//dev_info(pd_data->dev, "%s\n", __func__);

	/* Get Data Role */
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* tChunkingNotSupported 40~50ms */
	if ((pd_data->protocol_rx.msg_header.extended)
		&& (pd_data->protocol_rx.data_obj[0].extended_msg_header_type.chunked)
		&& (pd_data->protocol_rx.data_obj[0].extended_msg_header_type.data_size >= 26)) {
		msleep(30);
	}

	/* Send Not_Supported Message */
	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Not_Supported, data_role, USBPD_SINK)) {
		SOFT_RESET();
	} else
		ret = PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_snk_not_supported_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Not_Supported_Received;

	/**********************************************
	Actions on entry:
	Inform Device Policy Manager of Not_Supported Message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_snk_chunk_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Chunk_Received;

	/**********************************************
	Actions on entry:
	Start ChunkingNotSupportedTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_src_send_source_alert(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Send_Source_Alert;

	/**********************************************
	Actions on entry:
	Send Alert Message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_snk_source_alert_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Source_Alert_Received;

	/**********************************************
	Actions on entry:
	Inform DPM of the detail of the alert
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	if ((policy->rx_data_obj[0].object >> 25) & 1)
		ret = PE_Get_Battery_Status;
	else
		ret = PE_Get_Status;

	return ret;
}

policy_state usbpd_policy_snk_send_sink_alert(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Send_Sink_Alert;

	/**********************************************
	Actions on entry:
	Send Alert Message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_src_sink_alert_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Sink_Alert_Received;

	/**********************************************
	Actions on entry:
	Inform DPM of the detail of the alert
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_snk_get_source_cap_ext(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Get_Source_Cap_Ext;

	/**********************************************
	Actions on entry:
	Send Get_Source_Cap_Extended MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/**********************************************
	Actions on exit:
	Pass source extended capabilities/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_src_give_source_cap_ext(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int i;
	int data_role = 0;
	int power_role = 0;
	int ret = PE_SRC_Give_Source_Cap_Ext;

	/**********************************************
	Actions on entry:
	Get present extended source capabilities
	from Device Policy ManagerSend Source_Capabilities_Extended message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Source_Capabilities_Extended;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 7;
	policy->tx_msg_header.extended = 1;

	for (i = 0; i < 7; i++)
		policy->tx_data_obj[i].object = 0;

	policy->tx_data_obj[0].extended_msg_header_type.chunked = 1; //no unchunked
	policy->tx_data_obj[0].extended_msg_header_type.data_size = 25;

	policy->tx_data_obj[0].Source_Capabilities_Extended_Data_Block_Type1.VID = SAMSUNG_VENDOR_ID;
	policy->tx_data_obj[1].Source_Capabilities_Extended_Data_Block_Type2.PID = 0x6860;

	policy->tx_data_obj[6].Source_Capabilities_Extended_Data_Block_Type7.Number_of_Batteries_Slots = 0x01;
	policy->tx_data_obj[6].Source_Capabilities_Extended_Data_Block_Type7.SPR_Source_PDP_Rating = 0x02;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
	policy->tx_msg_header.extended = 0;

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_snk_get_source_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0, data_role = 0;
	int ret = PE_SNK_Get_Source_Status;

	/**********************************************
	Actions on entry:
	Send Get_Status MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;
	/**********************************************
	Actions on exit:
	Pass Source status/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_get_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0, data_role = 0;
	int ret = PE_Get_Status;
	long long ms = 0;

	/**********************************************
	Actions on entry:
	Send Get_Status MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Get_Status, data_role, power_role)) {
		SOFT_RESET();
	} else {
		/* Wait Message or State */
		while (1) {
			if (policy->rx_softreset || policy->rx_hardreset) {
				ret = 0;
				break;
			}
			if (policy->plug_valid == 0) {
				ret = PE_Get_Status;
				break;
			}
	
			CHECK_MSG_BREAK(MSG_STATUS, (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready);
	
			/* TimeOver Check */
			ms = usbpd_check_time1(pd_data);
			if (ms >= tSenderResponse) {
				ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;
				break;
			}
		}
	}
	/**********************************************
	Actions on exit:
	Pass Source status/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_give_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int i;
	int data_role = 0;
	int power_role = 0;
	int ret = PE_Give_Status;

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Status;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 3; /* GraphicUSB */
	//policy->tx_msg_header.num_data_objs = 2; /* Ellisys */
	policy->tx_msg_header.extended = 1;

	for (i = 0; i < 3; i++) /* Ellisys */
		policy->tx_data_obj[i].object = 0;

	policy->tx_data_obj[0].extended_msg_header_type.chunked = 1; //no unchunked
	policy->tx_data_obj[0].extended_msg_header_type.data_size = 7; /* GraphicUSB */
	//policy->tx_data_obj[0].extended_msg_header_type.data_size = 6; /* Ellisys */

	policy->tx_data_obj[0].status_type1.Internal_Temp = 25;
	policy->tx_data_obj[0].status_type1.Present_Input = 0x08; /*Internal Power from Battery when set */
	policy->tx_data_obj[1].status_type2.Temperature_Status = 0x02; /* Normal */

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
	policy->tx_msg_header.extended = 0;
	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_give_revision(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = 0;

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Revision;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;

	policy->tx_data_obj[0].revision.Revision_major = 3;
	policy->tx_data_obj[0].revision.Revision_minor = 1;
	policy->tx_data_obj[0].revision.Version_major = 1;
	policy->tx_data_obj[0].revision.Version_minor = 8;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_dr_snk_give_source_info(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = 0;

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Source_Info;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;

	policy->tx_data_obj[0].source_info.Port_Type = 1;
	policy->tx_data_obj[0].source_info.Port_Maximum_PDP = 2;
	policy->tx_data_obj[0].source_info.Port_Present_PDP = 2;
	policy->tx_data_obj[0].source_info.Port_Reported_PDP = 2;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_src_give_source_info(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = 0;

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Source_Info;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;

	policy->tx_data_obj[0].object = 0;

	policy->tx_data_obj[0].source_info.Port_Type = 1;
	policy->tx_data_obj[0].source_info.Port_Maximum_PDP = 2;
	policy->tx_data_obj[0].source_info.Port_Present_PDP = 2;
	policy->tx_data_obj[0].source_info.Port_Reported_PDP = 2;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_src_give_source_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Give_Source_Status;

	/**********************************************
	Actions on entry:
	Get present Source status from Device Policy ManagerSend Status message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_src_get_sink_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Get_Sink_Status;

	/**********************************************
	Actions on entry:
	Send Get_Status MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/**********************************************
	Actions on exit:
	Pass Sink status/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_snk_give_sink_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int i;
	int data_role = 0;
	int power_role = 0;
	int ret = PE_SNK_Give_Sink_Status;

	/**********************************************
	Actions on entry:
	Get present Sink status from Device Policy ManagerSend Status message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Status;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 3;
	policy->tx_msg_header.extended = 1;

	for (i = 0; i < 3; i++)
		policy->tx_data_obj[i].object = 0;

	policy->tx_data_obj[0].extended_msg_header_type.chunked = 1; //no unchunked
	policy->tx_data_obj[0].extended_msg_header_type.data_size = 7;

	policy->tx_data_obj[0].status_type1.Internal_Temp = 25;
	policy->tx_data_obj[0].status_type1.Present_Input = 0x08; /*Internal Power from Battery when set */
	policy->tx_data_obj[1].status_type2.Temperature_Status = 0x02; /* Normal */

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
	policy->tx_msg_header.extended = 0;

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_snk_get_pps_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SNK_Get_PPS_Status;

	/**********************************************
	Actions on entry:
	Send Get_PPS_Status MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/**********************************************
	Actions on exit:
	Pass Source status/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_src_give_pps_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_SRC_Give_PPS_Status;

	/**********************************************
	Actions on entry:
	Get present Source PPS status from Device Policy ManagerSend PPS_Status message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_get_battery_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Get_Battery_Cap;

	/**********************************************
	Actions on entry:
	Send Get_Battery_Cap MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/**********************************************
	Actions on exit:
	Pass Battery capabilities/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_give_battery_cap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int i;
	int data_role = 0;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Get present Battery capabilities from Device Policy ManagerSend Battery_Capabilities Message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Battery_Capabilities;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 3;
	policy->tx_msg_header.extended = 1;

	for (i = 0; i < 3; i++)
		policy->tx_data_obj[i].object = 0;

	policy->tx_data_obj[0].extended_msg_header_type.chunked = 1; //no unchunked
	policy->tx_data_obj[0].extended_msg_header_type.data_size = 9;

//	(*(Battery_Capabilities *)&policy->tx_data_obj[0].byte[2]).VID = SAMSUNG_VENDOR_ID;
//	(*(Battery_Capabilities *)&policy->tx_data_obj[0].byte[2]).PID = 0x6860;

	if (policy->rx_data_obj[0].byte[2] ==  0) {
		policy->tx_data_obj[0].Battery_Capabilities1.VID = SAMSUNG_VENDOR_ID;
		policy->tx_data_obj[1].Battery_Capabilities2.PID = 0x6860;
		policy->tx_data_obj[1].Battery_Capabilities2.Battery_Design_Capacity = 0xFFFF;
		policy->tx_data_obj[2].Battery_Capabilities3.Last_Full_Charge = 0xFFFF;
	} else {
		policy->tx_data_obj[0].Battery_Capabilities1.VID = 0xFFFF;
		policy->tx_data_obj[2].Battery_Capabilities3.Battery_Type = 1; /* Invalid Batt Reft bit */
	}

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
	policy->tx_msg_header.extended = 0;

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_get_battery_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = PE_Get_Battery_Status;

	/**********************************************
	Actions on entry:
	Send Get_Battery_Status MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Get_Battery_Status;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;
	policy->tx_msg_header.extended = 1;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].extended_msg_header_type.chunked = 1; //no unchunked
	policy->tx_data_obj[0].extended_msg_header_type.data_size = 1;

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
	policy->tx_msg_header.extended = 0;
	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

    /*********************************************
	Actions on exit:
	Pass Battery status/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_give_battery_status(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int ret = 0;

	/**********************************************
	Actions on entry:
	Get present Battery status from Device Policy Manager
	Send Battery_Status Message (based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Battery_Status;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 1;
	policy->tx_msg_header.extended = 0;

	policy->tx_data_obj[0].object = 0;
	policy->tx_data_obj[0].battery_status.battery_present_capacity = 0xFFFF;

	if (policy->rx_data_obj[0].byte[2] == 0) {
		policy->tx_data_obj[0].battery_status.battery_info |=
			((1 << 1) | ((power_role == USBPD_SINK ? 0 : 1) << 2));
	} else
		policy->tx_data_obj[0].battery_status.battery_info = 1;	/* Invalid Batt Reft bit */

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_policy_get_manufacturer_info(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Get_Manufacturer_Info;

	/**********************************************
	Actions on entry:
	Get present Manufacturer Information from Device Policy ManagerSend Manufacturer_Info Message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_give_manufacturer_info(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;
	int i;
	int ret = PE_Give_Manufacturer_Info;

	/**********************************************
	Actions on entry:
	Get present Manufacturer Information from Device Policy ManagerSend Manufacturer_Info Message
	(based on Device Policy Manager response)
	**********************************************/

	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);
	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);

	/* Message Setting */
	policy->tx_msg_header.msg_type = USBPD_Manufacturer_Info;
	policy->tx_msg_header.port_data_role = data_role;
	policy->tx_msg_header.port_power_role = power_role;
	policy->tx_msg_header.num_data_objs = 5;
	policy->tx_msg_header.extended = 1;

	for (i = 0; i < 5; i++)
		policy->tx_data_obj[i].object = 0;

	policy->tx_data_obj[0].extended_msg_header_type.chunked = 1; //no unchunked
	policy->tx_data_obj[0].extended_msg_header_type.data_size = 18;

	policy->tx_data_obj[0].Manufacturer_Info1.VID = 0x0000;
	policy->tx_data_obj[1].Manufacturer_Info2.PID = 0x0000;

	if (policy->rx_data_obj[0].byte[2] == 0) { //Port/Cable Plug
		policy->tx_data_obj[1].Manufacturer_Info2.Manufacturer_String1 = '0';
		policy->tx_data_obj[1].Manufacturer_Info2.Manufacturer_String2 = '\0';
	} else if (policy->rx_data_obj[0].byte[2] == 1 && policy->rx_data_obj[0].byte[3] == 0) {
		policy->tx_data_obj[1].Manufacturer_Info2.Manufacturer_String1 = '0';
		policy->tx_data_obj[1].Manufacturer_Info2.Manufacturer_String2 = '\0';
	} else {
		policy->tx_data_obj[0].Manufacturer_Info1.VID = 0xFFFF;
		policy->tx_data_obj[1].Manufacturer_Info2.PID = 0x0000;

		policy->tx_data_obj[1].Manufacturer_Info2.Manufacturer_String1 = 'N';
		policy->tx_data_obj[1].Manufacturer_Info2.Manufacturer_String2 = 'o';

		policy->tx_data_obj[2].Manufacturer_Info3.Manufacturer_String1 = 't';
		policy->tx_data_obj[2].Manufacturer_Info3.Manufacturer_String2 = ' ';
		policy->tx_data_obj[2].Manufacturer_Info3.Manufacturer_String3 = 'S';
		policy->tx_data_obj[2].Manufacturer_Info3.Manufacturer_String4 = 'u';

		policy->tx_data_obj[3].Manufacturer_Info4.Manufacturer_String1 = 'p';
		policy->tx_data_obj[3].Manufacturer_Info4.Manufacturer_String2 = 'p';
		policy->tx_data_obj[3].Manufacturer_Info4.Manufacturer_String3 = 'o';
		policy->tx_data_obj[3].Manufacturer_Info4.Manufacturer_String4 = 'r';

		policy->tx_data_obj[4].Manufacturer_Info5.Manufacturer_String1 = 't';
		policy->tx_data_obj[4].Manufacturer_Info5.Manufacturer_String2 = 'e';
		policy->tx_data_obj[4].Manufacturer_Info5.Manufacturer_String3 = 'd';
		policy->tx_data_obj[4].Manufacturer_Info5.Manufacturer_String4 = '\0';
	}

	usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
	policy->tx_msg_header.extended = 0;

	ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_get_country_codes(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Get_Country_Codes;

	/**********************************************
	Actions on entry:
	Send Get_Country_Codes MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/**********************************************
	Actions on exit:
	Pass Country Codes/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_give_country_codes(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Give_Country_Codes;

	/**********************************************
	Actions on entry:
	Get present Country Codes from Device Policy ManagerSend Country_Codes Message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_get_country_info(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Get_Country_Info;

	/**********************************************
	Actions on entry:
	Send Get_Country_Info MessageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	/**********************************************
	Actions on exit:
	Pass Country Information/outcome to Device Policy Manager
	**********************************************/

	return ret;
}

policy_state usbpd_policy_give_country_info(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Give_Country_Info;

	/**********************************************
	Actions on entry:
	Get present Country Information from Device Policy ManagerSend Country_Info Message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_send_security_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Send_Security_Request;

	/**********************************************
	Actions on entry:
	Send Security_Request Message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_send_security_response(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Send_Security_Response;

	/**********************************************
	Actions on entry:
	Get present Security response from Device Policy ManagerSend Security_Response Message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_security_response_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Security_Response_Received;

	/**********************************************
	Actions on entry:
	Inform Device Policy Manager of the security response details.
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_send_firmware_update_request(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Send_Firmware_Update_Request;

	/**********************************************
	Actions on entry:
	Send Firmware_Update_Request Message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_send_firmware_update_response(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Send_Firmware_Update_Response;

	/**********************************************
	Actions on entry:
	Get present firmware update response from Device Policy ManagerSend Firmware_Update_Response Message
	(based on Device Policy Manager response)
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_firmware_update_response_received(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_Firmware_Update_Response_Received;

	/**********************************************
	Actions on entry:
	Inform Device Policy Manager of the firmware update response details.
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_src_snk_evaluate_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SRC_SNK_Evaluate_Swap;

	/**********************************************
	Actions on entry:
	Ask Device Policy Manager if Fast Role Swap signaled on CC wire
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_src_snk_accept_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SRC_SNK_Accept_Swap;

	/**********************************************
	Actions on entry:
	Send Accept Message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_src_snk_transition_to_off(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SRC_SNK_Transition_to_off;

	/**********************************************
	Actions on entry:
	Wait for VBUS to reach vSafe5V
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_src_snk_assert_rd(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SRC_SNK_Assert_Rd;

	/**********************************************
	Actions on entry:
	Request DPM to assert Rd
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_src_snk_wait_source_on(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SRC_SNK_Wait_Source_on;

	/**********************************************
	Actions on entry:
	Send PS_RDY MessageInitialize and run PSSourceOnTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_snk_src_start_ams(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SNK_SRC_Start_AMS;

	/**********************************************
	Actions on entry:
	Notify the Protocol Layer that the first Message in the AMS will follow.
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_snk_src_send_swap(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SNK_SRC_Send_Swap;

	/**********************************************
	Actions on entry:
	Send FR_Swap messageInitialize and run SenderResponseTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_snk_src_transition_to_off(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SNK_SRC_Transition_to_off;

	/**********************************************
	Actions on entry:
	Initialize and run PSSourceOffTimer
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_snk_src_vbus_applied(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SNK_SRC_Vbus_Applied;

	/**********************************************
	Actions on entry:
	Request Device Policy Manager to notify when vSafe5v is being applied by the local power source.
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_snk_src_assert_rp(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SNK_SRC_Assert_Rp;

	/**********************************************
	Actions on entry:
	Request DPM to assert Rp
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_frs_snk_src_source_on(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int ret = PE_FRS_SNK_SRC_Source_on;

	/**********************************************
	Actions on entry:
	Send PS_RDY Message
	**********************************************/

	/* PD State Inform to AP */
	dev_info(pd_data->dev, "%s\n", __func__);

	return ret;
}

policy_state usbpd_policy_dr_send_reject(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0, data_role = 0;
	int ret = 0;

	PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
	PDIC_OPS_PARAM_FUNC(get_data_role, pd_data, &data_role);

	if (!usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header, USBPD_Reject, data_role, power_role)) {
		SOFT_RESET();
	} else
		ret = (power_role == USBPD_SOURCE) ? PE_SRC_Ready : PE_SNK_Ready;

	return ret;
}

policy_state usbpd_error_recovery(struct policy_data *policy)
{
	struct usbpd_data *pd_data = policy_to_usbpd(policy);

	/**********************************************
	**********************************************/

	dev_err(pd_data->dev, "%s\n", __func__);

	PDIC_OPS_PARAM_FUNC(set_pd_control, pd_data, USBPD_CC_OFF);
	PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_SOURCE);
	PDIC_OPS_PARAM_FUNC(set_rp_control, pd_data, PLUG_CTRL_RP0);
	msleep(23);

	PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);

	return PE_SNK_Startup;
}

void usbpd_policy_work(struct work_struct *work)
{
	struct usbpd_data *pd_data = container_of(work, struct usbpd_data,
			worker);
	struct policy_data *policy = &pd_data->policy;
	int power_role = 0;
	policy_state next_state = policy->state;
	policy_state saved_state;
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
	union power_supply_propval val;
	struct power_supply *psy;
#endif

	__pm_stay_awake(pd_data->policy_wake);
	do {
		if (!policy->plug_valid) {
			usbpd_info("%s : usbpd cable is empty\n", __func__);
			break;
		}

		if (policy->rx_hardreset || policy->rx_softreset
				|| policy->plug) {
			saved_state = 0;
			next_state = 0; /* default */
		}
		saved_state = next_state;
		switch (next_state) {
		case PE_SRC_Startup:
			next_state = usbpd_policy_src_startup(policy);
			break;
		case PE_SRC_Discovery:
			next_state = usbpd_policy_src_discovery(policy);
			break;
		case PE_SRC_Send_Capabilities:
			next_state = usbpd_policy_src_send_capabilities(policy);
			break;
		case PE_SRC_Negotiate_Capability:
			next_state = usbpd_policy_src_negotiate_capability(policy);
			break;
		case PE_SRC_Transition_Supply:
			next_state = usbpd_policy_src_transition_supply(policy);
			break;
		case PE_SRC_Ready:
			next_state = usbpd_policy_src_ready(policy);
			break;
		case PE_SRC_Disabled:
			next_state = usbpd_policy_src_disabled(policy);
			break;
		case PE_SRC_Capability_Response:
			next_state = usbpd_policy_src_capability_response(policy);
			break;
		case PE_SRC_Hard_Reset:
			next_state = usbpd_policy_src_hard_reset(policy);
			break;
		case PE_SRC_Hard_Reset_Received:
			next_state = usbpd_policy_src_hard_reset_received(policy);
			break;
		case PE_SRC_Transition_to_default:
			next_state = usbpd_policy_src_transition_to_default(policy);
			break;
		case PE_SRC_Give_Source_Cap:
			next_state = usbpd_policy_src_give_source_cap(policy);
			break;
		case PE_SRC_Get_Sink_Cap:
			next_state = usbpd_policy_src_get_sink_cap(policy);
			break;
		case PE_SRC_Wait_New_Capabilities:
			next_state = usbpd_policy_src_wait_new_capabilities(policy);
			break;
		case PE_SRC_Send_Soft_Reset:
			next_state = usbpd_policy_src_send_soft_reset(policy);
			break;
		case PE_SRC_Soft_Reset:
			next_state = usbpd_policy_src_soft_reset(policy);
			break;
		case PE_SNK_Startup:
			next_state = usbpd_policy_snk_startup(policy);
			break;
		case PE_SNK_Discovery:
			next_state = usbpd_policy_snk_discovery(policy);
			break;
		case PE_SNK_Wait_for_Capabilities:
			next_state = usbpd_policy_snk_wait_for_capabilities(policy);
			break;
		case PE_SNK_Evaluate_Capability:
			next_state = usbpd_policy_snk_evaluate_capability(policy);
			break;
		case PE_SNK_Select_Capability:
			next_state = usbpd_policy_snk_select_capability(policy);
			break;
		case PE_SNK_Transition_Sink:
			next_state = usbpd_policy_snk_transition_sink(policy);
			break;
		case PE_SNK_Ready:
			next_state = usbpd_policy_snk_ready(policy);
			break;
		case PE_SNK_Hard_Reset:
			next_state = usbpd_policy_snk_hard_reset(policy);
			break;
		case PE_SNK_Transition_to_default:
			next_state = usbpd_policy_snk_transition_to_default(policy);
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
			psy = power_supply_get_by_name("battery");
			if (psy) {
				val.intval = 1;
				psy_do_property("battery", set,
					POWER_SUPPLY_EXT_PROP_HARDRESET_OCCUR, val);
			} else {
				usbpd_err("%s: Fail to get psy battery\n", __func__);
			}
#endif
			break;
		case PE_SNK_Give_Sink_Cap:
			next_state = usbpd_policy_snk_give_sink_cap(policy);
			break;
		case PE_SNK_Get_Source_Cap:
			next_state = usbpd_policy_snk_get_source_cap(policy);
			break;
		case PE_SNK_Send_Soft_Reset:
			next_state = usbpd_policy_snk_send_soft_reset(policy);
			break;
		case PE_SNK_Soft_Reset:
			next_state = usbpd_policy_snk_soft_reset(policy);
			break;
		case PE_DRS_Evaluate_Port:
			next_state = usbpd_policy_drs_evaluate_port(policy);
			break;
		case PE_DRS_Evaluate_Send_Port:
			next_state = usbpd_policy_drs_evaluate_send_port(policy);
			break;
		case PE_DRS_DFP_UFP_Evaluate_DR_Swap:
			next_state = usbpd_policy_drs_dfp_ufp_evaluate_dr_swap(policy);
			break;
		case PE_DRS_DFP_UFP_Accept_DR_Swap:
			next_state = usbpd_policy_drs_dfp_ufp_accept_dr_swap(policy);
			break;
		case PE_DRS_DFP_UFP_Change_to_UFP:
			next_state = usbpd_policy_drs_dfp_ufp_change_to_ufp(policy);
			break;
		case PE_DRS_DFP_UFP_Send_DR_Swap:
			next_state = usbpd_policy_drs_dfp_ufp_send_dr_swap(policy);
			break;
		case PE_DRS_DFP_UFP_Reject_DR_Swap:
			next_state = usbpd_policy_drs_dfp_ufp_reject_dr_swap(policy);
			break;
		case PE_DRS_UFP_DFP_Evaluate_DR_Swap:
			next_state = usbpd_policy_drs_ufp_dfp_evaluate_dr_swap(policy);
			break;
		case PE_DRS_UFP_DFP_Accept_DR_Swap:
			next_state = usbpd_policy_drs_ufp_dfp_accept_dr_swap(policy);
			break;
		case PE_DRS_UFP_DFP_Change_to_DFP:
			next_state = usbpd_policy_drs_ufp_dfp_change_to_dfp(policy);
			break;
		case PE_DRS_UFP_DFP_Send_DR_Swap:
			next_state = usbpd_policy_drs_ufp_dfp_send_dr_swap(policy);
			break;
		case PE_DRS_UFP_DFP_Reject_DR_Swap:
			next_state = usbpd_policy_drs_ufp_dfp_reject_dr_swap(policy);
			break;
		case PE_PRS_SRC_SNK_Reject_PR_Swap:
			next_state = usbpd_policy_prs_src_snk_reject_pr_swap(policy);
			break;
		case PE_PRS_SRC_SNK_Evaluate_Swap:
			next_state = usbpd_policy_prs_src_snk_evaluate_swap(policy);
			break;
		case PE_PRS_SRC_SNK_Send_Swap:
			next_state = usbpd_policy_prs_src_snk_send_swap(policy);
			break;
		case PE_PRS_SRC_SNK_Accept_Swap:
			next_state = usbpd_policy_prs_src_snk_accept_swap(policy);
			break;
		case PE_PRS_SRC_SNK_Transition_off:
			next_state = usbpd_policy_prs_src_snk_transition_to_off(policy);
			break;
		case PE_PRS_SRC_SNK_Assert_Rd:
			next_state = usbpd_policy_prs_src_snk_assert_rd(policy);
			break;
		case PE_PRS_SRC_SNK_Wait_Source_on:
			next_state = usbpd_policy_prs_src_snk_wait_source_on(policy);
			break;
		case PE_PRS_SNK_SRC_Reject_Swap:
			next_state = usbpd_policy_prs_snk_src_reject_swap(policy);
			break;
		case PE_PRS_SNK_SRC_Evaluate_Swap:
			next_state = usbpd_policy_prs_snk_src_evaluate_swap(policy);
			break;
		case PE_PRS_SNK_SRC_Send_Swap:
			next_state = usbpd_policy_prs_snk_src_send_swap(policy);
			break;
		case PE_PRS_SNK_SRC_Accept_Swap:
			next_state = usbpd_policy_prs_snk_src_accept_swap(policy);
			break;
		case PE_PRS_SNK_SRC_Transition_off:
			next_state = usbpd_policy_prs_snk_src_transition_to_off(policy);
			break;
		case PE_PRS_SNK_SRC_Assert_Rp:
			next_state = usbpd_policy_prs_snk_src_assert_rp(policy);
			break;
		case PE_PRS_SNK_SRC_Source_on:
			next_state = usbpd_policy_prs_snk_src_source_on(policy);
			break;
		case PE_VCS_Evaluate_Swap:
			next_state = usbpd_policy_vcs_evaluate_swap(policy);
			break;
		case PE_VCS_Accept_Swap:
			next_state = usbpd_policy_vcs_accept_swap(policy);
			break;
		case PE_VCS_Wait_for_VCONN:
			next_state = usbpd_policy_vcs_wait_for_vconn(policy);
			break;
		case PE_VCS_Turn_Off_VCONN:
			next_state = usbpd_policy_vcs_turn_off_vconn(policy);
			break;
		case PE_VCS_Turn_On_VCONN:
			next_state = usbpd_policy_vcs_turn_on_vconn(policy);
			break;
		case PE_VCS_Send_PS_RDY:
			next_state = usbpd_policy_vcs_send_ps_rdy(policy);
			break;
		case PE_VCS_Send_Swap:
			next_state = usbpd_policy_vcs_send_swap(policy);
			break;
		case PE_VCS_Reject_VCONN_Swap:
			next_state = usbpd_policy_vcs_reject_vconn_swap(policy);
			break;
		case PE_UFP_VDM_Get_Identity:
			next_state = usbpd_policy_ufp_vdm_get_identity(policy);
			break;
		case PE_UFP_VDM_Send_Identity:
			next_state = usbpd_policy_ufp_vdm_send_identity(policy);
			break;
		case PE_UFP_VDM_Get_Identity_NAK:
			next_state = usbpd_policy_ufp_vdm_get_identity_nak(policy);
			break;
		case PE_UFP_VDM_Get_SVIDs:
			next_state = usbpd_policy_ufp_vdm_get_svids(policy);
			break;
		case PE_UFP_VDM_Send_SVIDs:
			next_state = usbpd_policy_ufp_vdm_send_svids(policy);
			break;
		case PE_UFP_VDM_Get_SVIDs_NAK:
			next_state = usbpd_policy_ufp_vdm_get_svids_nak(policy);
			break;
		case PE_UFP_VDM_Get_Modes:
			next_state = usbpd_policy_ufp_vdm_get_modes(policy);
			break;
		case PE_UFP_VDM_Send_Modes:
			next_state = usbpd_policy_ufp_vdm_send_modes(policy);
			break;
		case PE_UFP_VDM_Get_Modes_NAK:
			next_state = usbpd_policy_ufp_vdm_get_modes_nak(policy);
			break;
		case PE_UFP_VDM_Evaluate_Mode_Entry:
			next_state = usbpd_policy_ufp_vdm_evaluate_mode_entry(policy);
			break;
		case PE_UFP_VDM_Mode_Entry_ACK:
			next_state = usbpd_policy_ufp_vdm_mode_entry_ack(policy);
			break;
		case PE_UFP_VDM_Mode_Entry_NAK:
			next_state = usbpd_policy_ufp_vdm_mode_entry_nak(policy);
			break;
		case PE_UFP_VDM_Mode_Exit:
			next_state = usbpd_policy_ufp_vdm_mode_exit(policy);
			break;
		case PE_UFP_VDM_Mode_Exit_ACK:
			next_state = usbpd_policy_ufp_vdm_mode_exit_ack(policy);
			break;
		case PE_UFP_VDM_Mode_Exit_NAK:
			next_state = usbpd_policy_ufp_vdm_mode_exit_nak(policy);
			break;
		case PE_UFP_VDM_Attention_Request:
			next_state = usbpd_policy_ufp_vdm_attention_request(policy);
			break;
		case PE_UFP_VDM_Evaluate_Status:
			next_state = usbpd_policy_ufp_vdm_evaluate_status(policy);
			break;
		case PE_UFP_VDM_Status_ACK:
			next_state = usbpd_policy_ufp_vdm_status_ack(policy);
			break;
		case PE_UFP_VDM_Status_NAK:
			next_state = usbpd_policy_ufp_vdm_status_nak(policy);
			break;
		case PE_UFP_VDM_Evaluate_Configure:
			next_state = usbpd_policy_ufp_vdm_evaluate_configure(policy);
			break;
		case PE_UFP_VDM_Configure_ACK:
			next_state = usbpd_policy_ufp_vdm_configure_ack(policy);
			break;
		case PE_UFP_VDM_Configure_NAK:
			next_state = usbpd_policy_ufp_vdm_configure_nak(policy);
			break;
		case PE_DFP_VDM_EVALUATE:
			next_state = usbpd_policy_dfp_vdm_evaluate(policy);
			break;
		case PE_DFP_VDM_Identity_Request:
			next_state = usbpd_policy_dfp_vdm_identity_request(policy);
			break;
		case PE_DFP_VDM_Identity_ACKed:
			next_state = usbpd_policy_dfp_vdm_identity_acked(policy);
			break;
		case PE_DFP_VDM_Identity_NAKed:
			next_state = usbpd_policy_dfp_vdm_identity_naked(policy);
			break;
		case PE_DFP_VDM_SVIDs_Request:
			next_state = usbpd_policy_dfp_vdm_svids_request(policy);
			break;
		case PE_DFP_VDM_SVIDs_ACKed:
			next_state = usbpd_policy_dfp_vdm_svids_acked(policy);
			break;
		case PE_DFP_VDM_SVIDs_NAKed:
			next_state = usbpd_policy_dfp_vdm_svids_naked(policy);
			break;
		case PE_DFP_VDM_Modes_Request:
			next_state = usbpd_policy_dfp_vdm_modes_request(policy);
			break;
		case PE_DFP_VDM_Modes_ACKed:
			next_state = usbpd_policy_dfp_vdm_modes_acked(policy);
			break;
		case PE_DFP_VDM_Modes_NAKed:
			next_state = usbpd_policy_dfp_vdm_modes_naked(policy);
			break;
		case PE_DFP_VDM_Mode_Entry_Request:
			next_state = usbpd_policy_dfp_vdm_entry_request(policy);
			break;
		case PE_DFP_VDM_Mode_Entry_ACKed:
			next_state = usbpd_policy_dfp_vdm_entry_acked(policy);
			break;
		case PE_DFP_VDM_Mode_Entry_NAKed:
			next_state = usbpd_policy_dfp_vdm_entry_naked(policy);
			break;
		case PE_DFP_VDM_Mode_Exit_Request:
			next_state = usbpd_policy_dfp_vdm_exit_request(policy);
			break;
		case PE_DFP_VDM_Mode_Exit_ACKed:
			next_state = usbpd_policy_dfp_vdm_exit_acked(policy);
			break;
		case PE_DFP_VDM_Mode_Exit_NAKed:
			next_state = usbpd_policy_dfp_vdm_exit_naked(policy);
			break;
		case PE_DFP_VDM_Attention_Request:
			next_state = usbpd_policy_dfp_vdm_attention_request(policy);
			break;
		case PE_DFP_VDM_Status_Update:
			next_state = usbpd_policy_dfp_vdm_status_update(policy);
			break;
		case PE_DFP_VDM_Status_Update_ACKed:
			next_state = usbpd_policy_dfp_vdm_status_update_acked(policy);
			break;
		case PE_DFP_VDM_Status_Update_NAKed:
			next_state = usbpd_policy_dfp_vdm_status_update_naked(policy);
			break;
		case PE_DFP_VDM_DisplayPort_Configure:
			next_state = usbpd_policy_dfp_vdm_displayport_configure(policy);
			break;
		case PE_DFP_VDM_DisplayPort_Configure_ACKed:
			next_state = usbpd_policy_dfp_vdm_displayport_configure_acked(policy);
			break;
		case PE_DFP_VDM_DisplayPort_Configure_NAKed:
			next_state = usbpd_policy_dfp_vdm_displayport_configure_naked(policy);
			break;
		case PE_DFP_UVDM_Send_Message:
			next_state = usbpd_policy_dfp_uvdm_send_message(policy);
			break;
		case PE_DFP_UVDM_Receive_Message:
			next_state = usbpd_policy_dfp_uvdm_receive_message(policy);
			break;
		case PE_DR_SRC_Get_Source_Cap:
			next_state = usbpd_policy_dr_src_get_source_cap(policy);
			break;
		case PE_DR_SRC_Give_Sink_Cap:
			next_state = usbpd_policy_dr_src_give_sink_cap(policy);
			break;
		case PE_DR_SNK_Get_Sink_Cap:
			next_state = usbpd_policy_dr_snk_get_sink_cap(policy);
			break;
		case PE_DR_SNK_Give_Source_Cap:
			next_state = usbpd_policy_dr_snk_give_source_cap(policy);
			break;
		case PE_BIST_Carrier_Mode:
			next_state = usbpd_policy_bist_carrier_mode(policy);
			break;

		/* PD 3.0 */
		case PE_SRC_Send_Not_Supported:
			next_state = usbpd_policy_src_send_not_supported(policy);
			break;
		case PE_SRC_Not_Supported_Received:
			next_state = usbpd_policy_src_not_supported_received(policy);
			break;
		case PE_SRC_Chunk_Received:
			next_state = usbpd_policy_src_chunk_received(policy);
			break;

		case PE_SNK_Send_Not_Supported:
			next_state = usbpd_policy_snk_send_not_supported(policy);
			break;
		case PE_SNK_Not_Supported_Received:
			next_state = usbpd_policy_snk_not_supported_received(policy);
			break;
		case PE_SNK_Chunk_Received :
			next_state = usbpd_policy_snk_chunk_received(policy);
			break;

		case PE_SRC_Send_Source_Alert:
			next_state = usbpd_policy_src_send_source_alert(policy);
			break;
		case PE_SNK_Source_Alert_Received:
			next_state = usbpd_policy_snk_source_alert_received(policy);
			break;
		case PE_SNK_Send_Sink_Alert:
			next_state = usbpd_policy_snk_send_sink_alert(policy);
			break;
		case PE_SRC_Sink_Alert_Received:
			next_state = usbpd_policy_src_sink_alert_received(policy);
			break;
		case PE_SNK_Get_Source_Cap_Ext:
			next_state = usbpd_policy_snk_get_source_cap_ext(policy);
			break;
		case PE_SRC_Give_Source_Cap_Ext:
			next_state = usbpd_policy_src_give_source_cap_ext(policy);
			break;
		case PE_SNK_Get_Source_Status:
			next_state = usbpd_policy_snk_get_source_status(policy);
			break;
		case PE_SRC_Give_Source_Status:
			next_state = usbpd_policy_src_give_source_status(policy);
			break;
		case PE_SRC_Get_Sink_Status:
			next_state = usbpd_policy_src_get_sink_status(policy);
			break;
		case PE_SNK_Give_Sink_Status:
			next_state = usbpd_policy_snk_give_sink_status(policy);
			break;
		case PE_Get_Status:
			next_state = usbpd_policy_get_status(policy);
			break;
		case PE_Give_Status:
			next_state = usbpd_policy_give_status(policy);
			break;
		case PE_Give_Revision:
			next_state = usbpd_policy_give_revision(policy);
			break;
		case PE_DR_SNK_Give_Source_Info:
			next_state = usbpd_policy_dr_snk_give_source_info(policy);
			break;
		case PE_SRC_Give_Source_Info:
			next_state = usbpd_policy_src_give_source_info(policy);
			break;
		case PE_SNK_Get_PPS_Status:
			next_state = usbpd_policy_snk_get_pps_status(policy);
			break;
		case PE_SRC_Give_PPS_Status:
			next_state = usbpd_policy_src_give_pps_status(policy);
			break;
		case PE_Get_Battery_Cap:
			next_state = usbpd_policy_get_battery_cap(policy);
			break;
		case PE_Give_Battery_Cap:
			next_state = usbpd_policy_give_battery_cap(policy);
			break;
		case PE_Get_Battery_Status:
			next_state = usbpd_policy_get_battery_status(policy);
			break;
		case PE_Give_Battery_Status:
			next_state = usbpd_policy_give_battery_status(policy);
			break;
		case PE_Get_Manufacturer_Info:
			next_state = usbpd_policy_get_manufacturer_info(policy);
			break;
		case PE_Give_Manufacturer_Info:
			next_state = usbpd_policy_give_manufacturer_info(policy);
			break;
		case PE_Get_Country_Codes:
			next_state = usbpd_policy_get_country_codes(policy);
			break;
		case PE_Give_Country_Codes:
			next_state = usbpd_policy_give_country_codes(policy);
			break;
		case PE_Get_Country_Info:
			next_state = usbpd_policy_get_country_info(policy);
			break;
		case PE_Give_Country_Info:
			next_state = usbpd_policy_give_country_info(policy);
			break;
		case PE_Send_Security_Request:
			next_state = usbpd_policy_send_security_request(policy);
			break;
		case PE_Send_Security_Response:
			next_state = usbpd_policy_send_security_response(policy);
			break;
		case PE_Security_Response_Received:
			next_state = usbpd_policy_security_response_received(policy);
			break;
		case PE_Send_Firmware_Update_Request:
			next_state = usbpd_policy_send_firmware_update_request(policy);
			break;
		case PE_Send_Firmware_Update_Response:
			next_state = usbpd_policy_send_firmware_update_response(policy);
			break;
		case PE_Firmware_Update_Response_Received:
			next_state = usbpd_policy_firmware_update_response_received(policy);
			break;
		case PE_FRS_SRC_SNK_Evaluate_Swap:
			next_state = usbpd_policy_frs_src_snk_evaluate_swap(policy);
			break;
		case PE_FRS_SRC_SNK_Accept_Swap:
			next_state = usbpd_policy_frs_src_snk_accept_swap(policy);
			break;
		case PE_FRS_SRC_SNK_Transition_to_off:
			next_state = usbpd_policy_frs_src_snk_transition_to_off(policy);
			break;
		case PE_FRS_SRC_SNK_Assert_Rd:
			next_state = usbpd_policy_frs_src_snk_assert_rd(policy);
			break;
		case PE_FRS_SRC_SNK_Wait_Source_on:
			next_state = usbpd_policy_frs_src_snk_wait_source_on(policy);
			break;
		case PE_FRS_SNK_SRC_Start_AMS:
			next_state = usbpd_policy_frs_snk_src_start_ams(policy);
			break;
		case PE_FRS_SNK_SRC_Send_Swap:
			next_state = usbpd_policy_frs_snk_src_send_swap(policy);
			break;
		case PE_FRS_SNK_SRC_Transition_to_off:
			next_state = usbpd_policy_frs_snk_src_transition_to_off(policy);
			break;
		case PE_FRS_SNK_SRC_Vbus_Applied:
			next_state = usbpd_policy_frs_snk_src_vbus_applied(policy);
			break;
		case PE_FRS_SNK_SRC_Assert_Rp:
			next_state = usbpd_policy_frs_snk_src_assert_rp(policy);
			break;
		case PE_FRS_SNK_SRC_Source_on:
			next_state = usbpd_policy_frs_snk_src_source_on(policy);
			break;
		case PE_DR_Send_Reject:
			next_state = usbpd_policy_dr_send_reject(policy);
			break;
		case Error_Recovery:
			next_state = usbpd_error_recovery(policy);
			break;
		default:
			PDIC_OPS_PARAM_FUNC(get_power_role, pd_data, &power_role);
			usbpd_info("%s, %d\n", __func__, power_role);

			if (power_role == USBPD_SINK) {
				usbpd_info("%s, SINK\n", __func__);
				if (policy->rx_hardreset) {
					policy->rx_hardreset = 0;
					next_state = PE_SNK_Transition_to_default;
				} else if (policy->rx_softreset) {
					policy->rx_softreset = 0;
					next_state = PE_SNK_Soft_Reset;
				} else if (policy->plug) {
					policy->plug = 0;
					next_state = PE_SNK_Startup;
				} else {
					next_state = PE_SNK_Startup;
				}
			} else {
				usbpd_info("%s, SOURCE\n", __func__);
				if (policy->rx_hardreset) {
					policy->rx_hardreset = 0;
					next_state = PE_SRC_Hard_Reset_Received;
				} else if (policy->rx_softreset) {
					policy->rx_softreset = 0;
					next_state = PE_SRC_Soft_Reset;
				} else if (policy->plug) {
					policy->plug = 0;
					next_state = PE_SRC_Startup;
				} else {
					next_state = PE_SRC_Startup;
				}
			}
			break;
		}
		dev_info(pd_data->dev, "%s saved state %x next_state %x\n", __func__, saved_state, next_state);
	} while (saved_state != next_state);

	if (pd_data->is_prswap)
		PDIC_OPS_PARAM_FUNC(set_power_role, pd_data, USBPD_DRP);

	policy->state = next_state;
	__pm_relax(pd_data->policy_wake);
	dev_info(pd_data->dev, "%s Finished\n", __func__);
}
EXPORT_SYMBOL(usbpd_policy_work);

void usbpd_init_policy(struct usbpd_data *pd_data)
{
	int i;
	struct policy_data *policy = &pd_data->policy;
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG) && IS_ENABLED(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	struct usbpd_manager_data *manager = &pd_data->manager;	
#endif

	usbpd_info("%s\n", __func__);

	policy->state = 0;
	policy->rx_hardreset = 0;
	policy->rx_softreset = 0;
	policy->plug = 0;
	policy->rx_msg_header.word = 0;
	policy->tx_msg_header.word = 0;
	policy->modal_operation = 0;
	policy->sink_cap_received = 0;
	policy->send_sink_cap = 0;
	policy->txhardresetflag = 0;
	policy->pd_support = 0;
	policy->send_reject = 0;
	policy->cap_mismatch = 0;
	policy->pd_src_ready = 0;
	policy->got_pps_apdo = 0;
	policy->selected_pdo_type = 0;
	policy->selected_pdo_num = 0;
	policy->requested_pdo_type = 0;
	policy->requested_pdo_num = 0;
	policy->got_ufp_vdm = 0;
	policy->not_support_svid_ack = 0;
#if IS_ENABLED(CONFIG_PDIC_PD30)
	policy->pps_enable = 0;
#endif
#if IS_ENABLED(CONFIG_PDIC_PD30)
	PDIC_OPS_PARAM_FUNC(set_revision, pd_data, USBPD_PD3_0);
#else
	PDIC_OPS_PARAM_FUNC(set_revision, pd_data, USBPD_PD2_0);
#endif
	pd_data->pd_noti.sink_status.current_pdo_num = 0;
	pd_data->pd_noti.sink_status.selected_pdo_num = 0;
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG) && IS_ENABLED(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	manager->auth_type = AUTH_NONE;
#endif

	for (i = 0; i < USBPD_MAX_COUNT_MSG_OBJECT; i++) {
		policy->rx_data_obj[i].object = 0;
		policy->tx_data_obj[i].object = 0;
	}
}
EXPORT_SYMBOL(usbpd_init_policy);

void usbpd_kick_policy_work(struct device *dev)
{
	struct usbpd_data *pd_data = dev_get_drvdata(dev);

	if (pd_data->policy_wqueue)
		queue_work(pd_data->policy_wqueue, &pd_data->worker);
	else
		schedule_work(&pd_data->worker);
}
EXPORT_SYMBOL(usbpd_kick_policy_work);

void usbpd_cancel_policy_work(struct device *dev)
{
	struct usbpd_data *pd_data = dev_get_drvdata(dev);

	if (pd_data->policy_wqueue)
		flush_workqueue(pd_data->policy_wqueue);
}
EXPORT_SYMBOL(usbpd_cancel_policy_work);

