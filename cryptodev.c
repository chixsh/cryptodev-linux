/*
 * Driver for /dev/crypto device (aka CryptoDev)
 *
 * Copyright (c) 2004 Michal Ludvig <mludvig@logix.net.nz>, SuSE Labs
 * Copyright (c) 2009 Nikos Mavrogiannopoulos <nmav@gennetsa.com>, Gennet S.A.
 *
 * Device /dev/crypto provides an interface for 
 * accessing kernel CryptoAPI algorithms (ciphers,
 * hashes) from userspace programs.
 *
 * /dev/crypto interface was originally introduced in
 * OpenBSD and this module attempts to keep the API,
 * although a bit extended.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/miscdevice.h>
#include <linux/crypto.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/random.h>
#include "cryptodev.h"
#include <asm/uaccess.h>
#include <asm/ioctl.h>
#include <linux/scatterlist.h>

MODULE_AUTHOR("Michal Ludvig <mludvig@logix.net.nz>");
MODULE_DESCRIPTION("CryptoDev driver");
MODULE_LICENSE("GPL");

/* ====== Compile-time config ====== */

#define CRYPTODEV_STATS

/* ====== Module parameters ====== */

static int verbosity = 0;
module_param(verbosity, int, 0644);
MODULE_PARM_DESC(verbosity, "0: normal, 1: verbose, 2: debug");

#ifdef CRYPTODEV_STATS
static int enable_stats = 0;
module_param(enable_stats, int, 0644);
MODULE_PARM_DESC(enable_stats, "collect statictics about cryptodev usage");
#endif

/* ====== Debug helpers ====== */

#define PFX "cryptodev: "
#define dprintk(level,severity,format,a...)			\
	do { 							\
		if (level <= verbosity)				\
			printk(severity PFX "%s[%u]: " format,	\
			       current->comm, current->pid,	\
			       ##a);				\
	} while (0)

/* ====== CryptoAPI ====== */

#define FILL_SG(sg,ptr,len)					\
	do {							\
		(sg)->page = virt_to_page(ptr);			\
		(sg)->offset = offset_in_page(ptr);		\
		(sg)->length = len;				\
		(sg)->dma_address = 0;				\
	} while (0)

struct csession {
	struct list_head entry;
	struct semaphore sem;
	struct crypto_blkcipher *tfm;
	struct crypto_hash *hash_tfm;
	uint32_t sid;
#ifdef CRYPTODEV_STATS
#if ! ((COP_ENCRYPT < 2) && (COP_DECRYPT < 2))
#error Struct csession.stat uses COP_{ENCRYPT,DECRYPT} as indices. Do something!
#endif
	unsigned long long stat[2];
	size_t stat_max_size, stat_count;
#endif
};

struct fcrypt {
	struct list_head list;
	struct semaphore sem;
};

/* Prepare session for future use. */
static int
crypto_create_session(struct fcrypt *fcr, struct session_op *sop)
{
	struct csession	*ses_new, *ses_ptr;
	struct crypto_blkcipher *blk_tfm=NULL;
	struct crypto_hash *hash_tfm=NULL;
	int ret = 0;
	const char *alg_name=NULL;
	const char *hash_name=NULL;
	struct blkcipher_alg* blkalg;
	int hmac_mode = 1;

	/* Does the request make sense? */
	if (!sop->cipher && !sop->mac) {
		dprintk(1,KERN_DEBUG,"Both 'cipher' and 'mac' unset.\n");
		return -EINVAL;
	}

	switch (sop->cipher) {
		case 0:
			break;
		case CRYPTO_DES_CBC:
			alg_name = "cbc(des)";
			break;
		case CRYPTO_3DES_CBC:
			alg_name = "cbc(3des_ede)";
			break;
		case CRYPTO_BLF_CBC:
			alg_name = "cbc(blowfish)";
			break;
		case CRYPTO_AES_CBC:
			alg_name = "cbc(aes)";
			break;
		case CRYPTO_CAMELLIA_CBC:
			alg_name = "cbc(camelia)";
			break;
		default:
			dprintk(1,KERN_DEBUG,"%s: bad cipher: %d\n", __func__, sop->cipher);
			return -EINVAL;
	}

	switch (sop->mac) {
		case 0:
			break;
		case CRYPTO_MD5_HMAC:
			hash_name = "hmac(md5)";
			break;
		case CRYPTO_RIPEMD160_HMAC:
			hash_name = "hmac(rmd160)";
			break;
		case CRYPTO_SHA1_HMAC:
			hash_name = "hmac(sha1)";
			break;
		case CRYPTO_SHA2_256_HMAC:
			hash_name = "hmac(sha256)";
			break;
		case CRYPTO_SHA2_384_HMAC:
			hash_name = "hmac(sha384)";
			break;
		case CRYPTO_SHA2_512_HMAC:
			hash_name = "hmac(sha512)";
			break;

		/* non-hmac cases */
		case CRYPTO_MD5:
			hash_name = "md5";
			hmac_mode = 0;
			break;
		case CRYPTO_RIPEMD160:
			hash_name = "rmd160";
			hmac_mode = 0;
			break;
		case CRYPTO_SHA1:
			hash_name = "sha1";
			hmac_mode = 0;
			break;
		case CRYPTO_SHA2_256:
			hash_name = "sha256";
			hmac_mode = 0;
			break;
		case CRYPTO_SHA2_384:
			hash_name = "sha384";
			hmac_mode = 0;
			break;
		case CRYPTO_SHA2_512:
			hash_name = "sha512";
			hmac_mode = 0;
			break;

		default:
			dprintk(1,KERN_DEBUG,"%s: bad mac: %d\n", __func__, sop->mac);
			return -EINVAL;
	}

	/* Set-up crypto transform. */
	if (alg_name) {
		uint8_t keyp[CRYPTO_CIPHER_MAX_KEY_LEN];

		blk_tfm = crypto_alloc_blkcipher(alg_name, 0, CRYPTO_ALG_ASYNC);
		if (IS_ERR(blk_tfm)) {
			dprintk(1,KERN_DEBUG,"%s: Failed to load transform for %s\n", __func__,
				   alg_name);
			return -EINVAL;
		}

		blkalg = crypto_blkcipher_alg(blk_tfm);
		if (blkalg != NULL) {
			/* Was correct key length supplied? */
			if ((sop->keylen < blkalg->min_keysize) ||
				(sop->keylen > blkalg->max_keysize)) {
				dprintk(0,KERN_DEBUG,"Wrong keylen '%zu' for algorithm '%s'. Use %u to %u.\n",
					   sop->keylen, alg_name, blkalg->min_keysize, 
					   blkalg->max_keysize);
				ret = -EINVAL;
				goto error;
			}
		}

		if (sop->keylen > CRYPTO_CIPHER_MAX_KEY_LEN) {
			dprintk(0,KERN_DEBUG,"Setting key failed for %s-%zu.\n",
				alg_name, sop->keylen*8);
			ret = -EINVAL;
			goto error;
		}

		/* Copy the key from user and set to TFM. */
		copy_from_user(keyp, sop->key, sop->keylen);
		ret = crypto_blkcipher_setkey(blk_tfm, keyp, sop->keylen);
		if (ret) {
			dprintk(0,KERN_DEBUG,"Setting key failed for %s-%zu.\n",
				alg_name, sop->keylen*8);
			ret = -EINVAL;
			goto error;
		}

	}
	
	if (hash_name) {
		hash_tfm = crypto_alloc_hash(hash_name, 0, CRYPTO_ALG_ASYNC);
		if (IS_ERR(hash_tfm)) {
			dprintk(1,KERN_DEBUG,"%s: Failed to load transform for %s\n", __func__,
				   hash_name);
			return -EINVAL;
		}

		/* Copy the key from user and set to TFM. */
		if (hmac_mode != 0) {
			uint8_t hkeyp[CRYPTO_HMAC_MAX_KEY_LEN];

			if (sop->mackeylen > CRYPTO_HMAC_MAX_KEY_LEN) {
				dprintk(0,KERN_DEBUG,"Setting hmac key failed for %s-%zu.\n",
					hash_name, sop->mackeylen*8);
				ret = -EINVAL;
				goto error;
			}

			copy_from_user(hkeyp, sop->mackey, sop->mackeylen);
			ret = crypto_hash_setkey(hash_tfm, hkeyp, sop->mackeylen);
			if (ret) {
				dprintk(0,KERN_DEBUG,"Setting hmac key failed for %s-%zu.\n",
					hash_name, sop->mackeylen*8);
				ret = -EINVAL;
				goto error;
			}
		}

	}

	/* Create a session and put it to the list. */
	ses_new = kmalloc(sizeof(*ses_new), GFP_KERNEL);
	if(!ses_new) {
		ret = -ENOMEM;
		goto error;
	}

	memset(ses_new, 0, sizeof(*ses_new));
	get_random_bytes(&ses_new->sid, sizeof(ses_new->sid));
	ses_new->tfm = blk_tfm;
	ses_new->hash_tfm = hash_tfm;
	init_MUTEX(&ses_new->sem);

	down(&fcr->sem);
restart:
	list_for_each_entry(ses_ptr, &fcr->list, entry) {
		/* Check for duplicate SID */
		if (unlikely(ses_new->sid == ses_ptr->sid)) {
			get_random_bytes(&ses_new->sid, sizeof(ses_new->sid));
			/* Unless we have a broken RNG this 
			   shouldn't loop forever... ;-) */
			goto restart;
		}
	}

	list_add(&ses_new->entry, &fcr->list);
	up(&fcr->sem);

	/* Fill in some values for the user. */
	sop->ses = ses_new->sid;

	return 0;

error:
	if (blk_tfm)
		crypto_free_blkcipher(blk_tfm);
	if (hash_tfm)
		crypto_free_hash(hash_tfm);
	
	return ret;

}

/* Everything that needs to be done when remowing a session. */
static inline void
crypto_destroy_session(struct csession *ses_ptr)
{
	if(down_trylock(&ses_ptr->sem)) {
		dprintk(2, KERN_DEBUG, "Waiting for semaphore of sid=0x%08X\n",
			ses_ptr->sid);
		down(&ses_ptr->sem);
	}
	dprintk(2, KERN_DEBUG, "Removed session 0x%08X\n", ses_ptr->sid);
#if defined(CRYPTODEV_STATS)
	if(enable_stats)
		dprintk(2, KERN_DEBUG,
			"Usage in Bytes: enc=%llu, dec=%llu, max=%zu, avg=%lu, cnt=%zu\n",
			ses_ptr->stat[COP_ENCRYPT], ses_ptr->stat[COP_DECRYPT],
			ses_ptr->stat_max_size, ses_ptr->stat_count > 0
				? ((unsigned long)(ses_ptr->stat[COP_ENCRYPT]+
						   ses_ptr->stat[COP_DECRYPT]) / 
				   ses_ptr->stat_count) : 0,
			ses_ptr->stat_count);
#endif
	if (ses_ptr->tfm) {
		crypto_free_blkcipher(ses_ptr->tfm);
		ses_ptr->tfm = NULL;
	}
	if (ses_ptr->hash_tfm) {
		crypto_free_hash(ses_ptr->hash_tfm);
		ses_ptr->hash_tfm = NULL;
	}
	up(&ses_ptr->sem);
	kfree(ses_ptr);
}

/* Look up a session by ID and remove. */
static int
crypto_finish_session(struct fcrypt *fcr, uint32_t sid)
{
	struct csession *tmp, *ses_ptr;
	struct list_head *head;
	int ret = 0;

	down(&fcr->sem);
	head = &fcr->list;
	list_for_each_entry_safe(ses_ptr, tmp, head, entry) {
		if(ses_ptr->sid == sid) {
			list_del(&ses_ptr->entry);
			crypto_destroy_session(ses_ptr);
			break;
		}
	}

	if (!ses_ptr) {
		dprintk(1, KERN_ERR, "Session with sid=0x%08X not found!\n", sid);
		ret = -ENOENT;
	}
	up(&fcr->sem);

	return ret;
}

/* Remove all sessions when closing the file */
static int
crypto_finish_all_sessions(struct fcrypt *fcr)
{
	struct csession *tmp, *ses_ptr;
	struct list_head *head;

	down(&fcr->sem);

	head = &fcr->list;
	list_for_each_entry_safe(ses_ptr, tmp, head, entry) {
		list_del(&ses_ptr->entry);
		crypto_destroy_session(ses_ptr);
	}
	up(&fcr->sem);

	return 0;
}

/* Look up session by session ID. The returned session is locked. */
static struct csession *
crypto_get_session_by_sid(struct fcrypt *fcr, uint32_t sid)
{
	struct csession *ses_ptr;

	down(&fcr->sem);
	list_for_each_entry(ses_ptr, &fcr->list, entry) {
		if(ses_ptr->sid == sid) {
			down(&ses_ptr->sem);
			break;
		}
	}
	up(&fcr->sem);

	return ses_ptr;
}

static int crypto_runv(struct fcrypt *fcr, struct crypt_opv *copv)
{
	char *data, ivp[EALG_MAX_BLOCK_LEN];
	char __user *src, __user *dst;
	struct scatterlist sg;
	struct csession *ses_ptr;
	unsigned int ivsize=0;
	size_t nbytes, bufsize;
	int ret = 0;
	uint8_t hash_output[HASH_MAX_LEN];
	int blocksize=1, i;
	struct blkcipher_desc bdesc = {
        .flags = CRYPTO_TFM_REQ_MAY_SLEEP,
	};
	struct hash_desc hdesc = {
		.flags = CRYPTO_TFM_REQ_MAY_SLEEP,
	};


	if (unlikely(copv->op != COP_ENCRYPT && copv->op != COP_DECRYPT)) {
		dprintk(1, KERN_DEBUG, "invalid operation op=%u\n", copv->op);
		return -EINVAL;
	}

	ses_ptr = crypto_get_session_by_sid(fcr, copv->ses);
	if (!ses_ptr) {
		dprintk(1, KERN_ERR, "invalid session ID=0x%08X\n", copv->ses);
		return -EINVAL;
	}

	data = (char*)__get_free_page(GFP_KERNEL);

	if (unlikely(!data)) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	bdesc.tfm = ses_ptr->tfm;
	hdesc.tfm = ses_ptr->hash_tfm;

	if (hdesc.tfm) {
		ret = crypto_hash_init(&hdesc);
		if (unlikely(ret)) {
			dprintk(1, KERN_ERR,
				"error in crypto_hash_init()\n");
			goto out_unlock;
		}
	}

	if (ses_ptr->tfm) {
		blocksize = crypto_blkcipher_blocksize(ses_ptr->tfm);
		ivsize = crypto_blkcipher_ivsize(ses_ptr->tfm);

		if (copv->iv) {
			copy_from_user(ivp, copv->iv, ivsize);
			crypto_blkcipher_set_iv(ses_ptr->tfm, ivp, ivsize);
		}
	}

	dst = copv->dst;

	for (i=0;i<copv->iovec_cnt;i++) {
		nbytes = copv->iovec[i].len;

		if (unlikely(bdesc.tfm && (copv->iovec[i].op_flags & IOP_CIPHER) && (nbytes % blocksize))) {
			dprintk(1, KERN_ERR,
				"data size (%zu) isn't a multiple of block size (%u)\n",
				nbytes, crypto_blkcipher_blocksize(ses_ptr->tfm));
			ret = -EINVAL;
			goto out_unlock;
		}

		bufsize = PAGE_SIZE < nbytes ? PAGE_SIZE : nbytes;

		src = copv->iovec[i].src;

		while(nbytes > 0) {
			size_t current_len = nbytes > bufsize ? bufsize : nbytes;

			copy_from_user(data, src, current_len);

			sg_set_buf(&sg, data, current_len);

			/* Always hash before encryption and after decryption. Maybe
			 * we should introduce a flag to switch... TBD later on.
			 */
			if (copv->op == COP_ENCRYPT) {
				if (hdesc.tfm && (copv->iovec[i].op_flags & IOP_HASH)) {
					ret = crypto_hash_update(&hdesc, &sg, current_len);
					if (unlikely(ret)) {
						dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
						goto out;
					}
				}
				if (bdesc.tfm && (copv->iovec[i].op_flags & IOP_CIPHER)) {
					ret = crypto_blkcipher_encrypt(&bdesc, &sg, &sg, current_len);

					if (unlikely(ret)) {
						dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
						goto out;
					}
					copy_to_user(dst, data, current_len);
					dst += current_len;
				}
			} else {
				if (bdesc.tfm && (copv->iovec[i].op_flags & IOP_CIPHER)) {
					ret = crypto_blkcipher_decrypt(&bdesc, &sg, &sg, current_len);

					if (unlikely(ret)) {
						dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
						goto out;
					}
					copy_to_user(dst, data, current_len);
					dst += current_len;

				}

				if (hdesc.tfm && (copv->iovec[i].op_flags & IOP_HASH)) {
					ret = crypto_hash_update(&hdesc, &sg, current_len);
					if (unlikely(ret)) {
						dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
						goto out;
					}
				}
			}

			nbytes -= current_len;
			src += current_len;
		}

#if defined(CRYPTODEV_STATS)
	if (enable_stats) {
		/* this is safe - we check cop->op at the function entry */
		ses_ptr->stat[copv->op] += copv->iovec[i].len;
		if (ses_ptr->stat_max_size < copv->iovec[i].len)
			ses_ptr->stat_max_size = copv->iovec[i].len;
		ses_ptr->stat_count++;
	}
#endif

	}
	
	if (hdesc.tfm) {
		ret = crypto_hash_final(&hdesc, hash_output);
		if (unlikely(ret)) {
			dprintk(0, KERN_ERR, "CryptoAPI failure: %d\n",ret);
			goto out;
		}

		copy_to_user(copv->mac, hash_output, crypto_hash_digestsize(ses_ptr->hash_tfm));
	}

out:
	free_page((unsigned long)data);

out_unlock:
	up(&ses_ptr->sem);

	return ret;
}

/* This is the main crypto function - feed it with plaintext 
   and get a ciphertext (or vice versa :-) */
static int crypto_run(struct fcrypt *fcr, struct crypt_op *cop)
{
	struct crypt_opv copv;
	struct crypt_iovec iovec;
	
	iovec.src = cop->src;
	iovec.len = cop->len;
	iovec.op_flags = IOP_CIPHER|IOP_HASH;
	
	copv.op = cop->op;
	copv.ses = cop->ses;
	copv.flags = cop->flags;
	copv.iovec = &iovec;
	copv.iovec_cnt = 1;

	copv.dst = cop->dst;
	copv.mac = cop->mac;
	copv.iv = cop->iv;
	
	return crypto_runv(fcr, &copv);
	
}



/* ====== /dev/crypto ====== */

static int
cryptodev_open(struct inode *inode, struct file *filp)
{
	struct fcrypt *fcr;

	fcr = kmalloc(sizeof(*fcr), GFP_KERNEL);
	if(!fcr)
		return -ENOMEM;

	memset(fcr, 0, sizeof(*fcr));
	init_MUTEX(&fcr->sem);
	INIT_LIST_HEAD(&fcr->list);
	filp->private_data = fcr;

	return 0;
}

static int
cryptodev_release(struct inode *inode, struct file *filp)
{
	struct fcrypt *fcr = filp->private_data;

	if(fcr) {
		crypto_finish_all_sessions(fcr);
		kfree(fcr);
		filp->private_data = NULL;
	}
	
	return 0;
}

static int
clonefd(struct file *filp)
{
	struct fdtable *fdt = files_fdtable(current->files);
	int ret;
	ret = get_unused_fd();
	if (ret >= 0) {
			get_file(filp);
			FD_SET(ret, fdt->open_fds);
			fd_install(ret, filp);
	}

	return ret;
}

static int
cryptodev_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	int __user *p = (void __user *)arg;
	struct session_op sop;
	struct crypt_op cop;
	struct crypt_opv copv;
	struct fcrypt *fcr = filp->private_data;
	uint32_t ses;
	int ret, fd;

	if (!fcr)
		BUG();

	switch (cmd) {
		case CIOCASYMFEAT:
			put_user(0, p);
			return 0;
		case CRIOGET:
			fd = clonefd(filp);
			put_user(fd, p);
			return 0;
			
		case CIOCGSESSION:
			copy_from_user(&sop, (void*)arg, sizeof(sop));
			ret = crypto_create_session(fcr, &sop);
			if (ret)
				return ret;
			copy_to_user((void*)arg, &sop, sizeof(sop));
			return 0;

		case CIOCFSESSION:
			get_user(ses, (uint32_t*)arg);
			ret = crypto_finish_session(fcr, ses);
			return ret;

		case CIOCCRYPT:
			copy_from_user(&cop, (void*)arg, sizeof(cop));
			ret = crypto_run(fcr, &cop);
			copy_to_user((void*)arg, &cop, sizeof(cop));
			return ret;

		case CIOCCRYPTV:
			copy_from_user(&cop, (void*)arg, sizeof(copv));
			ret = crypto_runv(fcr, &copv);
			copy_to_user((void*)arg, &copv, sizeof(copv));
			return ret;

		default:
			return -EINVAL;
	}
}

struct file_operations cryptodev_fops = {
	.owner = THIS_MODULE,
	.open = cryptodev_open,
	.release = cryptodev_release,
	.ioctl = cryptodev_ioctl,
};

struct miscdevice cryptodev = {
	.minor = CRYPTODEV_MINOR,
	.name = "crypto",
	.fops = &cryptodev_fops,
};

static int
cryptodev_register(void)
{
	int rc;

	rc = misc_register (&cryptodev);
	if (rc) {
		printk(KERN_ERR PFX "registeration of /dev/crypto failed\n");
		return rc;
	}

	return 0;
}

static void
cryptodev_deregister(void)
{
	misc_deregister(&cryptodev);
}

/* ====== Module init/exit ====== */

int __init init_cryptodev(void)
{
	int rc;
	
	rc = cryptodev_register();
	if (rc)
		return rc;

	printk(KERN_INFO PFX "driver loaded.\n");

	return 0;
}

void __exit exit_cryptodev(void)
{
	cryptodev_deregister();
	printk(KERN_INFO PFX "driver unloaded.\n");
}

module_init(init_cryptodev);
module_exit(exit_cryptodev);
