/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @brief File containing API definitions for the
 * HAL Layer of the Wi-Fi driver.
 */

#include "queue.h"
#include "common/hal_structs_common.h"
#include "common/hal_common.h"
#include "common/hal_reg.h"
#include "common/hal_mem.h"
#include "common/hal_interrupt.h"
#include "common/pal.h"

#ifdef NRF_WIFI_LOW_POWER
#ifdef NRF_WIFI_RPU_RECOVERY
static void did_rpu_had_sleep_opp(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	unsigned int deassert_time_diff_ms = nrf_wifi_osal_time_elapsed_ms(
		hal_dev_ctx->last_wakeup_now_deasserted_time_ms);

	if (deassert_time_diff_ms > NRF_WIFI_RPU_MIN_TIME_TO_ENTER_SLEEP_MS) {
		hal_dev_ctx->last_rpu_sleep_opp_time_ms =
			hal_dev_ctx->last_wakeup_now_deasserted_time_ms;
	}
}
#endif /* NRF_WIFI_RPU_RECOVERY */

enum nrf_wifi_status hal_rpu_ps_wake(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	unsigned int reg_val = 0;
	unsigned int rpu_ps_state_mask = 0;
	unsigned long start_time_us = 0;
	unsigned long idle_time_start_us = 0;
	unsigned long idle_time_us = 0;
	unsigned long elapsed_time_sec = 0;
	unsigned long elapsed_time_usec = 0;
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	if (!hal_dev_ctx) {
		nrf_wifi_osal_log_err("%s: Invalid parameters",
				      __func__);
		return status;
	}


	/* If the FW is not yet booted up (e.g. during the FW load stage of Host FW load)
	 * then skip the RPU wake attempt since RPU sleep/wake kicks in only after FW boot
	 */
	if (!hal_dev_ctx->rpu_fw_booted)
		return NRF_WIFI_STATUS_SUCCESS;

	if (hal_dev_ctx->rpu_ps_state == RPU_PS_STATE_AWAKE) {
		status = NRF_WIFI_STATUS_SUCCESS;

		goto out;
	}

	nrf_wifi_bal_rpu_ps_wake(hal_dev_ctx->bal_dev_ctx);
#ifdef NRF_WIFI_RPU_RECOVERY
	hal_dev_ctx->is_wakeup_now_asserted = true;
	hal_dev_ctx->last_wakeup_now_asserted_time_ms =
		nrf_wifi_osal_time_get_curr_ms();
#endif /* NRF_WIFI_RPU_RECOVERY */
	start_time_us = nrf_wifi_osal_time_get_curr_us();

	rpu_ps_state_mask = ((1 << RPU_REG_BIT_PS_STATE) |
			     (1 << RPU_REG_BIT_READY_STATE));

	/* Add a delay to avoid a race condition in the RPU */
	/* TODO: Reduce to 200 us after sleep has been stabilized */
	nrf_wifi_osal_delay_us(1000);

	do {
		/* Poll the RPU PS state */
		reg_val = nrf_wifi_bal_rpu_ps_status(hal_dev_ctx->bal_dev_ctx);

		if ((reg_val & rpu_ps_state_mask) == rpu_ps_state_mask) {
			status = NRF_WIFI_STATUS_SUCCESS;
			break;
		}

		idle_time_start_us = nrf_wifi_osal_time_get_curr_us();

		do {
			idle_time_us = nrf_wifi_osal_time_elapsed_us(idle_time_start_us);
		} while ((idle_time_us / 1000) < RPU_PS_WAKE_INTERVAL_MS);

		elapsed_time_usec = nrf_wifi_osal_time_elapsed_us(start_time_us);
		elapsed_time_sec = (elapsed_time_usec / 1000000);
	} while (elapsed_time_sec < RPU_PS_WAKE_TIMEOUT_S);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: RPU is not ready for more than %d sec,"
				      "reg_val = 0x%X rpu_ps_state_mask = 0x%X",
				      __func__,
				      RPU_PS_WAKE_TIMEOUT_S,
				      reg_val,
				      rpu_ps_state_mask);
#ifdef NRF_WIFI_RPU_RECOVERY
		nrf_wifi_osal_tasklet_schedule(hal_dev_ctx->recovery_tasklet);
#endif /* NRF_WIFI_RPU_RECOVERY */
		goto out;
	}
	hal_dev_ctx->rpu_ps_state = RPU_PS_STATE_AWAKE;
#ifdef NRF_WIFI_RPU_RECOVERY
	did_rpu_had_sleep_opp(hal_dev_ctx);
#endif /* NRF_WIFI_RPU_RECOVERY */
#ifdef NRF_WIFI_RPU_RECOVERY_PS_STATE_DEBUG
	nrf_wifi_osal_log_info("%s: RPU PS state is AWAKE\n",
			       __func__);
#endif /* NRF_WIFI_RPU_RECOVERY_PS_STATE_DEBUG */

out:

	nrf_wifi_osal_timer_schedule(hal_dev_ctx->rpu_ps_timer,
		NRF70_RPU_PS_IDLE_TIMEOUT_MS);
	return status;
}


static void hal_rpu_ps_sleep(unsigned long data)
{
	struct nrf_wifi_hal_dev_ctx *hal_dev_ctx = NULL;
	unsigned long flags = 0;

	hal_dev_ctx = (struct nrf_wifi_hal_dev_ctx *)data;

	nrf_wifi_osal_spinlock_irq_take(hal_dev_ctx->rpu_ps_lock,
					&flags);

	nrf_wifi_bal_rpu_ps_sleep(hal_dev_ctx->bal_dev_ctx);
#ifdef NRF_WIFI_RPU_RECOVERY
	hal_dev_ctx->is_wakeup_now_asserted = false;
	hal_dev_ctx->last_wakeup_now_deasserted_time_ms =
		nrf_wifi_osal_time_get_curr_ms();
#endif /* NRF_WIFI_RPU_RECOVERY */
	hal_dev_ctx->rpu_ps_state = RPU_PS_STATE_ASLEEP;

#ifdef NRF_WIFI_RPU_RECOVERY_PS_STATE_DEBUG
	nrf_wifi_osal_log_info("%s: RPU PS state is ASLEEP\n",
			       __func__);
#endif /* NRF_WIFI_RPU_RECOVERY_PS_STATE_DEBUG */
	nrf_wifi_osal_spinlock_irq_rel(hal_dev_ctx->rpu_ps_lock,
				       &flags);
}


enum nrf_wifi_status hal_rpu_ps_init(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	hal_dev_ctx->rpu_ps_lock = nrf_wifi_osal_spinlock_alloc();

	if (!hal_dev_ctx->rpu_ps_lock) {
		nrf_wifi_osal_log_err("%s: Unable to allocate lock",
				      __func__);
		goto out;
	}

	nrf_wifi_osal_spinlock_init(hal_dev_ctx->rpu_ps_lock);

	hal_dev_ctx->rpu_ps_timer = nrf_wifi_osal_timer_alloc();

	if (!hal_dev_ctx->rpu_ps_timer) {
		nrf_wifi_osal_log_err("%s: Unable to allocate timer",
				      __func__);
		nrf_wifi_osal_spinlock_free(hal_dev_ctx->rpu_ps_lock);
		goto out;
	}

	nrf_wifi_osal_timer_init(hal_dev_ctx->rpu_ps_timer,
				 hal_rpu_ps_sleep,
				 (unsigned long)hal_dev_ctx);

	hal_dev_ctx->rpu_ps_state = RPU_PS_STATE_ASLEEP;
	hal_dev_ctx->dbg_enable = true;

	status = NRF_WIFI_STATUS_SUCCESS;
out:
	return status;
}


static void hal_rpu_ps_deinit(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	nrf_wifi_osal_timer_kill(hal_dev_ctx->rpu_ps_timer);

	nrf_wifi_osal_timer_free(hal_dev_ctx->rpu_ps_timer);

	nrf_wifi_osal_spinlock_free(hal_dev_ctx->rpu_ps_lock);
}

enum nrf_wifi_status nrf_wifi_hal_get_rpu_ps_state(
				struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
				int *rpu_ps_ctrl_state)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	if (!hal_dev_ctx) {
		nrf_wifi_osal_log_err("%s: Invalid parameters",
				      __func__);
		goto out;
	}

	*rpu_ps_ctrl_state = hal_dev_ctx->rpu_ps_state;

	return NRF_WIFI_STATUS_SUCCESS;
out:
	return status;
}
#endif /* NRF_WIFI_LOW_POWER */


static bool hal_rpu_hpq_is_empty(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
				 struct host_rpu_hpq *hpq)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	unsigned int val = 0;

	status = hal_rpu_reg_read(hal_dev_ctx,
				  &val,
				  hpq->dequeue_addr);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Read from dequeue address failed, val (0x%X)",
				      __func__,
				      val);
		return true;
	}

	if (val) {
		return false;
	}

	return true;
}


static enum nrf_wifi_status hal_rpu_ready(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
					  enum NRF_WIFI_HAL_MSG_TYPE msg_type)
{
	bool is_empty = false;
	struct host_rpu_hpq *avl_buf_q = NULL;

	if (msg_type == NRF_WIFI_HAL_MSG_TYPE_CMD_CTRL) {
		avl_buf_q = &hal_dev_ctx->rpu_info.hpqm_info.cmd_avl_queue;
	} else {
		nrf_wifi_osal_log_err("%s: Invalid msg type %d",
				      __func__,
				      msg_type);

		return NRF_WIFI_STATUS_FAIL;
	}

	/* Check if any command pointers are available to post a message */
	is_empty = hal_rpu_hpq_is_empty(hal_dev_ctx,
					avl_buf_q);

	if (is_empty == true) {
		return NRF_WIFI_STATUS_FAIL;
	}

	return NRF_WIFI_STATUS_SUCCESS;
}


static enum nrf_wifi_status hal_rpu_ready_wait(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
					       enum NRF_WIFI_HAL_MSG_TYPE msg_type)
{
	unsigned long start_time_us = 0;
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	start_time_us = nrf_wifi_osal_time_get_curr_us();

	while (hal_rpu_ready(hal_dev_ctx, msg_type) != NRF_WIFI_STATUS_SUCCESS) {
		if (nrf_wifi_osal_time_elapsed_us(start_time_us) >= MAX_HAL_RPU_READY_WAIT) {
			nrf_wifi_osal_log_err("%s: Timed out waiting (msg_type = %d)",
					      __func__,
					      msg_type);
			goto out;
		}
	}

	status = NRF_WIFI_STATUS_SUCCESS;
out:
	return status;
}


static enum nrf_wifi_status hal_rpu_msg_trigger(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	status = hal_rpu_reg_write(hal_dev_ctx,
				   RPU_REG_INT_TO_MCU_CTRL,
				   (hal_dev_ctx->num_cmds | 0x7fff0000));

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Writing to MCU cmd register failed",
				      __func__);
		goto out;
	}

	hal_dev_ctx->num_cmds++;
out:
	return status;
}


enum nrf_wifi_status hal_rpu_msg_post(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
				      enum NRF_WIFI_HAL_MSG_TYPE msg_type,
				      unsigned int queue_id,
				      unsigned int msg_addr)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	struct host_rpu_hpq *busy_queue = NULL;

	if (queue_id >= MAX_NUM_OF_RX_QUEUES) {
		nrf_wifi_osal_log_err("%s: Invalid queue_id (%d)",
				      __func__,
				      queue_id);
		goto out;
	}

	if ((msg_type == NRF_WIFI_HAL_MSG_TYPE_CMD_CTRL) ||
	    (msg_type == NRF_WIFI_HAL_MSG_TYPE_CMD_DATA_TX)) {
		busy_queue = &hal_dev_ctx->rpu_info.hpqm_info.cmd_busy_queue;
	} else if (msg_type == NRF_WIFI_HAL_MSG_TYPE_CMD_DATA_RX) {
		busy_queue = &hal_dev_ctx->rpu_info.hpqm_info.rx_buf_busy_queue[queue_id];
	} else {
		nrf_wifi_osal_log_err("%s: Invalid msg_type (%d)",
				      __func__,
				      msg_type);
		goto out;
	}

	/* Copy the address, to which information was posted,
	 * to the busy queue.
	 */
	status = hal_rpu_hpq_enqueue(hal_dev_ctx,
				     busy_queue,
				     msg_addr);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Queueing of message to RPU failed",
				      __func__);
		goto out;
	}

	if (msg_type != NRF_WIFI_HAL_MSG_TYPE_CMD_DATA_RX) {
		/* Indicate to the RPU that the information has been posted */
		status = hal_rpu_msg_trigger(hal_dev_ctx);

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: Posting command to RPU failed",
					      __func__);
			goto out;
		}
	}
out:
	return status;
}


static enum nrf_wifi_status hal_rpu_msg_get_addr(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
						 enum NRF_WIFI_HAL_MSG_TYPE msg_type,
						 unsigned int *msg_addr)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	struct host_rpu_hpq *avl_queue = NULL;

	if (msg_type == NRF_WIFI_HAL_MSG_TYPE_CMD_CTRL) {
		avl_queue = &hal_dev_ctx->rpu_info.hpqm_info.cmd_avl_queue;
	} else {
		nrf_wifi_osal_log_err("%s: Invalid msg_type (%d)",
				      __func__,
				      msg_type);
		goto out;
	}

	status = hal_rpu_hpq_dequeue(hal_dev_ctx,
				     avl_queue,
				     msg_addr);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Dequeue of address failed msg_addr 0x%X",
				      __func__,
				      *msg_addr);
		*msg_addr = 0;
		goto out;
	}
out:
	return status;
}


static enum nrf_wifi_status hal_rpu_msg_write(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
					      enum NRF_WIFI_HAL_MSG_TYPE msg_type,
					      void *msg,
					      unsigned int len)
{
	unsigned int msg_addr = 0;
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	/* Get the address from the RPU to which
	 * the command needs to be copied to
	 */
	status = hal_rpu_msg_get_addr(hal_dev_ctx,
				      msg_type,
				      &msg_addr);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Getting address (0x%X) to post message failed",
				      __func__,
				      msg_addr);
		goto out;
	}

	/* Copy the information to the suggested address */
	status = hal_rpu_mem_write(hal_dev_ctx,
				   msg_addr,
				   msg,
				   len);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Copying information to RPU failed",
				      __func__);
		goto out;
	}

	/* Post the updated information to the RPU */
	status = hal_rpu_msg_post(hal_dev_ctx,
				  msg_type,
				  0,
				  msg_addr);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Posting command to RPU failed",
				      __func__);
		goto out;
	}

out:
	return status;
}


static enum nrf_wifi_status hal_rpu_cmd_process_queue(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	struct nrf_wifi_hal_msg *cmd = NULL;

	while ((cmd = nrf_wifi_utils_ctrl_q_dequeue(hal_dev_ctx->cmd_q))) {
		status = hal_rpu_ready_wait(hal_dev_ctx,
					    NRF_WIFI_HAL_MSG_TYPE_CMD_CTRL);

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: Timeout waiting to get free cmd buff from RPU",
					      __func__);
			nrf_wifi_osal_mem_free(cmd);
			cmd = NULL;
			continue;
		}

		status = hal_rpu_msg_write(hal_dev_ctx,
					   NRF_WIFI_HAL_MSG_TYPE_CMD_CTRL,
					   cmd->data,
					   cmd->len);

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: Writing command to RPU failed",
					      __func__);
			nrf_wifi_osal_mem_free(cmd);
			cmd = NULL;
			continue;
		}

		/* Free the command data and command */
		nrf_wifi_osal_mem_free(cmd);
		cmd = NULL;
	}

	return status;
}


static enum nrf_wifi_status hal_rpu_cmd_queue(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
					      void *cmd,
					      unsigned int cmd_size)
{
	int len = 0;
	int size = 0;
	char *data = NULL;
	struct nrf_wifi_hal_msg *hal_msg = NULL;
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	len = cmd_size;
	data = cmd;

	if (len > hal_dev_ctx->hpriv->cfg_params.max_cmd_size) {
		while (len > 0) {
			if (len > hal_dev_ctx->hpriv->cfg_params.max_cmd_size) {
				size = hal_dev_ctx->hpriv->cfg_params.max_cmd_size;
			} else {
				size = len;
			}

			hal_msg = nrf_wifi_osal_mem_zalloc(sizeof(*hal_msg) + size);

			if (!hal_msg) {
				nrf_wifi_osal_log_err("%s: Unable to alloc buff for frag HAL cmd",
						      __func__);
				status = NRF_WIFI_STATUS_FAIL;
				goto out;
			}

			nrf_wifi_osal_mem_cpy(hal_msg->data,
					      data,
					      size);

			hal_msg->len = size;

			status = nrf_wifi_utils_ctrl_q_enqueue(hal_dev_ctx->cmd_q,
							  hal_msg);

			if (status != NRF_WIFI_STATUS_SUCCESS) {
				nrf_wifi_osal_log_err("%s: Unable to queue frag HAL cmd",
						      __func__);
				goto out;
			}

			len -= size;
			data += size;
		}
	} else {
		hal_msg = nrf_wifi_osal_mem_zalloc(sizeof(*hal_msg) + len);

		if (!hal_msg) {
			nrf_wifi_osal_log_err("%s: Unable to allocate buffer for HAL command",
					      __func__);
			status = NRF_WIFI_STATUS_FAIL;
			goto out;
		}

		nrf_wifi_osal_mem_cpy(hal_msg->data,
				      cmd,
				      len);

		hal_msg->len = len;

		status = nrf_wifi_utils_ctrl_q_enqueue(hal_dev_ctx->cmd_q,
						  hal_msg);

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: Unable to queue fragmented command",
					      __func__);
			goto out;
		}
	}

	/* Free the original command data */
	nrf_wifi_osal_mem_free(cmd);

out:
	return status;
}


enum nrf_wifi_status nrf_wifi_hal_ctrl_cmd_send(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
						void *cmd,
						unsigned int cmd_size)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;


#ifdef CONFIG_NRF_WIFI_CMD_EVENT_LOG
	nrf_wifi_osal_log_info("%s: caller %p\n",
			      __func__,
			      __builtin_return_address(0));
#else
	nrf_wifi_osal_log_dbg("%s: caller %p\n",
			     __func__,
			     __builtin_return_address(0));
#endif
	nrf_wifi_osal_spinlock_take(hal_dev_ctx->lock_hal);

	status = hal_rpu_cmd_queue(hal_dev_ctx,
				   cmd,
				   cmd_size);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Queueing of command failed",
				      __func__);
		goto out;
	}

	status = hal_rpu_cmd_process_queue(hal_dev_ctx);

out:
	nrf_wifi_osal_spinlock_rel(hal_dev_ctx->lock_hal);

	return status;
}


enum nrf_wifi_status hal_rpu_eventq_process(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_SUCCESS;
	struct nrf_wifi_hal_msg *event = NULL;
	void *event_data = NULL;
	unsigned int event_len = 0;

	while (1) {
		event = nrf_wifi_utils_ctrl_q_dequeue(hal_dev_ctx->event_q);
		if (!event) {
			goto out;
		}

		event_data = event->data;
		event_len = event->len;

		/* Process the event further */
		status = hal_dev_ctx->hpriv->intr_callbk_fn(hal_dev_ctx->mac_dev_ctx,
							    event_data,
							    event_len);

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: Interrupt callback failed",
					      __func__);
		}

		/* Free up the local buffer */
		nrf_wifi_osal_mem_free(event);
		event = NULL;
	}

out:
	return status;
}

static void hal_rpu_eventq_drain(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	struct nrf_wifi_hal_msg *event = NULL;
	unsigned long flags = 0;

	while (1) {
		nrf_wifi_osal_spinlock_irq_take(hal_dev_ctx->lock_rx,
						&flags);

		event = nrf_wifi_utils_ctrl_q_dequeue(hal_dev_ctx->event_q);

		nrf_wifi_osal_spinlock_irq_rel(hal_dev_ctx->lock_rx,
					       &flags);

		if (!event) {
			goto out;
		}

		/* Free up the local buffer */
		nrf_wifi_osal_mem_free(event);
		event = NULL;
	}

out:
	return;
}

void nrf_wifi_hal_proc_ctx_set(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
			       enum RPU_PROC_TYPE proc)
{
	hal_dev_ctx->curr_proc = proc;
}


void nrf_wifi_hal_dev_rem(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	unsigned int i = 0;

	nrf_wifi_osal_tasklet_kill(hal_dev_ctx->recovery_tasklet);
	nrf_wifi_osal_tasklet_free(hal_dev_ctx->recovery_tasklet);
	nrf_wifi_osal_spinlock_free(hal_dev_ctx->lock_recovery);

	nrf_wifi_osal_tasklet_kill(hal_dev_ctx->event_tasklet);

	nrf_wifi_osal_tasklet_free(hal_dev_ctx->event_tasklet);

	hal_rpu_eventq_drain(hal_dev_ctx);

	nrf_wifi_osal_spinlock_free(hal_dev_ctx->lock_hal);
	nrf_wifi_osal_spinlock_free(hal_dev_ctx->lock_rx);

	nrf_wifi_utils_ctrl_q_free(hal_dev_ctx->event_q);

	nrf_wifi_utils_ctrl_q_free(hal_dev_ctx->cmd_q);

#ifdef NRF_WIFI_LOW_POWER
	hal_rpu_ps_deinit(hal_dev_ctx);
#endif /* NRF_WIFI_LOW_POWER */

	nrf_wifi_bal_dev_rem(hal_dev_ctx->bal_dev_ctx);

	nrf_wifi_osal_mem_free(hal_dev_ctx->tx_buf_info);
	hal_dev_ctx->tx_buf_info = NULL;

	for (i = 0; i < MAX_NUM_OF_RX_QUEUES; i++) {
		nrf_wifi_osal_mem_free(hal_dev_ctx->rx_buf_info[i]);
		hal_dev_ctx->rx_buf_info[i] = NULL;
	}

	hal_dev_ctx->hpriv->num_devs--;

	nrf_wifi_osal_mem_free(hal_dev_ctx);
}


enum nrf_wifi_status nrf_wifi_hal_dev_init(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

#ifdef NRF_WIFI_LOW_POWER
	hal_dev_ctx->rpu_fw_booted = true;
#endif /* NRF_WIFI_LOW_POWER */

	status = nrf_wifi_bal_dev_init(hal_dev_ctx->bal_dev_ctx);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: nrf_wifi_bal_dev_init failed",
				      __func__);
		goto out;
	}

	/* Read the HPQM info for all the queues provided by the RPU
	 * (like command, event, RX buf queues etc)
	 */
	status = hal_rpu_mem_read(hal_dev_ctx,
				  &hal_dev_ctx->rpu_info.hpqm_info,
				  RPU_MEM_HPQ_INFO,
				  sizeof(hal_dev_ctx->rpu_info.hpqm_info));

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Failed to get the HPQ info",
				      __func__);
		goto out;
	}

	status = hal_rpu_mem_read(hal_dev_ctx,
				  &hal_dev_ctx->rpu_info.rx_cmd_base,
				  RPU_MEM_RX_CMD_BASE,
				  sizeof(hal_dev_ctx->rpu_info.rx_cmd_base));

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Reading the RX cmd base failed",
				      __func__);
		goto out;
	}

	hal_dev_ctx->rpu_info.tx_cmd_base = RPU_MEM_TX_CMD_BASE;
	nrf_wifi_hal_enable(hal_dev_ctx);
out:
	return status;
}


void nrf_wifi_hal_dev_deinit(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	nrf_wifi_hal_disable(hal_dev_ctx);
	nrf_wifi_bal_dev_deinit(hal_dev_ctx->bal_dev_ctx);
	hal_rpu_eventq_drain(hal_dev_ctx);
}


enum nrf_wifi_status nrf_wifi_hal_irq_handler(void *data)
{
	struct nrf_wifi_hal_dev_ctx *hal_dev_ctx = NULL;
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	unsigned long flags = 0;
	bool do_rpu_recovery = false;

	hal_dev_ctx = (struct nrf_wifi_hal_dev_ctx *)data;

	nrf_wifi_osal_spinlock_irq_take(hal_dev_ctx->lock_rx,
					&flags);

	if (hal_dev_ctx->hal_status != NRF_WIFI_HAL_STATUS_ENABLED) {
		/* Ignore the interrupt if the HAL is not enabled */
		status = NRF_WIFI_STATUS_SUCCESS;
		goto out;
	}


	status = hal_rpu_irq_process(hal_dev_ctx, &do_rpu_recovery);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		goto out;
	}

	if (do_rpu_recovery) {
		nrf_wifi_osal_tasklet_schedule(hal_dev_ctx->recovery_tasklet);
		goto out;
	}

	nrf_wifi_osal_tasklet_schedule(hal_dev_ctx->event_tasklet);

out:
	nrf_wifi_osal_spinlock_irq_rel(hal_dev_ctx->lock_rx,
				       &flags);
	return status;
}


static int nrf_wifi_hal_poll_reg(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
				 unsigned int reg_addr,
				 unsigned int mask,
				 unsigned int req_value,
				 unsigned int poll_delay)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	unsigned int val = 0;
	unsigned int count = 50;

	do {
		status = hal_rpu_reg_read(hal_dev_ctx,
					  &val,
					  reg_addr);

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: Read from address (0x%X) failed, val (0x%X)",
					      __func__,
					      reg_addr,
					      val);
		}

		if ((val & mask) == req_value) {
			status = NRF_WIFI_STATUS_SUCCESS;
			break;
		}

		nrf_wifi_osal_sleep_ms(poll_delay);
	} while (count-- > 0);

	if (count == 0) {
		nrf_wifi_osal_log_err("%s: Timed out polling on (0x%X)",
				      __func__,
				      reg_addr);

		status = NRF_WIFI_STATUS_FAIL;
		goto out;
	}
out:
	return status;
}


/* Perform MIPS reset */
enum nrf_wifi_status nrf_wifi_hal_proc_reset(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
					     enum RPU_PROC_TYPE rpu_proc)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	if ((rpu_proc != RPU_PROC_TYPE_MCU_LMAC) &&
	    (rpu_proc != RPU_PROC_TYPE_MCU_UMAC)) {
		nrf_wifi_osal_log_err("%s: Unsupported RPU processor(%d)",
				      __func__,
				      rpu_proc);
		goto out;
	}

	hal_dev_ctx->curr_proc = rpu_proc;

	/* Perform pulsed soft reset of MIPS */
	if (rpu_proc == RPU_PROC_TYPE_MCU_LMAC) {
		status = hal_rpu_reg_write(hal_dev_ctx,
					   RPU_REG_MIPS_MCU_CONTROL,
					   0x1);
	} else {
		status = hal_rpu_reg_write(hal_dev_ctx,
					   RPU_REG_MIPS_MCU2_CONTROL,
					   0x1);
	}

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Pulsed soft reset of MCU failed for (%d) processor",
				      __func__,
				      rpu_proc);
		goto out;
	}


	/* Wait for it to come out of reset */
	if (rpu_proc == RPU_PROC_TYPE_MCU_LMAC) {
		status = nrf_wifi_hal_poll_reg(hal_dev_ctx,
					       RPU_REG_MIPS_MCU_CONTROL,
					       0x1,
					       0,
					       10);
	} else {
		status = nrf_wifi_hal_poll_reg(hal_dev_ctx,
					       RPU_REG_MIPS_MCU2_CONTROL,
					       0x1,
					       0,
					       10);
	}

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: MCU (%d) failed to come out of reset",
				      __func__,
				      rpu_proc);
		goto out;
	}

	/* MIPS will restart from it's boot exception registers
	 * and hit its default wait instruction
	 */
	if (rpu_proc == RPU_PROC_TYPE_MCU_LMAC) {
		status = nrf_wifi_hal_poll_reg(hal_dev_ctx,
					       0xA4000018,
					       0x1,
					       0x1,
					       10);
	} else {
		status = nrf_wifi_hal_poll_reg(hal_dev_ctx,
					       0xA4000118,
					       0x1,
					       0x1,
					       10);
	}
out:
	hal_dev_ctx->curr_proc = RPU_PROC_TYPE_MCU_LMAC;
	return status;
}

#define MCU_FW_BOOT_TIMEOUT_MS 1000
enum nrf_wifi_status nrf_wifi_hal_fw_chk_boot(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
					      enum RPU_PROC_TYPE rpu_proc)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	unsigned int addr = 0;
	unsigned int val = 0;
	unsigned int exp_val = 0;
	int mcu_ready_wait_count = MCU_FW_BOOT_TIMEOUT_MS / 10;

	if (rpu_proc == RPU_PROC_TYPE_MCU_LMAC) {
		addr = RPU_MEM_LMAC_BOOT_SIG;
		exp_val = NRF_WIFI_LMAC_BOOT_SIG;
	} else if (rpu_proc == RPU_PROC_TYPE_MCU_UMAC) {
		addr = RPU_MEM_UMAC_BOOT_SIG;
		exp_val = NRF_WIFI_UMAC_BOOT_SIG;
	} else {
		nrf_wifi_osal_log_err("%s: Invalid RPU processor (%d)",
				      __func__,
				      rpu_proc);
	}

	hal_dev_ctx->curr_proc = rpu_proc;

	while (mcu_ready_wait_count-- > 0) {
		status = hal_rpu_mem_read(hal_dev_ctx,
					  (unsigned char *)&val,
					  addr,
					  sizeof(val));

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: Reading of boot signature failed for RPU(%d)",
					      __func__,
					      rpu_proc);
		}

		if (val == exp_val) {
			break;
		}

		/* Sleep for 10 ms */
		nrf_wifi_osal_sleep_ms(10);
	};

	if (mcu_ready_wait_count <= 0) {
		nrf_wifi_osal_log_err("%s: Boot_sig check failed for RPU(%d), "
				      "Expected: 0x%X, Actual: 0x%X",
				      __func__,
				      rpu_proc,
				      exp_val,
				      val);
		status = NRF_WIFI_STATUS_FAIL;
		goto out;
	}

	status = NRF_WIFI_STATUS_SUCCESS;
out:
	hal_dev_ctx->curr_proc = RPU_PROC_TYPE_MCU_LMAC;

	return status;
}


struct nrf_wifi_hal_priv *
nrf_wifi_hal_init(struct nrf_wifi_hal_cfg_params *cfg_params,
		  enum nrf_wifi_status (*intr_callbk_fn)(void *dev_ctx,
							 void *event_data,
							 unsigned int len),
		  enum nrf_wifi_status (*rpu_recovery_callbk_fn)(void *mac_ctx,
								 void *event_data,
								 unsigned int len))
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	struct nrf_wifi_hal_priv *hpriv = NULL;
	struct nrf_wifi_bal_cfg_params bal_cfg_params;

	hpriv = nrf_wifi_osal_mem_zalloc(sizeof(*hpriv));

	if (!hpriv) {
		nrf_wifi_osal_log_err("%s: Unable to allocate memory for hpriv",
				      __func__);
		goto out;
	}

	nrf_wifi_osal_mem_cpy(&hpriv->cfg_params,
			      cfg_params,
			      sizeof(hpriv->cfg_params));

	hpriv->intr_callbk_fn = intr_callbk_fn;
	hpriv->rpu_recovery_callbk_fn = rpu_recovery_callbk_fn;

	status = pal_rpu_addr_offset_get(RPU_ADDR_PKTRAM_START,
					 &hpriv->addr_pktram_base,
					 RPU_PROC_TYPE_MAX);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: pal_rpu_addr_offset_get failed",
				      __func__);
		goto out;
	}

	bal_cfg_params.addr_pktram_base = hpriv->addr_pktram_base;

	hpriv->bpriv = nrf_wifi_bal_init(&bal_cfg_params,
					 &nrf_wifi_hal_irq_handler);

	if (!hpriv->bpriv) {
		nrf_wifi_osal_log_err("%s: Failed",
				      __func__);
		nrf_wifi_osal_mem_free(hpriv);
		hpriv = NULL;
	}
out:
	return hpriv;
}


void nrf_wifi_hal_deinit(struct nrf_wifi_hal_priv *hpriv)
{
	nrf_wifi_bal_deinit(hpriv->bpriv);

	nrf_wifi_osal_mem_free(hpriv);
}


enum nrf_wifi_status nrf_wifi_hal_otp_info_get(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
					       struct host_rpu_umac_info *otp_info,
					       unsigned int *otp_flags)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	if (!hal_dev_ctx || !otp_info) {
		nrf_wifi_osal_log_err("%s: Invalid parameters",
				      __func__);
		goto out;
	}

	status = hal_rpu_mem_read(hal_dev_ctx,
				  otp_info,
				  RPU_MEM_UMAC_BOOT_SIG,
				  sizeof(*otp_info));

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: OTP info get failed",
				      __func__);
		goto out;
	}

	status = hal_rpu_mem_read(hal_dev_ctx,
				  otp_flags,
				  RPU_MEM_OTP_INFO_FLAGS,
				  sizeof(*otp_flags));

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: OTP flags get failed",
				      __func__);
		goto out;
	}
out:
	return status;
}


enum nrf_wifi_status nrf_wifi_hal_otp_ft_prog_ver_get(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
						      unsigned int *ft_prog_ver)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	if (!hal_dev_ctx || !ft_prog_ver) {
		nrf_wifi_osal_log_err("%s: Invalid parameters",
				      __func__);
		goto out;
	}

	status = hal_rpu_mem_read(hal_dev_ctx,
				  ft_prog_ver,
				  RPU_MEM_OTP_FT_PROG_VERSION,
				  sizeof(*ft_prog_ver));

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: FT program version get failed",
				      __func__);
		goto out;
	}
out:
	return status;
}

enum nrf_wifi_status nrf_wifi_hal_otp_pack_info_get(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx,
						    unsigned int *package_info)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

	if (!hal_dev_ctx || !package_info) {
		nrf_wifi_osal_log_err("%s: Invalid parameters",
				      __func__);
		goto out;
	}

	status = hal_rpu_mem_read(hal_dev_ctx,
				  package_info,
				  RPU_MEM_OTP_PACKAGE_TYPE,
				  sizeof(*package_info));

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: Package info get failed",
				      __func__);
		goto out;
	}
out:
	return status;
}

void nrf_wifi_hal_enable(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	nrf_wifi_osal_spinlock_irq_take(hal_dev_ctx->lock_rx,
					NULL);
	hal_dev_ctx->hal_status = NRF_WIFI_HAL_STATUS_ENABLED;
	nrf_wifi_osal_spinlock_irq_rel(hal_dev_ctx->lock_rx,
				       NULL);
}

void nrf_wifi_hal_disable(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	nrf_wifi_osal_spinlock_irq_take(hal_dev_ctx->lock_rx,
					NULL);
	hal_dev_ctx->hal_status = NRF_WIFI_HAL_STATUS_DISABLED;
	nrf_wifi_osal_spinlock_irq_rel(hal_dev_ctx->lock_rx,
				       NULL);
}

enum NRF_WIFI_HAL_STATUS nrf_wifi_hal_status_unlocked(struct nrf_wifi_hal_dev_ctx *hal_dev_ctx)
{
	return hal_dev_ctx->hal_status;
}
