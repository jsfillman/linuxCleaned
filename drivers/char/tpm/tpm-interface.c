/*
 * Copyright (C) 2004 IBM Corporation
 * Copyright (C) 2014 Intel Corporation
 *
 * Authors:
 * Leendert van Doorn <leendert@watson.ibm.com>
 * Dave Safford <safford@watson.ibm.com>
 * Reiner Sailer <sailer@watson.ibm.com>
 * Kylene Hall <kjhall@us.ibm.com>
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * Device driver for TCG/TCPA TPM (trusted platform module).
 * Specifications at www.trustedcomputinggroup.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * Note, the TPM chip is not interrupt driven (only polling)
 * and can have very long timeouts (minutes!). Hence the unusual
 * calls to msleep.
 *
 */

#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/freezer.h>
#include <linux/tpm_eventlog.h>

#include "tpm.h"

/*
 * Bug workaround - some TPM's don't flush the most
 * recently changed pcr on suspend, so force the flush
 * with an extend to the selected _unused_ non-volatile pcr.
 */
static u32 tpm_suspend_pcr;
module_param_named(suspend_pcr, tpm_suspend_pcr, uint, 0644);
MODULE_PARM_DESC(suspend_pcr,
		 "PCR to use for dummy writes to facilitate flush on suspend.");

/**
 * tpm_calc_ordinal_duration() - calculate the maximum command duration
 * @chip:    TPM chip to use.
 * @ordinal: TPM command ordinal.
 *
 * The function returns the maximum amount of time the chip could take
 * to return the result for a particular ordinal in jiffies.
 *
 * Return: A maximal duration time for an ordinal in jiffies.
 */
unsigned long tpm_calc_ordinal_duration(struct tpm_chip *chip, u32 ordinal)
{
	if (chip->flags & TPM_CHIP_FLAG_TPM2)
		return tpm2_calc_ordinal_duration(chip, ordinal);
	else
		return tpm1_calc_ordinal_duration(chip, ordinal);
}
EXPORT_SYMBOL_GPL(tpm_calc_ordinal_duration);

static int tpm_validate_command(struct tpm_chip *chip,
				 struct tpm_space *space,
				 const u8 *cmd,
				 size_t len)
{
	const struct tpm_input_header *header = (const void *)cmd;
	int i;
	u32 cc;
	u32 attrs;
	unsigned int nr_handles;

	if (len < TPM_HEADER_SIZE)
		return -EINVAL;

	if (!space)
		return 0;

	if (chip->flags & TPM_CHIP_FLAG_TPM2 && chip->nr_commands) {
		cc = be32_to_cpu(header->ordinal);

		i = tpm2_find_cc(chip, cc);
		if (i < 0) {
			dev_dbg(&chip->dev, "0x%04X is an invalid command\n",
				cc);
			return -EOPNOTSUPP;
		}

		attrs = chip->cc_attrs_tbl[i];
		nr_handles =
			4 * ((attrs >> TPM2_CC_ATTR_CHANDLES) & GENMASK(2, 0));
		if (len < TPM_HEADER_SIZE + 4 * nr_handles)
			goto err_len;
	}

	return 0;
err_len:
	dev_dbg(&chip->dev,
		"%s: insufficient command length %zu", __func__, len);
	return -EINVAL;
}

static int tpm_request_locality(struct tpm_chip *chip, unsigned int flags)
{
	int rc;

	if (flags & TPM_TRANSMIT_NESTED)
		return 0;

	if (!chip->ops->request_locality)
		return 0;

	rc = chip->ops->request_locality(chip, 0);
	if (rc < 0)
		return rc;

	chip->locality = rc;

	return 0;
}

static void tpm_relinquish_locality(struct tpm_chip *chip, unsigned int flags)
{
	int rc;

	if (flags & TPM_TRANSMIT_NESTED)
		return;

	if (!chip->ops->relinquish_locality)
		return;

	rc = chip->ops->relinquish_locality(chip, chip->locality);
	if (rc)
		dev_err(&chip->dev, "%s: : error %d\n", __func__, rc);

	chip->locality = -1;
}

static int tpm_cmd_ready(struct tpm_chip *chip, unsigned int flags)
{
	if (flags & TPM_TRANSMIT_NESTED)
		return 0;

	if (!chip->ops->cmd_ready)
		return 0;

	return chip->ops->cmd_ready(chip);
}

static int tpm_go_idle(struct tpm_chip *chip, unsigned int flags)
{
	if (flags & TPM_TRANSMIT_NESTED)
		return 0;

	if (!chip->ops->go_idle)
		return 0;

	return chip->ops->go_idle(chip);
}

static ssize_t tpm_try_transmit(struct tpm_chip *chip,
				struct tpm_space *space,
				u8 *buf, size_t bufsiz,
				unsigned int flags)
{
	struct tpm_output_header *header = (void *)buf;
	int rc;
	ssize_t len = 0;
	u32 count, ordinal;
	unsigned long stop;
	bool need_locality;

	rc = tpm_validate_command(chip, space, buf, bufsiz);
	if (rc == -EINVAL)
		return rc;
	/*
	 * If the command is not implemented by the TPM, synthesize a
	 * response with a TPM2_RC_COMMAND_CODE return for user-space.
	 */
	if (rc == -EOPNOTSUPP) {
		header->length = cpu_to_be32(sizeof(*header));
		header->tag = cpu_to_be16(TPM2_ST_NO_SESSIONS);
		header->return_code = cpu_to_be32(TPM2_RC_COMMAND_CODE |
						  TSS2_RESMGR_TPM_RC_LAYER);
		return sizeof(*header);
	}

	if (bufsiz > TPM_BUFSIZE)
		bufsiz = TPM_BUFSIZE;

	count = be32_to_cpu(*((__be32 *) (buf + 2)));
	ordinal = be32_to_cpu(*((__be32 *) (buf + 6)));
	if (count == 0)
		return -ENODATA;
	if (count > bufsiz) {
		dev_err(&chip->dev,
			"invalid count value %x %zx\n", count, bufsiz);
		return -E2BIG;
	}

	if (!(flags & TPM_TRANSMIT_UNLOCKED) && !(flags & TPM_TRANSMIT_NESTED))
		mutex_lock(&chip->tpm_mutex);

	if (chip->ops->clk_enable != NULL)
		chip->ops->clk_enable(chip, true);

	/* Store the decision as chip->locality will be changed. */
	need_locality = chip->locality == -1;

	if (need_locality) {
		rc = tpm_request_locality(chip, flags);
		if (rc < 0) {
			need_locality = false;
			goto out_locality;
		}
	}

	rc = tpm_cmd_ready(chip, flags);
	if (rc)
		goto out_locality;

	rc = tpm2_prepare_space(chip, space, ordinal, buf);
	if (rc)
		goto out;

	rc = chip->ops->send(chip, buf, count);
	if (rc < 0) {
		if (rc != -EPIPE)
			dev_err(&chip->dev,
				"%s: tpm_send: error %d\n", __func__, rc);
		goto out;
	}

	if (chip->flags & TPM_CHIP_FLAG_IRQ)
		goto out_recv;

	stop = jiffies + tpm_calc_ordinal_duration(chip, ordinal);
	do {
		u8 status = chip->ops->status(chip);
		if ((status & chip->ops->req_complete_mask) ==
		    chip->ops->req_complete_val)
			goto out_recv;

		if (chip->ops->req_canceled(chip, status)) {
			dev_err(&chip->dev, "Operation Canceled\n");
			rc = -ECANCELED;
			goto out;
		}

		tpm_msleep(TPM_TIMEOUT_POLL);
		rmb();
	} while (time_before(jiffies, stop));

	chip->ops->cancel(chip);
	dev_err(&chip->dev, "Operation Timed out\n");
	rc = -ETIME;
	goto out;

out_recv:
	len = chip->ops->recv(chip, buf, bufsiz);
	if (len < 0) {
		rc = len;
		dev_err(&chip->dev,
			"tpm_transmit: tpm_recv: error %d\n", rc);
		goto out;
	} else if (len < TPM_HEADER_SIZE) {
		rc = -EFAULT;
		goto out;
	}

	if (len != be32_to_cpu(header->length)) {
		rc = -EFAULT;
		goto out;
	}

	rc = tpm2_commit_space(chip, space, ordinal, buf, &len);
	if (rc)
		dev_err(&chip->dev, "tpm2_commit_space: error %d\n", rc);

out:
	/* may fail but do not override previous error value in rc */
	tpm_go_idle(chip, flags);

out_locality:
	if (need_locality)
		tpm_relinquish_locality(chip, flags);

	if (chip->ops->clk_enable != NULL)
		chip->ops->clk_enable(chip, false);

	if (!(flags & TPM_TRANSMIT_UNLOCKED) && !(flags & TPM_TRANSMIT_NESTED))
		mutex_unlock(&chip->tpm_mutex);
	return rc ? rc : len;
}

/**
 * tpm_transmit - Internal kernel interface to transmit TPM commands.
 *
 * @chip: TPM chip to use
 * @space: tpm space
 * @buf: TPM command buffer
 * @bufsiz: length of the TPM command buffer
 * @flags: tpm transmit flags - bitmap
 *
 * A wrapper around tpm_try_transmit that handles TPM2_RC_RETRY
 * returns from the TPM and retransmits the command after a delay up
 * to a maximum wait of TPM2_DURATION_LONG.
 *
 * Note: TPM1 never returns TPM2_RC_RETRY so the retry logic is TPM2
 * only
 *
 * Return:
 *     the length of the return when the operation is successful.
 *     A negative number for system errors (errno).
 */
ssize_t tpm_transmit(struct tpm_chip *chip, struct tpm_space *space,
		     u8 *buf, size_t bufsiz, unsigned int flags)
{
	struct tpm_output_header *header = (struct tpm_output_header *)buf;
	/* space for header and handles */
	u8 save[TPM_HEADER_SIZE + 3*sizeof(u32)];
	unsigned int delay_msec = TPM2_DURATION_SHORT;
	u32 rc = 0;
	ssize_t ret;
	const size_t save_size = min(space ? sizeof(save) : TPM_HEADER_SIZE,
				     bufsiz);
	/* the command code is where the return code will be */
	u32 cc = be32_to_cpu(header->return_code);

	/*
	 * Subtlety here: if we have a space, the handles will be
	 * transformed, so when we restore the header we also have to
	 * restore the handles.
	 */
	memcpy(save, buf, save_size);

	for (;;) {
		ret = tpm_try_transmit(chip, space, buf, bufsiz, flags);
		if (ret < 0)
			break;
		rc = be32_to_cpu(header->return_code);
		if (rc != TPM2_RC_RETRY && rc != TPM2_RC_TESTING)
			break;
		/*
		 * return immediately if self test returns test
		 * still running to shorten boot time.
		 */
		if (rc == TPM2_RC_TESTING && cc == TPM2_CC_SELF_TEST)
			break;

		if (delay_msec > TPM2_DURATION_LONG) {
			if (rc == TPM2_RC_RETRY)
				dev_err(&chip->dev, "in retry loop\n");
			else
				dev_err(&chip->dev,
					"self test is still running\n");
			break;
		}
		tpm_msleep(delay_msec);
		delay_msec *= 2;
		memcpy(buf, save, save_size);
	}
	return ret;
}
/**
 * tpm_transmit_cmd - send a tpm command to the device
 *    The function extracts tpm out header return code
 *
 * @chip: TPM chip to use
 * @space: tpm space
 * @buf: TPM command buffer
 * @bufsiz: length of the buffer
 * @min_rsp_body_length: minimum expected length of response body
 * @flags: tpm transmit flags - bitmap
 * @desc: command description used in the error message
 *
 * Return:
 *     0 when the operation is successful.
 *     A negative number for system errors (errno).
 *     A positive number for a TPM error.
 */
ssize_t tpm_transmit_cmd(struct tpm_chip *chip, struct tpm_space *space,
			 void *buf, size_t bufsiz,
			 size_t min_rsp_body_length, unsigned int flags,
			 const char *desc)
{
	const struct tpm_output_header *header = buf;
	int err;
	ssize_t len;

	len = tpm_transmit(chip, space, buf, bufsiz, flags);
	if (len <  0)
		return len;

	err = be32_to_cpu(header->return_code);
	if (err != 0 && err != TPM_ERR_DISABLED && err != TPM_ERR_DEACTIVATED
	    && desc)
		dev_err(&chip->dev, "A TPM error (%d) occurred %s\n", err,
			desc);
	if (err)
		return err;

	if (len < min_rsp_body_length + TPM_HEADER_SIZE)
		return -EFAULT;

	return 0;
}
EXPORT_SYMBOL_GPL(tpm_transmit_cmd);

int tpm_get_timeouts(struct tpm_chip *chip)
{
	if (chip->flags & TPM_CHIP_FLAG_HAVE_TIMEOUTS)
		return 0;

	if (chip->flags & TPM_CHIP_FLAG_TPM2)
		return tpm2_get_timeouts(chip);
	else
		return tpm1_get_timeouts(chip);
}
EXPORT_SYMBOL_GPL(tpm_get_timeouts);

/**
 * tpm_is_tpm2 - do we a have a TPM2 chip?
 * @chip:	a &struct tpm_chip instance, %NULL for the default chip
 *
 * Return:
 * 1 if we have a TPM2 chip.
 * 0 if we don't have a TPM2 chip.
 * A negative number for system errors (errno).
 */
int tpm_is_tpm2(struct tpm_chip *chip)
{
	int rc;

	chip = tpm_find_get_ops(chip);
	if (!chip)
		return -ENODEV;

	rc = (chip->flags & TPM_CHIP_FLAG_TPM2) != 0;

	tpm_put_ops(chip);

	return rc;
}
EXPORT_SYMBOL_GPL(tpm_is_tpm2);

/**
 * tpm_pcr_read - read a PCR value from SHA1 bank
 * @chip:	a &struct tpm_chip instance, %NULL for the default chip
 * @pcr_idx:	the PCR to be retrieved
 * @res_buf:	the value of the PCR
 *
 * Return: same as with tpm_transmit_cmd()
 */
int tpm_pcr_read(struct tpm_chip *chip, u32 pcr_idx, u8 *res_buf)
{
	int rc;

	chip = tpm_find_get_ops(chip);
	if (!chip)
		return -ENODEV;

	if (chip->flags & TPM_CHIP_FLAG_TPM2)
		rc = tpm2_pcr_read(chip, pcr_idx, res_buf);
	else
		rc = tpm1_pcr_read(chip, pcr_idx, res_buf);

	tpm_put_ops(chip);
	return rc;
}
EXPORT_SYMBOL_GPL(tpm_pcr_read);

/**
 * tpm_pcr_extend - extend a PCR value in SHA1 bank.
 * @chip:	a &struct tpm_chip instance, %NULL for the default chip
 * @pcr_idx:	the PCR to be retrieved
 * @hash:	the hash value used to extend the PCR value
 *
 * Note: with TPM 2.0 extends also those banks with a known digest size to the
 * cryto subsystem in order to prevent malicious use of those PCR banks. In the
 * future we should dynamically determine digest sizes.
 *
 * Return: same as with tpm_transmit_cmd()
 */
int tpm_pcr_extend(struct tpm_chip *chip, u32 pcr_idx, const u8 *hash)
{
	int rc;
	struct tpm2_digest digest_list[ARRAY_SIZE(chip->active_banks)];
	u32 count = 0;
	int i;

	chip = tpm_find_get_ops(chip);
	if (!chip)
		return -ENODEV;

	if (chip->flags & TPM_CHIP_FLAG_TPM2) {
		memset(digest_list, 0, sizeof(digest_list));

		for (i = 0; i < ARRAY_SIZE(chip->active_banks) &&
			    chip->active_banks[i] != TPM2_ALG_ERROR; i++) {
			digest_list[i].alg_id = chip->active_banks[i];
			memcpy(digest_list[i].digest, hash, TPM_DIGEST_SIZE);
			count++;
		}

		rc = tpm2_pcr_extend(chip, pcr_idx, count, digest_list);
		tpm_put_ops(chip);
		return rc;
	}

	rc = tpm1_pcr_extend(chip, pcr_idx, hash,
			     "attempting extend a PCR value");
	tpm_put_ops(chip);
	return rc;
}
EXPORT_SYMBOL_GPL(tpm_pcr_extend);

/**
 * tpm_send - send a TPM command
 * @chip:	a &struct tpm_chip instance, %NULL for the default chip
 * @cmd:	a TPM command buffer
 * @buflen:	the length of the TPM command buffer
 *
 * Return: same as with tpm_transmit_cmd()
 */
int tpm_send(struct tpm_chip *chip, void *cmd, size_t buflen)
{
	int rc;

	chip = tpm_find_get_ops(chip);
	if (!chip)
		return -ENODEV;

	rc = tpm_transmit_cmd(chip, NULL, cmd, buflen, 0, 0,
			      "attempting to a send a command");
	tpm_put_ops(chip);
	return rc;
}
EXPORT_SYMBOL_GPL(tpm_send);

int tpm_auto_startup(struct tpm_chip *chip)
{
	int rc;

	if (!(chip->ops->flags & TPM_OPS_AUTO_STARTUP))
		return 0;

	if (chip->flags & TPM_CHIP_FLAG_TPM2)
		rc = tpm2_auto_startup(chip);
	else
		rc = tpm1_auto_startup(chip);

	return rc;
}

/*
 * We are about to suspend. Save the TPM state
 * so that it can be restored.
 */
int tpm_pm_suspend(struct device *dev)
{
	struct tpm_chip *chip = dev_get_drvdata(dev);
	int rc = 0;

	if (!chip)
		return -ENODEV;

	if (chip->flags & TPM_CHIP_FLAG_ALWAYS_POWERED)
		return 0;

	if (chip->flags & TPM_CHIP_FLAG_TPM2)
		tpm2_shutdown(chip, TPM2_SU_STATE);
	else
		rc = tpm1_pm_suspend(chip, tpm_suspend_pcr);

	return rc;
}
EXPORT_SYMBOL_GPL(tpm_pm_suspend);

/*
 * Resume from a power safe. The BIOS already restored
 * the TPM state.
 */
int tpm_pm_resume(struct device *dev)
{
	struct tpm_chip *chip = dev_get_drvdata(dev);

	if (chip == NULL)
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL_GPL(tpm_pm_resume);

/**
 * tpm_get_random() - get random bytes from the TPM's RNG
 * @chip:	a &struct tpm_chip instance, %NULL for the default chip
 * @out:	destination buffer for the random bytes
 * @max:	the max number of bytes to write to @out
 *
 * Return: number of random bytes read or a negative error value.
 */
int tpm_get_random(struct tpm_chip *chip, u8 *out, size_t max)
{
	int rc;

	if (!out || max > TPM_MAX_RNG_DATA)
		return -EINVAL;

	chip = tpm_find_get_ops(chip);
	if (!chip)
		return -ENODEV;

	if (chip->flags & TPM_CHIP_FLAG_TPM2)
		rc = tpm2_get_random(chip, out, max);
	else
		rc = tpm1_get_random(chip, out, max);

	tpm_put_ops(chip);
	return rc;
}
EXPORT_SYMBOL_GPL(tpm_get_random);

/**
 * tpm_seal_trusted() - seal a trusted key payload
 * @chip:	a &struct tpm_chip instance, %NULL for the default chip
 * @options:	authentication values and other options
 * @payload:	the key data in clear and encrypted form
 *
 * Note: only TPM 2.0 chip are supported. TPM 1.x implementation is located in
 * the keyring subsystem.
 *
 * Return: same as with tpm_transmit_cmd()
 */
int tpm_seal_trusted(struct tpm_chip *chip, struct trusted_key_payload *payload,
		     struct trusted_key_options *options)
{
	int rc;

	chip = tpm_find_get_ops(chip);
	if (!chip || !(chip->flags & TPM_CHIP_FLAG_TPM2))
		return -ENODEV;

	rc = tpm2_seal_trusted(chip, payload, options);

	tpm_put_ops(chip);
	return rc;
}
EXPORT_SYMBOL_GPL(tpm_seal_trusted);

/**
 * tpm_unseal_trusted() - unseal a trusted key
 * @chip:	a &struct tpm_chip instance, %NULL for the default chip
 * @options:	authentication values and other options
 * @payload:	the key data in clear and encrypted form
 *
 * Note: only TPM 2.0 chip are supported. TPM 1.x implementation is located in
 * the keyring subsystem.
 *
 * Return: same as with tpm_transmit_cmd()
 */
int tpm_unseal_trusted(struct tpm_chip *chip,
		       struct trusted_key_payload *payload,
		       struct trusted_key_options *options)
{
	int rc;

	chip = tpm_find_get_ops(chip);
	if (!chip || !(chip->flags & TPM_CHIP_FLAG_TPM2))
		return -ENODEV;

	rc = tpm2_unseal_trusted(chip, payload, options);

	tpm_put_ops(chip);

	return rc;
}
EXPORT_SYMBOL_GPL(tpm_unseal_trusted);

static int __init tpm_init(void)
{
	int rc;

	tpm_class = class_create(THIS_MODULE, "tpm");
	if (IS_ERR(tpm_class)) {
		pr_err("couldn't create tpm class\n");
		return PTR_ERR(tpm_class);
	}

	tpmrm_class = class_create(THIS_MODULE, "tpmrm");
	if (IS_ERR(tpmrm_class)) {
		pr_err("couldn't create tpmrm class\n");
		rc = PTR_ERR(tpmrm_class);
		goto out_destroy_tpm_class;
	}

	rc = alloc_chrdev_region(&tpm_devt, 0, 2*TPM_NUM_DEVICES, "tpm");
	if (rc < 0) {
		pr_err("tpm: failed to allocate char dev region\n");
		goto out_destroy_tpmrm_class;
	}

	rc = tpm_dev_common_init();
	if (rc) {
		pr_err("tpm: failed to allocate char dev region\n");
		goto out_unreg_chrdev;
	}

	return 0;

out_unreg_chrdev:
	unregister_chrdev_region(tpm_devt, 2 * TPM_NUM_DEVICES);
out_destroy_tpmrm_class:
	class_destroy(tpmrm_class);
out_destroy_tpm_class:
	class_destroy(tpm_class);

	return rc;
}

static void __exit tpm_exit(void)
{
	idr_destroy(&dev_nums_idr);
	class_destroy(tpm_class);
	class_destroy(tpmrm_class);
	unregister_chrdev_region(tpm_devt, 2*TPM_NUM_DEVICES);
	tpm_dev_common_exit();
}

subsys_initcall(tpm_init);
module_exit(tpm_exit);

MODULE_AUTHOR("Leendert van Doorn (leendert@watson.ibm.com)");
MODULE_DESCRIPTION("TPM Driver");
MODULE_VERSION("2.0");
MODULE_LICENSE("GPL");
