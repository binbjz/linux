// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Abstract layer for MIDI v1.0 stream
 *  Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 */

#include <sound/core.h>
#include <linux/major.h>
#include <linux/init.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/nospec.h>
#include <sound/rawmidi.h>
#include <sound/info.h>
#include <sound/control.h>
#include <sound/minors.h>
#include <sound/initval.h>
#include <sound/ump.h>

MODULE_AUTHOR("Jaroslav Kysela <perex@perex.cz>");
MODULE_DESCRIPTION("Midlevel RawMidi code for ALSA.");
MODULE_LICENSE("GPL");

#ifdef CONFIG_SND_OSSEMUL
static int midi_map[SNDRV_CARDS];
static int amidi_map[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS-1)] = 1};
module_param_array(midi_map, int, NULL, 0444);
MODULE_PARM_DESC(midi_map, "Raw MIDI device number assigned to 1st OSS device.");
module_param_array(amidi_map, int, NULL, 0444);
MODULE_PARM_DESC(amidi_map, "Raw MIDI device number assigned to 2nd OSS device.");
#endif /* CONFIG_SND_OSSEMUL */

static int snd_rawmidi_dev_free(struct snd_device *device);
static int snd_rawmidi_dev_register(struct snd_device *device);
static int snd_rawmidi_dev_disconnect(struct snd_device *device);

static LIST_HEAD(snd_rawmidi_devices);
static DEFINE_MUTEX(register_mutex);

#define rmidi_err(rmidi, fmt, args...) \
	dev_err((rmidi)->dev, fmt, ##args)
#define rmidi_warn(rmidi, fmt, args...) \
	dev_warn((rmidi)->dev, fmt, ##args)
#define rmidi_dbg(rmidi, fmt, args...) \
	dev_dbg((rmidi)->dev, fmt, ##args)

struct snd_rawmidi_status32 {
	s32 stream;
	s32 tstamp_sec;			/* Timestamp */
	s32 tstamp_nsec;
	u32 avail;			/* available bytes */
	u32 xruns;			/* count of overruns since last status (in bytes) */
	unsigned char reserved[16];	/* reserved for future use */
};

#define SNDRV_RAWMIDI_IOCTL_STATUS32	_IOWR('W', 0x20, struct snd_rawmidi_status32)

struct snd_rawmidi_status64 {
	int stream;
	u8 rsvd[4];			/* alignment */
	s64 tstamp_sec;			/* Timestamp */
	s64 tstamp_nsec;
	size_t avail;			/* available bytes */
	size_t xruns;			/* count of overruns since last status (in bytes) */
	unsigned char reserved[16];	/* reserved for future use */
};

#define SNDRV_RAWMIDI_IOCTL_STATUS64	_IOWR('W', 0x20, struct snd_rawmidi_status64)

#define rawmidi_is_ump(rmidi) \
	(IS_ENABLED(CONFIG_SND_UMP) && ((rmidi)->info_flags & SNDRV_RAWMIDI_INFO_UMP))

static struct snd_rawmidi *snd_rawmidi_search(struct snd_card *card, int device)
{
	struct snd_rawmidi *rawmidi;

	list_for_each_entry(rawmidi, &snd_rawmidi_devices, list)
		if (rawmidi->card == card && rawmidi->device == device)
			return rawmidi;
	return NULL;
}

static inline unsigned short snd_rawmidi_file_flags(struct file *file)
{
	switch (file->f_mode & (FMODE_READ | FMODE_WRITE)) {
	case FMODE_WRITE:
		return SNDRV_RAWMIDI_LFLG_OUTPUT;
	case FMODE_READ:
		return SNDRV_RAWMIDI_LFLG_INPUT;
	default:
		return SNDRV_RAWMIDI_LFLG_OPEN;
	}
}

static inline bool __snd_rawmidi_ready(struct snd_rawmidi_runtime *runtime)
{
	return runtime->avail >= runtime->avail_min;
}

static bool snd_rawmidi_ready(struct snd_rawmidi_substream *substream)
{
	guard(spinlock_irqsave)(&substream->lock);
	return __snd_rawmidi_ready(substream->runtime);
}

static inline int snd_rawmidi_ready_append(struct snd_rawmidi_substream *substream,
					   size_t count)
{
	struct snd_rawmidi_runtime *runtime = substream->runtime;

	return runtime->avail >= runtime->avail_min &&
	       (!substream->append || runtime->avail >= count);
}

static void snd_rawmidi_input_event_work(struct work_struct *work)
{
	struct snd_rawmidi_runtime *runtime =
		container_of(work, struct snd_rawmidi_runtime, event_work);

	if (runtime->event)
		runtime->event(runtime->substream);
}

/* buffer refcount management: call with substream->lock held */
static inline void snd_rawmidi_buffer_ref(struct snd_rawmidi_runtime *runtime)
{
	runtime->buffer_ref++;
}

static inline void snd_rawmidi_buffer_unref(struct snd_rawmidi_runtime *runtime)
{
	runtime->buffer_ref--;
}

static void snd_rawmidi_buffer_ref_sync(struct snd_rawmidi_substream *substream)
{
	int loop = HZ;

	spin_lock_irq(&substream->lock);
	while (substream->runtime->buffer_ref) {
		spin_unlock_irq(&substream->lock);
		if (!--loop) {
			rmidi_err(substream->rmidi, "Buffer ref sync timeout\n");
			return;
		}
		schedule_timeout_uninterruptible(1);
		spin_lock_irq(&substream->lock);
	}
	spin_unlock_irq(&substream->lock);
}

static int snd_rawmidi_runtime_create(struct snd_rawmidi_substream *substream)
{
	struct snd_rawmidi_runtime *runtime;

	runtime = kzalloc(sizeof(*runtime), GFP_KERNEL);
	if (!runtime)
		return -ENOMEM;
	runtime->substream = substream;
	init_waitqueue_head(&runtime->sleep);
	INIT_WORK(&runtime->event_work, snd_rawmidi_input_event_work);
	runtime->event = NULL;
	runtime->buffer_size = PAGE_SIZE;
	runtime->avail_min = 1;
	if (substream->stream == SNDRV_RAWMIDI_STREAM_INPUT)
		runtime->avail = 0;
	else
		runtime->avail = runtime->buffer_size;
	runtime->buffer = kvzalloc(runtime->buffer_size, GFP_KERNEL);
	if (!runtime->buffer) {
		kfree(runtime);
		return -ENOMEM;
	}
	runtime->appl_ptr = runtime->hw_ptr = 0;
	substream->runtime = runtime;
	if (rawmidi_is_ump(substream->rmidi))
		runtime->align = 3;
	return 0;
}

/* get the current alignment (either 0 or 3) */
static inline int get_align(struct snd_rawmidi_runtime *runtime)
{
	if (IS_ENABLED(CONFIG_SND_UMP))
		return runtime->align;
	else
		return 0;
}

/* get the trimmed size with the current alignment */
#define get_aligned_size(runtime, size) ((size) & ~get_align(runtime))

static int snd_rawmidi_runtime_free(struct snd_rawmidi_substream *substream)
{
	struct snd_rawmidi_runtime *runtime = substream->runtime;

	kvfree(runtime->buffer);
	kfree(runtime);
	substream->runtime = NULL;
	return 0;
}

static inline void snd_rawmidi_output_trigger(struct snd_rawmidi_substream *substream, int up)
{
	if (!substream->opened)
		return;
	substream->ops->trigger(substream, up);
}

static void snd_rawmidi_input_trigger(struct snd_rawmidi_substream *substream, int up)
{
	if (!substream->opened)
		return;
	substream->ops->trigger(substream, up);
	if (!up)
		cancel_work_sync(&substream->runtime->event_work);
}

static void __reset_runtime_ptrs(struct snd_rawmidi_runtime *runtime,
				 bool is_input)
{
	runtime->drain = 0;
	runtime->appl_ptr = runtime->hw_ptr = 0;
	runtime->avail = is_input ? 0 : runtime->buffer_size;
}

static void reset_runtime_ptrs(struct snd_rawmidi_substream *substream,
			       bool is_input)
{
	guard(spinlock_irqsave)(&substream->lock);
	if (substream->opened && substream->runtime)
		__reset_runtime_ptrs(substream->runtime, is_input);
}

int snd_rawmidi_drop_output(struct snd_rawmidi_substream *substream)
{
	snd_rawmidi_output_trigger(substream, 0);
	reset_runtime_ptrs(substream, false);
	return 0;
}
EXPORT_SYMBOL(snd_rawmidi_drop_output);

int snd_rawmidi_drain_output(struct snd_rawmidi_substream *substream)
{
	int err = 0;
	long timeout;
	struct snd_rawmidi_runtime *runtime;

	scoped_guard(spinlock_irq, &substream->lock) {
		runtime = substream->runtime;
		if (!substream->opened || !runtime || !runtime->buffer)
			return -EINVAL;
		snd_rawmidi_buffer_ref(runtime);
		runtime->drain = 1;
	}

	timeout = wait_event_interruptible_timeout(runtime->sleep,
				(runtime->avail >= runtime->buffer_size),
				10*HZ);

	scoped_guard(spinlock_irq, &substream->lock) {
		if (signal_pending(current))
			err = -ERESTARTSYS;
		if (runtime->avail < runtime->buffer_size && !timeout) {
			rmidi_warn(substream->rmidi,
				   "rawmidi drain error (avail = %li, buffer_size = %li)\n",
				   (long)runtime->avail, (long)runtime->buffer_size);
			err = -EIO;
		}
		runtime->drain = 0;
	}

	if (err != -ERESTARTSYS) {
		/* we need wait a while to make sure that Tx FIFOs are empty */
		if (substream->ops->drain)
			substream->ops->drain(substream);
		else
			msleep(50);
		snd_rawmidi_drop_output(substream);
	}

	scoped_guard(spinlock_irq, &substream->lock)
		snd_rawmidi_buffer_unref(runtime);

	return err;
}
EXPORT_SYMBOL(snd_rawmidi_drain_output);

int snd_rawmidi_drain_input(struct snd_rawmidi_substream *substream)
{
	snd_rawmidi_input_trigger(substream, 0);
	reset_runtime_ptrs(substream, true);
	return 0;
}
EXPORT_SYMBOL(snd_rawmidi_drain_input);

/* look for an available substream for the given stream direction;
 * if a specific subdevice is given, try to assign it
 */
static int assign_substream(struct snd_rawmidi *rmidi, int subdevice,
			    int stream, int mode,
			    struct snd_rawmidi_substream **sub_ret)
{
	struct snd_rawmidi_substream *substream;
	struct snd_rawmidi_str *s = &rmidi->streams[stream];
	static const unsigned int info_flags[2] = {
		[SNDRV_RAWMIDI_STREAM_OUTPUT] = SNDRV_RAWMIDI_INFO_OUTPUT,
		[SNDRV_RAWMIDI_STREAM_INPUT] = SNDRV_RAWMIDI_INFO_INPUT,
	};

	if (!(rmidi->info_flags & info_flags[stream]))
		return -ENXIO;
	if (subdevice >= 0 && subdevice >= s->substream_count)
		return -ENODEV;

	list_for_each_entry(substream, &s->substreams, list) {
		if (substream->opened) {
			if (stream == SNDRV_RAWMIDI_STREAM_INPUT ||
			    !(mode & SNDRV_RAWMIDI_LFLG_APPEND) ||
			    !substream->append)
				continue;
		}
		if (subdevice < 0 || subdevice == substream->number) {
			*sub_ret = substream;
			return 0;
		}
	}
	return -EAGAIN;
}

/* open and do ref-counting for the given substream */
static int open_substream(struct snd_rawmidi *rmidi,
			  struct snd_rawmidi_substream *substream,
			  int mode)
{
	int err;

	if (substream->use_count == 0) {
		err = snd_rawmidi_runtime_create(substream);
		if (err < 0)
			return err;
		err = substream->ops->open(substream);
		if (err < 0) {
			snd_rawmidi_runtime_free(substream);
			return err;
		}
		guard(spinlock_irq)(&substream->lock);
		substream->opened = 1;
		substream->active_sensing = 0;
		if (mode & SNDRV_RAWMIDI_LFLG_APPEND)
			substream->append = 1;
		substream->pid = get_pid(task_pid(current));
		rmidi->streams[substream->stream].substream_opened++;
	}
	substream->use_count++;
	return 0;
}

static void close_substream(struct snd_rawmidi *rmidi,
			    struct snd_rawmidi_substream *substream,
			    int cleanup);

static int rawmidi_open_priv(struct snd_rawmidi *rmidi, int subdevice, int mode,
			     struct snd_rawmidi_file *rfile)
{
	struct snd_rawmidi_substream *sinput = NULL, *soutput = NULL;
	int err;

	rfile->input = rfile->output = NULL;
	if (mode & SNDRV_RAWMIDI_LFLG_INPUT) {
		err = assign_substream(rmidi, subdevice,
				       SNDRV_RAWMIDI_STREAM_INPUT,
				       mode, &sinput);
		if (err < 0)
			return err;
	}
	if (mode & SNDRV_RAWMIDI_LFLG_OUTPUT) {
		err = assign_substream(rmidi, subdevice,
				       SNDRV_RAWMIDI_STREAM_OUTPUT,
				       mode, &soutput);
		if (err < 0)
			return err;
	}

	if (sinput) {
		err = open_substream(rmidi, sinput, mode);
		if (err < 0)
			return err;
	}
	if (soutput) {
		err = open_substream(rmidi, soutput, mode);
		if (err < 0) {
			if (sinput)
				close_substream(rmidi, sinput, 0);
			return err;
		}
	}

	rfile->rmidi = rmidi;
	rfile->input = sinput;
	rfile->output = soutput;
	return 0;
}

/* called from sound/core/seq/seq_midi.c */
int snd_rawmidi_kernel_open(struct snd_rawmidi *rmidi, int subdevice,
			    int mode, struct snd_rawmidi_file *rfile)
{
	int err;

	if (snd_BUG_ON(!rfile))
		return -EINVAL;
	if (!try_module_get(rmidi->card->module))
		return -ENXIO;

	guard(mutex)(&rmidi->open_mutex);
	err = rawmidi_open_priv(rmidi, subdevice, mode, rfile);
	if (err < 0)
		module_put(rmidi->card->module);
	return err;
}
EXPORT_SYMBOL(snd_rawmidi_kernel_open);

static int snd_rawmidi_open(struct inode *inode, struct file *file)
{
	int maj = imajor(inode);
	struct snd_card *card;
	int subdevice;
	unsigned short fflags;
	int err;
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_file *rawmidi_file = NULL;
	wait_queue_entry_t wait;

	if ((file->f_flags & O_APPEND) && !(file->f_flags & O_NONBLOCK))
		return -EINVAL;		/* invalid combination */

	err = stream_open(inode, file);
	if (err < 0)
		return err;

	if (maj == snd_major) {
		rmidi = snd_lookup_minor_data(iminor(inode),
					      SNDRV_DEVICE_TYPE_RAWMIDI);
#ifdef CONFIG_SND_OSSEMUL
	} else if (maj == SOUND_MAJOR) {
		rmidi = snd_lookup_oss_minor_data(iminor(inode),
						  SNDRV_OSS_DEVICE_TYPE_MIDI);
#endif
	} else
		return -ENXIO;

	if (rmidi == NULL)
		return -ENODEV;

	if (!try_module_get(rmidi->card->module)) {
		snd_card_unref(rmidi->card);
		return -ENXIO;
	}

	mutex_lock(&rmidi->open_mutex);
	card = rmidi->card;
	err = snd_card_file_add(card, file);
	if (err < 0)
		goto __error_card;
	fflags = snd_rawmidi_file_flags(file);
	if ((file->f_flags & O_APPEND) || maj == SOUND_MAJOR) /* OSS emul? */
		fflags |= SNDRV_RAWMIDI_LFLG_APPEND;
	rawmidi_file = kmalloc(sizeof(*rawmidi_file), GFP_KERNEL);
	if (rawmidi_file == NULL) {
		err = -ENOMEM;
		goto __error;
	}
	rawmidi_file->user_pversion = 0;
	init_waitqueue_entry(&wait, current);
	add_wait_queue(&rmidi->open_wait, &wait);
	while (1) {
		subdevice = snd_ctl_get_preferred_subdevice(card, SND_CTL_SUBDEV_RAWMIDI);
		err = rawmidi_open_priv(rmidi, subdevice, fflags, rawmidi_file);
		if (err >= 0)
			break;
		if (err == -EAGAIN) {
			if (file->f_flags & O_NONBLOCK) {
				err = -EBUSY;
				break;
			}
		} else
			break;
		set_current_state(TASK_INTERRUPTIBLE);
		mutex_unlock(&rmidi->open_mutex);
		schedule();
		mutex_lock(&rmidi->open_mutex);
		if (rmidi->card->shutdown) {
			err = -ENODEV;
			break;
		}
		if (signal_pending(current)) {
			err = -ERESTARTSYS;
			break;
		}
	}
	remove_wait_queue(&rmidi->open_wait, &wait);
	if (err < 0) {
		kfree(rawmidi_file);
		goto __error;
	}
#ifdef CONFIG_SND_OSSEMUL
	if (rawmidi_file->input && rawmidi_file->input->runtime)
		rawmidi_file->input->runtime->oss = (maj == SOUND_MAJOR);
	if (rawmidi_file->output && rawmidi_file->output->runtime)
		rawmidi_file->output->runtime->oss = (maj == SOUND_MAJOR);
#endif
	file->private_data = rawmidi_file;
	mutex_unlock(&rmidi->open_mutex);
	snd_card_unref(rmidi->card);
	return 0;

 __error:
	snd_card_file_remove(card, file);
 __error_card:
	mutex_unlock(&rmidi->open_mutex);
	module_put(rmidi->card->module);
	snd_card_unref(rmidi->card);
	return err;
}

static void close_substream(struct snd_rawmidi *rmidi,
			    struct snd_rawmidi_substream *substream,
			    int cleanup)
{
	if (--substream->use_count)
		return;

	if (cleanup) {
		if (substream->stream == SNDRV_RAWMIDI_STREAM_INPUT)
			snd_rawmidi_input_trigger(substream, 0);
		else {
			if (substream->active_sensing) {
				unsigned char buf = 0xfe;
				/* sending single active sensing message
				 * to shut the device up
				 */
				snd_rawmidi_kernel_write(substream, &buf, 1);
			}
			if (snd_rawmidi_drain_output(substream) == -ERESTARTSYS)
				snd_rawmidi_output_trigger(substream, 0);
		}
		snd_rawmidi_buffer_ref_sync(substream);
	}
	scoped_guard(spinlock_irq, &substream->lock) {
		substream->opened = 0;
		substream->append = 0;
	}
	substream->ops->close(substream);
	if (substream->runtime->private_free)
		substream->runtime->private_free(substream);
	snd_rawmidi_runtime_free(substream);
	put_pid(substream->pid);
	substream->pid = NULL;
	rmidi->streams[substream->stream].substream_opened--;
}

static void rawmidi_release_priv(struct snd_rawmidi_file *rfile)
{
	struct snd_rawmidi *rmidi;

	rmidi = rfile->rmidi;
	guard(mutex)(&rmidi->open_mutex);
	if (rfile->input) {
		close_substream(rmidi, rfile->input, 1);
		rfile->input = NULL;
	}
	if (rfile->output) {
		close_substream(rmidi, rfile->output, 1);
		rfile->output = NULL;
	}
	rfile->rmidi = NULL;
	wake_up(&rmidi->open_wait);
}

/* called from sound/core/seq/seq_midi.c */
int snd_rawmidi_kernel_release(struct snd_rawmidi_file *rfile)
{
	struct snd_rawmidi *rmidi;

	if (snd_BUG_ON(!rfile))
		return -ENXIO;

	rmidi = rfile->rmidi;
	rawmidi_release_priv(rfile);
	module_put(rmidi->card->module);
	return 0;
}
EXPORT_SYMBOL(snd_rawmidi_kernel_release);

static int snd_rawmidi_release(struct inode *inode, struct file *file)
{
	struct snd_rawmidi_file *rfile;
	struct snd_rawmidi *rmidi;
	struct module *module;

	rfile = file->private_data;
	rmidi = rfile->rmidi;
	rawmidi_release_priv(rfile);
	kfree(rfile);
	module = rmidi->card->module;
	snd_card_file_remove(rmidi->card, file);
	module_put(module);
	return 0;
}

static int snd_rawmidi_info(struct snd_rawmidi_substream *substream,
			    struct snd_rawmidi_info *info)
{
	struct snd_rawmidi *rmidi;

	if (substream == NULL)
		return -ENODEV;
	rmidi = substream->rmidi;
	memset(info, 0, sizeof(*info));
	info->card = rmidi->card->number;
	info->device = rmidi->device;
	info->subdevice = substream->number;
	info->stream = substream->stream;
	info->flags = rmidi->info_flags;
	if (substream->inactive)
		info->flags |= SNDRV_RAWMIDI_INFO_STREAM_INACTIVE;
	strscpy(info->id, rmidi->id);
	strscpy(info->name, rmidi->name);
	strscpy(info->subname, substream->name);
	info->subdevices_count = substream->pstr->substream_count;
	info->subdevices_avail = (substream->pstr->substream_count -
				  substream->pstr->substream_opened);
	info->tied_device = rmidi->tied_device;
	return 0;
}

static int snd_rawmidi_info_user(struct snd_rawmidi_substream *substream,
				 struct snd_rawmidi_info __user *_info)
{
	struct snd_rawmidi_info info;
	int err;

	err = snd_rawmidi_info(substream, &info);
	if (err < 0)
		return err;
	if (copy_to_user(_info, &info, sizeof(struct snd_rawmidi_info)))
		return -EFAULT;
	return 0;
}

static int __snd_rawmidi_info_select(struct snd_card *card,
				     struct snd_rawmidi_info *info)
{
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_str *pstr;
	struct snd_rawmidi_substream *substream;

	rmidi = snd_rawmidi_search(card, info->device);
	if (!rmidi)
		return -ENXIO;
	if (info->stream < 0 || info->stream > 1)
		return -EINVAL;
	info->stream = array_index_nospec(info->stream, 2);
	pstr = &rmidi->streams[info->stream];
	if (pstr->substream_count == 0)
		return -ENOENT;
	if (info->subdevice >= pstr->substream_count)
		return -ENXIO;
	list_for_each_entry(substream, &pstr->substreams, list) {
		if ((unsigned int)substream->number == info->subdevice)
			return snd_rawmidi_info(substream, info);
	}
	return -ENXIO;
}

int snd_rawmidi_info_select(struct snd_card *card, struct snd_rawmidi_info *info)
{
	guard(mutex)(&register_mutex);
	return __snd_rawmidi_info_select(card, info);
}
EXPORT_SYMBOL(snd_rawmidi_info_select);

static int snd_rawmidi_info_select_user(struct snd_card *card,
					struct snd_rawmidi_info __user *_info)
{
	int err;
	struct snd_rawmidi_info info;

	if (get_user(info.device, &_info->device))
		return -EFAULT;
	if (get_user(info.stream, &_info->stream))
		return -EFAULT;
	if (get_user(info.subdevice, &_info->subdevice))
		return -EFAULT;
	err = snd_rawmidi_info_select(card, &info);
	if (err < 0)
		return err;
	if (copy_to_user(_info, &info, sizeof(struct snd_rawmidi_info)))
		return -EFAULT;
	return 0;
}

static int resize_runtime_buffer(struct snd_rawmidi_substream *substream,
				 struct snd_rawmidi_params *params,
				 bool is_input)
{
	struct snd_rawmidi_runtime *runtime = substream->runtime;
	char *newbuf, *oldbuf;
	unsigned int framing = params->mode & SNDRV_RAWMIDI_MODE_FRAMING_MASK;

	if (params->buffer_size < 32 || params->buffer_size > 1024L * 1024L)
		return -EINVAL;
	if (framing == SNDRV_RAWMIDI_MODE_FRAMING_TSTAMP && (params->buffer_size & 0x1f) != 0)
		return -EINVAL;
	if (params->avail_min < 1 || params->avail_min > params->buffer_size)
		return -EINVAL;
	if (params->buffer_size & get_align(runtime))
		return -EINVAL;
	if (params->buffer_size != runtime->buffer_size) {
		newbuf = kvzalloc(params->buffer_size, GFP_KERNEL);
		if (!newbuf)
			return -ENOMEM;
		spin_lock_irq(&substream->lock);
		if (runtime->buffer_ref) {
			spin_unlock_irq(&substream->lock);
			kvfree(newbuf);
			return -EBUSY;
		}
		oldbuf = runtime->buffer;
		runtime->buffer = newbuf;
		runtime->buffer_size = params->buffer_size;
		__reset_runtime_ptrs(runtime, is_input);
		spin_unlock_irq(&substream->lock);
		kvfree(oldbuf);
	}
	runtime->avail_min = params->avail_min;
	return 0;
}

int snd_rawmidi_output_params(struct snd_rawmidi_substream *substream,
			      struct snd_rawmidi_params *params)
{
	int err;

	snd_rawmidi_drain_output(substream);
	guard(mutex)(&substream->rmidi->open_mutex);
	if (substream->append && substream->use_count > 1)
		return -EBUSY;
	err = resize_runtime_buffer(substream, params, false);
	if (!err)
		substream->active_sensing = !params->no_active_sensing;
	return err;
}
EXPORT_SYMBOL(snd_rawmidi_output_params);

int snd_rawmidi_input_params(struct snd_rawmidi_substream *substream,
			     struct snd_rawmidi_params *params)
{
	unsigned int framing = params->mode & SNDRV_RAWMIDI_MODE_FRAMING_MASK;
	unsigned int clock_type = params->mode & SNDRV_RAWMIDI_MODE_CLOCK_MASK;
	int err;

	snd_rawmidi_drain_input(substream);
	guard(mutex)(&substream->rmidi->open_mutex);
	if (framing == SNDRV_RAWMIDI_MODE_FRAMING_NONE && clock_type != SNDRV_RAWMIDI_MODE_CLOCK_NONE)
		err = -EINVAL;
	else if (clock_type > SNDRV_RAWMIDI_MODE_CLOCK_MONOTONIC_RAW)
		err = -EINVAL;
	else if (framing > SNDRV_RAWMIDI_MODE_FRAMING_TSTAMP)
		err = -EINVAL;
	else
		err = resize_runtime_buffer(substream, params, true);

	if (!err) {
		substream->framing = framing;
		substream->clock_type = clock_type;
	}
	return 0;
}
EXPORT_SYMBOL(snd_rawmidi_input_params);

static int snd_rawmidi_output_status(struct snd_rawmidi_substream *substream,
				     struct snd_rawmidi_status64 *status)
{
	struct snd_rawmidi_runtime *runtime = substream->runtime;

	memset(status, 0, sizeof(*status));
	status->stream = SNDRV_RAWMIDI_STREAM_OUTPUT;
	guard(spinlock_irq)(&substream->lock);
	status->avail = runtime->avail;
	return 0;
}

static int snd_rawmidi_input_status(struct snd_rawmidi_substream *substream,
				    struct snd_rawmidi_status64 *status)
{
	struct snd_rawmidi_runtime *runtime = substream->runtime;

	memset(status, 0, sizeof(*status));
	status->stream = SNDRV_RAWMIDI_STREAM_INPUT;
	guard(spinlock_irq)(&substream->lock);
	status->avail = runtime->avail;
	status->xruns = runtime->xruns;
	runtime->xruns = 0;
	return 0;
}

static int snd_rawmidi_ioctl_status32(struct snd_rawmidi_file *rfile,
				      struct snd_rawmidi_status32 __user *argp)
{
	int err = 0;
	struct snd_rawmidi_status32 __user *status = argp;
	struct snd_rawmidi_status32 status32;
	struct snd_rawmidi_status64 status64;

	if (copy_from_user(&status32, argp,
			   sizeof(struct snd_rawmidi_status32)))
		return -EFAULT;

	switch (status32.stream) {
	case SNDRV_RAWMIDI_STREAM_OUTPUT:
		if (rfile->output == NULL)
			return -EINVAL;
		err = snd_rawmidi_output_status(rfile->output, &status64);
		break;
	case SNDRV_RAWMIDI_STREAM_INPUT:
		if (rfile->input == NULL)
			return -EINVAL;
		err = snd_rawmidi_input_status(rfile->input, &status64);
		break;
	default:
		return -EINVAL;
	}
	if (err < 0)
		return err;

	status32 = (struct snd_rawmidi_status32) {
		.stream = status64.stream,
		.tstamp_sec = status64.tstamp_sec,
		.tstamp_nsec = status64.tstamp_nsec,
		.avail = status64.avail,
		.xruns = status64.xruns,
	};

	if (copy_to_user(status, &status32, sizeof(*status)))
		return -EFAULT;

	return 0;
}

static int snd_rawmidi_ioctl_status64(struct snd_rawmidi_file *rfile,
				      struct snd_rawmidi_status64 __user *argp)
{
	int err = 0;
	struct snd_rawmidi_status64 status;

	if (copy_from_user(&status, argp, sizeof(struct snd_rawmidi_status64)))
		return -EFAULT;

	switch (status.stream) {
	case SNDRV_RAWMIDI_STREAM_OUTPUT:
		if (rfile->output == NULL)
			return -EINVAL;
		err = snd_rawmidi_output_status(rfile->output, &status);
		break;
	case SNDRV_RAWMIDI_STREAM_INPUT:
		if (rfile->input == NULL)
			return -EINVAL;
		err = snd_rawmidi_input_status(rfile->input, &status);
		break;
	default:
		return -EINVAL;
	}
	if (err < 0)
		return err;
	if (copy_to_user(argp, &status,
			 sizeof(struct snd_rawmidi_status64)))
		return -EFAULT;
	return 0;
}

static long snd_rawmidi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct snd_rawmidi_file *rfile;
	struct snd_rawmidi *rmidi;
	void __user *argp = (void __user *)arg;

	rfile = file->private_data;
	if (((cmd >> 8) & 0xff) != 'W')
		return -ENOTTY;
	switch (cmd) {
	case SNDRV_RAWMIDI_IOCTL_PVERSION:
		return put_user(SNDRV_RAWMIDI_VERSION, (int __user *)argp) ? -EFAULT : 0;
	case SNDRV_RAWMIDI_IOCTL_INFO:
	{
		int stream;
		struct snd_rawmidi_info __user *info = argp;

		if (get_user(stream, &info->stream))
			return -EFAULT;
		switch (stream) {
		case SNDRV_RAWMIDI_STREAM_INPUT:
			return snd_rawmidi_info_user(rfile->input, info);
		case SNDRV_RAWMIDI_STREAM_OUTPUT:
			return snd_rawmidi_info_user(rfile->output, info);
		default:
			return -EINVAL;
		}
	}
	case SNDRV_RAWMIDI_IOCTL_USER_PVERSION:
		if (get_user(rfile->user_pversion, (unsigned int __user *)arg))
			return -EFAULT;
		return 0;

	case SNDRV_RAWMIDI_IOCTL_PARAMS:
	{
		struct snd_rawmidi_params params;

		if (copy_from_user(&params, argp, sizeof(struct snd_rawmidi_params)))
			return -EFAULT;
		if (rfile->user_pversion < SNDRV_PROTOCOL_VERSION(2, 0, 2)) {
			params.mode = 0;
			memset(params.reserved, 0, sizeof(params.reserved));
		}
		switch (params.stream) {
		case SNDRV_RAWMIDI_STREAM_OUTPUT:
			if (rfile->output == NULL)
				return -EINVAL;
			return snd_rawmidi_output_params(rfile->output, &params);
		case SNDRV_RAWMIDI_STREAM_INPUT:
			if (rfile->input == NULL)
				return -EINVAL;
			return snd_rawmidi_input_params(rfile->input, &params);
		default:
			return -EINVAL;
		}
	}
	case SNDRV_RAWMIDI_IOCTL_STATUS32:
		return snd_rawmidi_ioctl_status32(rfile, argp);
	case SNDRV_RAWMIDI_IOCTL_STATUS64:
		return snd_rawmidi_ioctl_status64(rfile, argp);
	case SNDRV_RAWMIDI_IOCTL_DROP:
	{
		int val;

		if (get_user(val, (int __user *) argp))
			return -EFAULT;
		switch (val) {
		case SNDRV_RAWMIDI_STREAM_OUTPUT:
			if (rfile->output == NULL)
				return -EINVAL;
			return snd_rawmidi_drop_output(rfile->output);
		default:
			return -EINVAL;
		}
	}
	case SNDRV_RAWMIDI_IOCTL_DRAIN:
	{
		int val;

		if (get_user(val, (int __user *) argp))
			return -EFAULT;
		switch (val) {
		case SNDRV_RAWMIDI_STREAM_OUTPUT:
			if (rfile->output == NULL)
				return -EINVAL;
			return snd_rawmidi_drain_output(rfile->output);
		case SNDRV_RAWMIDI_STREAM_INPUT:
			if (rfile->input == NULL)
				return -EINVAL;
			return snd_rawmidi_drain_input(rfile->input);
		default:
			return -EINVAL;
		}
	}
	default:
		rmidi = rfile->rmidi;
		if (rmidi->ops && rmidi->ops->ioctl)
			return rmidi->ops->ioctl(rmidi, cmd, argp);
		rmidi_dbg(rmidi, "rawmidi: unknown command = 0x%x\n", cmd);
	}
	return -ENOTTY;
}

/* ioctl to find the next device; either legacy or UMP depending on @find_ump */
static int snd_rawmidi_next_device(struct snd_card *card, int __user *argp,
				   bool find_ump)

{
	struct snd_rawmidi *rmidi;
	int device;
	bool is_ump;

	if (get_user(device, argp))
		return -EFAULT;
	if (device >= SNDRV_RAWMIDI_DEVICES) /* next device is -1 */
		device = SNDRV_RAWMIDI_DEVICES - 1;
	scoped_guard(mutex, &register_mutex) {
		device = device < 0 ? 0 : device + 1;
		for (; device < SNDRV_RAWMIDI_DEVICES; device++) {
			rmidi = snd_rawmidi_search(card, device);
			if (!rmidi)
				continue;
			is_ump = rawmidi_is_ump(rmidi);
			if (find_ump == is_ump)
				break;
		}
		if (device == SNDRV_RAWMIDI_DEVICES)
			device = -1;
	}
	if (put_user(device, argp))
		return -EFAULT;
	return 0;
}

#if IS_ENABLED(CONFIG_SND_UMP)
/* inquiry of UMP endpoint and block info via control API */
static int snd_rawmidi_call_ump_ioctl(struct snd_card *card, int cmd,
				      void __user *argp)
{
	struct snd_ump_endpoint_info __user *info = argp;
	struct snd_rawmidi *rmidi;
	int device;

	if (get_user(device, &info->device))
		return -EFAULT;
	guard(mutex)(&register_mutex);
	rmidi = snd_rawmidi_search(card, device);
	if (rmidi && rmidi->ops && rmidi->ops->ioctl)
		return rmidi->ops->ioctl(rmidi, cmd, argp);
	else
		return -ENXIO;
}
#endif

static int snd_rawmidi_control_ioctl(struct snd_card *card,
				     struct snd_ctl_file *control,
				     unsigned int cmd,
				     unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE:
		return snd_rawmidi_next_device(card, argp, false);
#if IS_ENABLED(CONFIG_SND_UMP)
	case SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE:
		return snd_rawmidi_next_device(card, argp, true);
	case SNDRV_CTL_IOCTL_UMP_ENDPOINT_INFO:
		return snd_rawmidi_call_ump_ioctl(card, SNDRV_UMP_IOCTL_ENDPOINT_INFO, argp);
	case SNDRV_CTL_IOCTL_UMP_BLOCK_INFO:
		return snd_rawmidi_call_ump_ioctl(card, SNDRV_UMP_IOCTL_BLOCK_INFO, argp);
#endif
	case SNDRV_CTL_IOCTL_RAWMIDI_PREFER_SUBDEVICE:
	{
		int val;

		if (get_user(val, (int __user *)argp))
			return -EFAULT;
		control->preferred_subdevice[SND_CTL_SUBDEV_RAWMIDI] = val;
		return 0;
	}
	case SNDRV_CTL_IOCTL_RAWMIDI_INFO:
		return snd_rawmidi_info_select_user(card, argp);
	}
	return -ENOIOCTLCMD;
}

static int receive_with_tstamp_framing(struct snd_rawmidi_substream *substream,
			const unsigned char *buffer, int src_count, const struct timespec64 *tstamp)
{
	struct snd_rawmidi_runtime *runtime = substream->runtime;
	struct snd_rawmidi_framing_tstamp *dest_ptr;
	struct snd_rawmidi_framing_tstamp frame = { .tv_sec = tstamp->tv_sec, .tv_nsec = tstamp->tv_nsec };
	int orig_count = src_count;
	int frame_size = sizeof(struct snd_rawmidi_framing_tstamp);
	int align = get_align(runtime);

	BUILD_BUG_ON(frame_size != 0x20);
	if (snd_BUG_ON((runtime->hw_ptr & 0x1f) != 0))
		return -EINVAL;

	while (src_count > align) {
		if ((int)(runtime->buffer_size - runtime->avail) < frame_size) {
			runtime->xruns += src_count;
			break;
		}
		if (src_count >= SNDRV_RAWMIDI_FRAMING_DATA_LENGTH)
			frame.length = SNDRV_RAWMIDI_FRAMING_DATA_LENGTH;
		else {
			frame.length = get_aligned_size(runtime, src_count);
			if (!frame.length)
				break;
			memset(frame.data, 0, SNDRV_RAWMIDI_FRAMING_DATA_LENGTH);
		}
		memcpy(frame.data, buffer, frame.length);
		buffer += frame.length;
		src_count -= frame.length;
		dest_ptr = (struct snd_rawmidi_framing_tstamp *) (runtime->buffer + runtime->hw_ptr);
		*dest_ptr = frame;
		runtime->avail += frame_size;
		runtime->hw_ptr += frame_size;
		runtime->hw_ptr %= runtime->buffer_size;
	}
	return orig_count - src_count;
}

static struct timespec64 get_framing_tstamp(struct snd_rawmidi_substream *substream)
{
	struct timespec64 ts64 = {0, 0};

	switch (substream->clock_type) {
	case SNDRV_RAWMIDI_MODE_CLOCK_MONOTONIC_RAW:
		ktime_get_raw_ts64(&ts64);
		break;
	case SNDRV_RAWMIDI_MODE_CLOCK_MONOTONIC:
		ktime_get_ts64(&ts64);
		break;
	case SNDRV_RAWMIDI_MODE_CLOCK_REALTIME:
		ktime_get_real_ts64(&ts64);
		break;
	}
	return ts64;
}

/**
 * snd_rawmidi_receive - receive the input data from the device
 * @substream: the rawmidi substream
 * @buffer: the buffer pointer
 * @count: the data size to read
 *
 * Reads the data from the internal buffer.
 *
 * Return: The size of read data, or a negative error code on failure.
 */
int snd_rawmidi_receive(struct snd_rawmidi_substream *substream,
			const unsigned char *buffer, int count)
{
	struct timespec64 ts64 = get_framing_tstamp(substream);
	int result = 0, count1;
	struct snd_rawmidi_runtime *runtime;

	guard(spinlock_irqsave)(&substream->lock);
	if (!substream->opened)
		return -EBADFD;
	runtime = substream->runtime;
	if (!runtime || !runtime->buffer) {
		rmidi_dbg(substream->rmidi,
			  "snd_rawmidi_receive: input is not active!!!\n");
		return -EINVAL;
	}

	count = get_aligned_size(runtime, count);
	if (!count)
		return result;

	if (substream->framing == SNDRV_RAWMIDI_MODE_FRAMING_TSTAMP) {
		result = receive_with_tstamp_framing(substream, buffer, count, &ts64);
	} else if (count == 1) {	/* special case, faster code */
		substream->bytes++;
		if (runtime->avail < runtime->buffer_size) {
			runtime->buffer[runtime->hw_ptr++] = buffer[0];
			runtime->hw_ptr %= runtime->buffer_size;
			runtime->avail++;
			result++;
		} else {
			runtime->xruns++;
		}
	} else {
		substream->bytes += count;
		count1 = runtime->buffer_size - runtime->hw_ptr;
		if (count1 > count)
			count1 = count;
		if (count1 > (int)(runtime->buffer_size - runtime->avail))
			count1 = runtime->buffer_size - runtime->avail;
		count1 = get_aligned_size(runtime, count1);
		if (!count1)
			return result;
		memcpy(runtime->buffer + runtime->hw_ptr, buffer, count1);
		runtime->hw_ptr += count1;
		runtime->hw_ptr %= runtime->buffer_size;
		runtime->avail += count1;
		count -= count1;
		result += count1;
		if (count > 0) {
			buffer += count1;
			count1 = count;
			if (count1 > (int)(runtime->buffer_size - runtime->avail)) {
				count1 = runtime->buffer_size - runtime->avail;
				runtime->xruns += count - count1;
			}
			if (count1 > 0) {
				memcpy(runtime->buffer, buffer, count1);
				runtime->hw_ptr = count1;
				runtime->avail += count1;
				result += count1;
			}
		}
	}
	if (result > 0) {
		if (runtime->event)
			schedule_work(&runtime->event_work);
		else if (__snd_rawmidi_ready(runtime))
			wake_up(&runtime->sleep);
	}
	return result;
}
EXPORT_SYMBOL(snd_rawmidi_receive);

static long snd_rawmidi_kernel_read1(struct snd_rawmidi_substream *substream,
				     unsigned char __user *userbuf,
				     unsigned char *kernelbuf, long count)
{
	unsigned long flags;
	long result = 0, count1;
	struct snd_rawmidi_runtime *runtime = substream->runtime;
	unsigned long appl_ptr;
	int err = 0;

	spin_lock_irqsave(&substream->lock, flags);
	snd_rawmidi_buffer_ref(runtime);
	while (count > 0 && runtime->avail) {
		count1 = runtime->buffer_size - runtime->appl_ptr;
		if (count1 > count)
			count1 = count;
		if (count1 > (int)runtime->avail)
			count1 = runtime->avail;

		/* update runtime->appl_ptr before unlocking for userbuf */
		appl_ptr = runtime->appl_ptr;
		runtime->appl_ptr += count1;
		runtime->appl_ptr %= runtime->buffer_size;
		runtime->avail -= count1;

		if (kernelbuf)
			memcpy(kernelbuf + result, runtime->buffer + appl_ptr, count1);
		if (userbuf) {
			spin_unlock_irqrestore(&substream->lock, flags);
			if (copy_to_user(userbuf + result,
					 runtime->buffer + appl_ptr, count1))
				err = -EFAULT;
			spin_lock_irqsave(&substream->lock, flags);
			if (err)
				goto out;
		}
		result += count1;
		count -= count1;
	}
 out:
	snd_rawmidi_buffer_unref(runtime);
	spin_unlock_irqrestore(&substream->lock, flags);
	return result > 0 ? result : err;
}

long snd_rawmidi_kernel_read(struct snd_rawmidi_substream *substream,
			     unsigned char *buf, long count)
{
	snd_rawmidi_input_trigger(substream, 1);
	return snd_rawmidi_kernel_read1(substream, NULL/*userbuf*/, buf, count);
}
EXPORT_SYMBOL(snd_rawmidi_kernel_read);

static ssize_t snd_rawmidi_read(struct file *file, char __user *buf, size_t count,
				loff_t *offset)
{
	long result;
	int count1;
	struct snd_rawmidi_file *rfile;
	struct snd_rawmidi_substream *substream;
	struct snd_rawmidi_runtime *runtime;

	rfile = file->private_data;
	substream = rfile->input;
	if (substream == NULL)
		return -EIO;
	runtime = substream->runtime;
	snd_rawmidi_input_trigger(substream, 1);
	result = 0;
	while (count > 0) {
		spin_lock_irq(&substream->lock);
		while (!__snd_rawmidi_ready(runtime)) {
			wait_queue_entry_t wait;

			if ((file->f_flags & O_NONBLOCK) != 0 || result > 0) {
				spin_unlock_irq(&substream->lock);
				return result > 0 ? result : -EAGAIN;
			}
			init_waitqueue_entry(&wait, current);
			add_wait_queue(&runtime->sleep, &wait);
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&substream->lock);
			schedule();
			remove_wait_queue(&runtime->sleep, &wait);
			if (rfile->rmidi->card->shutdown)
				return -ENODEV;
			if (signal_pending(current))
				return result > 0 ? result : -ERESTARTSYS;
			spin_lock_irq(&substream->lock);
			if (!runtime->avail) {
				spin_unlock_irq(&substream->lock);
				return result > 0 ? result : -EIO;
			}
		}
		spin_unlock_irq(&substream->lock);
		count1 = snd_rawmidi_kernel_read1(substream,
						  (unsigned char __user *)buf,
						  NULL/*kernelbuf*/,
						  count);
		if (count1 < 0)
			return result > 0 ? result : count1;
		result += count1;
		buf += count1;
		count -= count1;
	}
	return result;
}

/**
 * snd_rawmidi_transmit_empty - check whether the output buffer is empty
 * @substream: the rawmidi substream
 *
 * Return: 1 if the internal output buffer is empty, 0 if not.
 */
int snd_rawmidi_transmit_empty(struct snd_rawmidi_substream *substream)
{
	struct snd_rawmidi_runtime *runtime;

	guard(spinlock_irqsave)(&substream->lock);
	runtime = substream->runtime;
	if (!substream->opened || !runtime || !runtime->buffer) {
		rmidi_dbg(substream->rmidi,
			  "snd_rawmidi_transmit_empty: output is not active!!!\n");
		return 1;
	}
	return (runtime->avail >= runtime->buffer_size);
}
EXPORT_SYMBOL(snd_rawmidi_transmit_empty);

/*
 * __snd_rawmidi_transmit_peek - copy data from the internal buffer
 * @substream: the rawmidi substream
 * @buffer: the buffer pointer
 * @count: data size to transfer
 *
 * This is a variant of snd_rawmidi_transmit_peek() without spinlock.
 */
static int __snd_rawmidi_transmit_peek(struct snd_rawmidi_substream *substream,
				       unsigned char *buffer, int count)
{
	int result, count1;
	struct snd_rawmidi_runtime *runtime = substream->runtime;

	if (runtime->buffer == NULL) {
		rmidi_dbg(substream->rmidi,
			  "snd_rawmidi_transmit_peek: output is not active!!!\n");
		return -EINVAL;
	}
	result = 0;
	if (runtime->avail >= runtime->buffer_size) {
		/* warning: lowlevel layer MUST trigger down the hardware */
		goto __skip;
	}
	if (count == 1) {	/* special case, faster code */
		*buffer = runtime->buffer[runtime->hw_ptr];
		result++;
	} else {
		count1 = runtime->buffer_size - runtime->hw_ptr;
		if (count1 > count)
			count1 = count;
		if (count1 > (int)(runtime->buffer_size - runtime->avail))
			count1 = runtime->buffer_size - runtime->avail;
		count1 = get_aligned_size(runtime, count1);
		if (!count1)
			goto __skip;
		memcpy(buffer, runtime->buffer + runtime->hw_ptr, count1);
		count -= count1;
		result += count1;
		if (count > 0) {
			if (count > (int)(runtime->buffer_size - runtime->avail - count1))
				count = runtime->buffer_size - runtime->avail - count1;
			count = get_aligned_size(runtime, count);
			if (!count)
				goto __skip;
			memcpy(buffer + count1, runtime->buffer, count);
			result += count;
		}
	}
      __skip:
	return result;
}

/**
 * snd_rawmidi_transmit_peek - copy data from the internal buffer
 * @substream: the rawmidi substream
 * @buffer: the buffer pointer
 * @count: data size to transfer
 *
 * Copies data from the internal output buffer to the given buffer.
 *
 * Call this in the interrupt handler when the midi output is ready,
 * and call snd_rawmidi_transmit_ack() after the transmission is
 * finished.
 *
 * Return: The size of copied data, or a negative error code on failure.
 */
int snd_rawmidi_transmit_peek(struct snd_rawmidi_substream *substream,
			      unsigned char *buffer, int count)
{
	guard(spinlock_irqsave)(&substream->lock);
	if (!substream->opened || !substream->runtime)
		return -EBADFD;
	return __snd_rawmidi_transmit_peek(substream, buffer, count);
}
EXPORT_SYMBOL(snd_rawmidi_transmit_peek);

/*
 * __snd_rawmidi_transmit_ack - acknowledge the transmission
 * @substream: the rawmidi substream
 * @count: the transferred count
 *
 * This is a variant of __snd_rawmidi_transmit_ack() without spinlock.
 */
static int __snd_rawmidi_transmit_ack(struct snd_rawmidi_substream *substream,
				      int count)
{
	struct snd_rawmidi_runtime *runtime = substream->runtime;

	if (runtime->buffer == NULL) {
		rmidi_dbg(substream->rmidi,
			  "snd_rawmidi_transmit_ack: output is not active!!!\n");
		return -EINVAL;
	}
	snd_BUG_ON(runtime->avail + count > runtime->buffer_size);
	count = get_aligned_size(runtime, count);
	runtime->hw_ptr += count;
	runtime->hw_ptr %= runtime->buffer_size;
	runtime->avail += count;
	substream->bytes += count;
	if (count > 0) {
		if (runtime->drain || __snd_rawmidi_ready(runtime))
			wake_up(&runtime->sleep);
	}
	return count;
}

/**
 * snd_rawmidi_transmit_ack - acknowledge the transmission
 * @substream: the rawmidi substream
 * @count: the transferred count
 *
 * Advances the hardware pointer for the internal output buffer with
 * the given size and updates the condition.
 * Call after the transmission is finished.
 *
 * Return: The advanced size if successful, or a negative error code on failure.
 */
int snd_rawmidi_transmit_ack(struct snd_rawmidi_substream *substream, int count)
{
	guard(spinlock_irqsave)(&substream->lock);
	if (!substream->opened || !substream->runtime)
		return -EBADFD;
	return __snd_rawmidi_transmit_ack(substream, count);
}
EXPORT_SYMBOL(snd_rawmidi_transmit_ack);

/**
 * snd_rawmidi_transmit - copy from the buffer to the device
 * @substream: the rawmidi substream
 * @buffer: the buffer pointer
 * @count: the data size to transfer
 *
 * Copies data from the buffer to the device and advances the pointer.
 *
 * Return: The copied size if successful, or a negative error code on failure.
 */
int snd_rawmidi_transmit(struct snd_rawmidi_substream *substream,
			 unsigned char *buffer, int count)
{
	guard(spinlock_irqsave)(&substream->lock);
	if (!substream->opened)
		return -EBADFD;
	count = __snd_rawmidi_transmit_peek(substream, buffer, count);
	if (count <= 0)
		return count;
	return __snd_rawmidi_transmit_ack(substream, count);
}
EXPORT_SYMBOL(snd_rawmidi_transmit);

/**
 * snd_rawmidi_proceed - Discard the all pending bytes and proceed
 * @substream: rawmidi substream
 *
 * Return: the number of discarded bytes
 */
int snd_rawmidi_proceed(struct snd_rawmidi_substream *substream)
{
	struct snd_rawmidi_runtime *runtime;
	int count = 0;

	guard(spinlock_irqsave)(&substream->lock);
	runtime = substream->runtime;
	if (substream->opened && runtime &&
	    runtime->avail < runtime->buffer_size) {
		count = runtime->buffer_size - runtime->avail;
		__snd_rawmidi_transmit_ack(substream, count);
	}
	return count;
}
EXPORT_SYMBOL(snd_rawmidi_proceed);

static long snd_rawmidi_kernel_write1(struct snd_rawmidi_substream *substream,
				      const unsigned char __user *userbuf,
				      const unsigned char *kernelbuf,
				      long count)
{
	unsigned long flags;
	long count1, result;
	struct snd_rawmidi_runtime *runtime = substream->runtime;
	unsigned long appl_ptr;

	if (!kernelbuf && !userbuf)
		return -EINVAL;
	if (snd_BUG_ON(!runtime->buffer))
		return -EINVAL;

	result = 0;
	spin_lock_irqsave(&substream->lock, flags);
	if (substream->append) {
		if ((long)runtime->avail < count) {
			spin_unlock_irqrestore(&substream->lock, flags);
			return -EAGAIN;
		}
	}
	snd_rawmidi_buffer_ref(runtime);
	while (count > 0 && runtime->avail > 0) {
		count1 = runtime->buffer_size - runtime->appl_ptr;
		if (count1 > count)
			count1 = count;
		if (count1 > (long)runtime->avail)
			count1 = runtime->avail;

		/* update runtime->appl_ptr before unlocking for userbuf */
		appl_ptr = runtime->appl_ptr;
		runtime->appl_ptr += count1;
		runtime->appl_ptr %= runtime->buffer_size;
		runtime->avail -= count1;

		if (kernelbuf)
			memcpy(runtime->buffer + appl_ptr,
			       kernelbuf + result, count1);
		else if (userbuf) {
			spin_unlock_irqrestore(&substream->lock, flags);
			if (copy_from_user(runtime->buffer + appl_ptr,
					   userbuf + result, count1)) {
				spin_lock_irqsave(&substream->lock, flags);
				result = result > 0 ? result : -EFAULT;
				goto __end;
			}
			spin_lock_irqsave(&substream->lock, flags);
		}
		result += count1;
		count -= count1;
	}
      __end:
	count1 = runtime->avail < runtime->buffer_size;
	snd_rawmidi_buffer_unref(runtime);
	spin_unlock_irqrestore(&substream->lock, flags);
	if (count1)
		snd_rawmidi_output_trigger(substream, 1);
	return result;
}

long snd_rawmidi_kernel_write(struct snd_rawmidi_substream *substream,
			      const unsigned char *buf, long count)
{
	return snd_rawmidi_kernel_write1(substream, NULL, buf, count);
}
EXPORT_SYMBOL(snd_rawmidi_kernel_write);

static ssize_t snd_rawmidi_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *offset)
{
	long result, timeout;
	int count1;
	struct snd_rawmidi_file *rfile;
	struct snd_rawmidi_runtime *runtime;
	struct snd_rawmidi_substream *substream;

	rfile = file->private_data;
	substream = rfile->output;
	runtime = substream->runtime;
	/* we cannot put an atomic message to our buffer */
	if (substream->append && count > runtime->buffer_size)
		return -EIO;
	result = 0;
	while (count > 0) {
		spin_lock_irq(&substream->lock);
		while (!snd_rawmidi_ready_append(substream, count)) {
			wait_queue_entry_t wait;

			if (file->f_flags & O_NONBLOCK) {
				spin_unlock_irq(&substream->lock);
				return result > 0 ? result : -EAGAIN;
			}
			init_waitqueue_entry(&wait, current);
			add_wait_queue(&runtime->sleep, &wait);
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&substream->lock);
			timeout = schedule_timeout(30 * HZ);
			remove_wait_queue(&runtime->sleep, &wait);
			if (rfile->rmidi->card->shutdown)
				return -ENODEV;
			if (signal_pending(current))
				return result > 0 ? result : -ERESTARTSYS;
			spin_lock_irq(&substream->lock);
			if (!runtime->avail && !timeout) {
				spin_unlock_irq(&substream->lock);
				return result > 0 ? result : -EIO;
			}
		}
		spin_unlock_irq(&substream->lock);
		count1 = snd_rawmidi_kernel_write1(substream, buf, NULL, count);
		if (count1 < 0)
			return result > 0 ? result : count1;
		result += count1;
		buf += count1;
		if ((size_t)count1 < count && (file->f_flags & O_NONBLOCK))
			break;
		count -= count1;
	}
	if (file->f_flags & O_DSYNC) {
		spin_lock_irq(&substream->lock);
		while (runtime->avail != runtime->buffer_size) {
			wait_queue_entry_t wait;
			unsigned int last_avail = runtime->avail;

			init_waitqueue_entry(&wait, current);
			add_wait_queue(&runtime->sleep, &wait);
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&substream->lock);
			timeout = schedule_timeout(30 * HZ);
			remove_wait_queue(&runtime->sleep, &wait);
			if (signal_pending(current))
				return result > 0 ? result : -ERESTARTSYS;
			if (runtime->avail == last_avail && !timeout)
				return result > 0 ? result : -EIO;
			spin_lock_irq(&substream->lock);
		}
		spin_unlock_irq(&substream->lock);
	}
	return result;
}

static __poll_t snd_rawmidi_poll(struct file *file, poll_table *wait)
{
	struct snd_rawmidi_file *rfile;
	struct snd_rawmidi_runtime *runtime;
	__poll_t mask;

	rfile = file->private_data;
	if (rfile->input != NULL) {
		runtime = rfile->input->runtime;
		snd_rawmidi_input_trigger(rfile->input, 1);
		poll_wait(file, &runtime->sleep, wait);
	}
	if (rfile->output != NULL) {
		runtime = rfile->output->runtime;
		poll_wait(file, &runtime->sleep, wait);
	}
	mask = 0;
	if (rfile->input != NULL) {
		if (snd_rawmidi_ready(rfile->input))
			mask |= EPOLLIN | EPOLLRDNORM;
	}
	if (rfile->output != NULL) {
		if (snd_rawmidi_ready(rfile->output))
			mask |= EPOLLOUT | EPOLLWRNORM;
	}
	return mask;
}

/*
 */
#ifdef CONFIG_COMPAT
#include "rawmidi_compat.c"
#else
#define snd_rawmidi_ioctl_compat	NULL
#endif

/*
 */

static void snd_rawmidi_proc_info_read(struct snd_info_entry *entry,
				       struct snd_info_buffer *buffer)
{
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_substream *substream;
	struct snd_rawmidi_runtime *runtime;
	unsigned long buffer_size, avail, xruns;
	unsigned int clock_type;
	static const char *clock_names[4] = { "none", "realtime", "monotonic", "monotonic raw" };

	rmidi = entry->private_data;
	snd_iprintf(buffer, "%s\n\n", rmidi->name);
	if (IS_ENABLED(CONFIG_SND_UMP))
		snd_iprintf(buffer, "Type: %s\n",
			    rawmidi_is_ump(rmidi) ? "UMP" : "Legacy");
	if (rmidi->ops && rmidi->ops->proc_read)
		rmidi->ops->proc_read(entry, buffer);
	guard(mutex)(&rmidi->open_mutex);
	if (rmidi->info_flags & SNDRV_RAWMIDI_INFO_OUTPUT) {
		list_for_each_entry(substream,
				    &rmidi->streams[SNDRV_RAWMIDI_STREAM_OUTPUT].substreams,
				    list) {
			snd_iprintf(buffer,
				    "Output %d\n"
				    "  Tx bytes     : %lu\n",
				    substream->number,
				    (unsigned long) substream->bytes);
			if (substream->opened) {
				snd_iprintf(buffer,
				    "  Owner PID    : %d\n",
				    pid_vnr(substream->pid));
				runtime = substream->runtime;
				scoped_guard(spinlock_irq, &substream->lock) {
					buffer_size = runtime->buffer_size;
					avail = runtime->avail;
				}
				snd_iprintf(buffer,
				    "  Mode         : %s\n"
				    "  Buffer size  : %lu\n"
				    "  Avail        : %lu\n",
				    runtime->oss ? "OSS compatible" : "native",
				    buffer_size, avail);
			}
		}
	}
	if (rmidi->info_flags & SNDRV_RAWMIDI_INFO_INPUT) {
		list_for_each_entry(substream,
				    &rmidi->streams[SNDRV_RAWMIDI_STREAM_INPUT].substreams,
				    list) {
			snd_iprintf(buffer,
				    "Input %d\n"
				    "  Rx bytes     : %lu\n",
				    substream->number,
				    (unsigned long) substream->bytes);
			if (substream->opened) {
				snd_iprintf(buffer,
					    "  Owner PID    : %d\n",
					    pid_vnr(substream->pid));
				runtime = substream->runtime;
				scoped_guard(spinlock_irq, &substream->lock) {
					buffer_size = runtime->buffer_size;
					avail = runtime->avail;
					xruns = runtime->xruns;
				}
				snd_iprintf(buffer,
					    "  Buffer size  : %lu\n"
					    "  Avail        : %lu\n"
					    "  Overruns     : %lu\n",
					    buffer_size, avail, xruns);
				if (substream->framing == SNDRV_RAWMIDI_MODE_FRAMING_TSTAMP) {
					clock_type = substream->clock_type >> SNDRV_RAWMIDI_MODE_CLOCK_SHIFT;
					if (!snd_BUG_ON(clock_type >= ARRAY_SIZE(clock_names)))
						snd_iprintf(buffer,
							    "  Framing      : tstamp\n"
							    "  Clock type   : %s\n",
							    clock_names[clock_type]);
				}
			}
		}
	}
}

/*
 *  Register functions
 */

static const struct file_operations snd_rawmidi_f_ops = {
	.owner =	THIS_MODULE,
	.read =		snd_rawmidi_read,
	.write =	snd_rawmidi_write,
	.open =		snd_rawmidi_open,
	.release =	snd_rawmidi_release,
	.poll =		snd_rawmidi_poll,
	.unlocked_ioctl =	snd_rawmidi_ioctl,
	.compat_ioctl =	snd_rawmidi_ioctl_compat,
};

static int snd_rawmidi_alloc_substreams(struct snd_rawmidi *rmidi,
					struct snd_rawmidi_str *stream,
					int direction,
					int count)
{
	struct snd_rawmidi_substream *substream;
	int idx;

	for (idx = 0; idx < count; idx++) {
		substream = kzalloc(sizeof(*substream), GFP_KERNEL);
		if (!substream)
			return -ENOMEM;
		substream->stream = direction;
		substream->number = idx;
		substream->rmidi = rmidi;
		substream->pstr = stream;
		spin_lock_init(&substream->lock);
		list_add_tail(&substream->list, &stream->substreams);
		stream->substream_count++;
	}
	return 0;
}

/* used for both rawmidi and ump */
int snd_rawmidi_init(struct snd_rawmidi *rmidi,
		     struct snd_card *card, char *id, int device,
		     int output_count, int input_count,
		     unsigned int info_flags)
{
	int err;
	static const struct snd_device_ops ops = {
		.dev_free = snd_rawmidi_dev_free,
		.dev_register = snd_rawmidi_dev_register,
		.dev_disconnect = snd_rawmidi_dev_disconnect,
	};

	rmidi->card = card;
	rmidi->device = device;
	mutex_init(&rmidi->open_mutex);
	init_waitqueue_head(&rmidi->open_wait);
	INIT_LIST_HEAD(&rmidi->streams[SNDRV_RAWMIDI_STREAM_INPUT].substreams);
	INIT_LIST_HEAD(&rmidi->streams[SNDRV_RAWMIDI_STREAM_OUTPUT].substreams);
	rmidi->info_flags = info_flags;

	if (id != NULL)
		strscpy(rmidi->id, id, sizeof(rmidi->id));

	err = snd_device_alloc(&rmidi->dev, card);
	if (err < 0)
		return err;
	if (rawmidi_is_ump(rmidi))
		dev_set_name(rmidi->dev, "umpC%iD%i", card->number, device);
	else
		dev_set_name(rmidi->dev, "midiC%iD%i", card->number, device);

	err = snd_rawmidi_alloc_substreams(rmidi,
					   &rmidi->streams[SNDRV_RAWMIDI_STREAM_INPUT],
					   SNDRV_RAWMIDI_STREAM_INPUT,
					   input_count);
	if (err < 0)
		return err;
	err = snd_rawmidi_alloc_substreams(rmidi,
					   &rmidi->streams[SNDRV_RAWMIDI_STREAM_OUTPUT],
					   SNDRV_RAWMIDI_STREAM_OUTPUT,
					   output_count);
	if (err < 0)
		return err;
	err = snd_device_new(card, SNDRV_DEV_RAWMIDI, rmidi, &ops);
	if (err < 0)
		return err;
	return 0;
}
EXPORT_SYMBOL_GPL(snd_rawmidi_init);

/**
 * snd_rawmidi_new - create a rawmidi instance
 * @card: the card instance
 * @id: the id string
 * @device: the device index
 * @output_count: the number of output streams
 * @input_count: the number of input streams
 * @rrawmidi: the pointer to store the new rawmidi instance
 *
 * Creates a new rawmidi instance.
 * Use snd_rawmidi_set_ops() to set the operators to the new instance.
 *
 * Return: Zero if successful, or a negative error code on failure.
 */
int snd_rawmidi_new(struct snd_card *card, char *id, int device,
		    int output_count, int input_count,
		    struct snd_rawmidi **rrawmidi)
{
	struct snd_rawmidi *rmidi;
	int err;

	if (rrawmidi)
		*rrawmidi = NULL;
	rmidi = kzalloc(sizeof(*rmidi), GFP_KERNEL);
	if (!rmidi)
		return -ENOMEM;
	err = snd_rawmidi_init(rmidi, card, id, device,
			       output_count, input_count, 0);
	if (err < 0) {
		snd_rawmidi_free(rmidi);
		return err;
	}
	if (rrawmidi)
		*rrawmidi = rmidi;
	return 0;
}
EXPORT_SYMBOL(snd_rawmidi_new);

static void snd_rawmidi_free_substreams(struct snd_rawmidi_str *stream)
{
	struct snd_rawmidi_substream *substream;

	while (!list_empty(&stream->substreams)) {
		substream = list_entry(stream->substreams.next, struct snd_rawmidi_substream, list);
		list_del(&substream->list);
		kfree(substream);
	}
}

/* called from ump.c, too */
int snd_rawmidi_free(struct snd_rawmidi *rmidi)
{
	if (!rmidi)
		return 0;

	snd_info_free_entry(rmidi->proc_entry);
	rmidi->proc_entry = NULL;
	if (rmidi->ops && rmidi->ops->dev_unregister)
		rmidi->ops->dev_unregister(rmidi);

	snd_rawmidi_free_substreams(&rmidi->streams[SNDRV_RAWMIDI_STREAM_INPUT]);
	snd_rawmidi_free_substreams(&rmidi->streams[SNDRV_RAWMIDI_STREAM_OUTPUT]);
	if (rmidi->private_free)
		rmidi->private_free(rmidi);
	put_device(rmidi->dev);
	kfree(rmidi);
	return 0;
}
EXPORT_SYMBOL_GPL(snd_rawmidi_free);

static int snd_rawmidi_dev_free(struct snd_device *device)
{
	struct snd_rawmidi *rmidi = device->device_data;

	return snd_rawmidi_free(rmidi);
}

#if IS_ENABLED(CONFIG_SND_SEQUENCER)
static void snd_rawmidi_dev_seq_free(struct snd_seq_device *device)
{
	struct snd_rawmidi *rmidi = device->private_data;

	rmidi->seq_dev = NULL;
}
#endif

static int snd_rawmidi_dev_register(struct snd_device *device)
{
	int err;
	struct snd_info_entry *entry;
	char name[16];
	struct snd_rawmidi *rmidi = device->device_data;

	if (rmidi->device >= SNDRV_RAWMIDI_DEVICES)
		return -ENOMEM;
	err = 0;
	scoped_guard(mutex, &register_mutex) {
		if (snd_rawmidi_search(rmidi->card, rmidi->device))
			err = -EBUSY;
		else
			list_add_tail(&rmidi->list, &snd_rawmidi_devices);
	}
	if (err < 0)
		return err;

	err = snd_register_device(SNDRV_DEVICE_TYPE_RAWMIDI,
				  rmidi->card, rmidi->device,
				  &snd_rawmidi_f_ops, rmidi, rmidi->dev);
	if (err < 0) {
		rmidi_err(rmidi, "unable to register\n");
		goto error;
	}
	if (rmidi->ops && rmidi->ops->dev_register) {
		err = rmidi->ops->dev_register(rmidi);
		if (err < 0)
			goto error_unregister;
	}
#ifdef CONFIG_SND_OSSEMUL
	rmidi->ossreg = 0;
	if (!rawmidi_is_ump(rmidi) &&
	    (int)rmidi->device == midi_map[rmidi->card->number]) {
		if (snd_register_oss_device(SNDRV_OSS_DEVICE_TYPE_MIDI,
					    rmidi->card, 0, &snd_rawmidi_f_ops,
					    rmidi) < 0) {
			rmidi_err(rmidi,
				  "unable to register OSS rawmidi device %i:%i\n",
				  rmidi->card->number, 0);
		} else {
			rmidi->ossreg++;
#ifdef SNDRV_OSS_INFO_DEV_MIDI
			snd_oss_info_register(SNDRV_OSS_INFO_DEV_MIDI, rmidi->card->number, rmidi->name);
#endif
		}
	}
	if (!rawmidi_is_ump(rmidi) &&
	    (int)rmidi->device == amidi_map[rmidi->card->number]) {
		if (snd_register_oss_device(SNDRV_OSS_DEVICE_TYPE_MIDI,
					    rmidi->card, 1, &snd_rawmidi_f_ops,
					    rmidi) < 0) {
			rmidi_err(rmidi,
				  "unable to register OSS rawmidi device %i:%i\n",
				  rmidi->card->number, 1);
		} else {
			rmidi->ossreg++;
		}
	}
#endif /* CONFIG_SND_OSSEMUL */
	sprintf(name, "midi%d", rmidi->device);
	entry = snd_info_create_card_entry(rmidi->card, name, rmidi->card->proc_root);
	if (entry) {
		entry->private_data = rmidi;
		entry->c.text.read = snd_rawmidi_proc_info_read;
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			entry = NULL;
		}
	}
	rmidi->proc_entry = entry;
#if IS_ENABLED(CONFIG_SND_SEQUENCER)
	/* no own registration mechanism? */
	if (!rmidi->ops || !rmidi->ops->dev_register) {
		if (snd_seq_device_new(rmidi->card, rmidi->device, SNDRV_SEQ_DEV_ID_MIDISYNTH, 0, &rmidi->seq_dev) >= 0) {
			rmidi->seq_dev->private_data = rmidi;
			rmidi->seq_dev->private_free = snd_rawmidi_dev_seq_free;
			sprintf(rmidi->seq_dev->name, "MIDI %d-%d", rmidi->card->number, rmidi->device);
			snd_device_register(rmidi->card, rmidi->seq_dev);
		}
	}
#endif
	return 0;

 error_unregister:
	snd_unregister_device(rmidi->dev);
 error:
	scoped_guard(mutex, &register_mutex)
		list_del(&rmidi->list);
	return err;
}

static int snd_rawmidi_dev_disconnect(struct snd_device *device)
{
	struct snd_rawmidi *rmidi = device->device_data;
	int dir;

	guard(mutex)(&register_mutex);
	guard(mutex)(&rmidi->open_mutex);
	wake_up(&rmidi->open_wait);
	list_del_init(&rmidi->list);
	for (dir = 0; dir < 2; dir++) {
		struct snd_rawmidi_substream *s;

		list_for_each_entry(s, &rmidi->streams[dir].substreams, list) {
			if (s->runtime)
				wake_up(&s->runtime->sleep);
		}
	}

#ifdef CONFIG_SND_OSSEMUL
	if (rmidi->ossreg) {
		if ((int)rmidi->device == midi_map[rmidi->card->number]) {
			snd_unregister_oss_device(SNDRV_OSS_DEVICE_TYPE_MIDI, rmidi->card, 0);
#ifdef SNDRV_OSS_INFO_DEV_MIDI
			snd_oss_info_unregister(SNDRV_OSS_INFO_DEV_MIDI, rmidi->card->number);
#endif
		}
		if ((int)rmidi->device == amidi_map[rmidi->card->number])
			snd_unregister_oss_device(SNDRV_OSS_DEVICE_TYPE_MIDI, rmidi->card, 1);
		rmidi->ossreg = 0;
	}
#endif /* CONFIG_SND_OSSEMUL */
	snd_unregister_device(rmidi->dev);
	return 0;
}

/**
 * snd_rawmidi_set_ops - set the rawmidi operators
 * @rmidi: the rawmidi instance
 * @stream: the stream direction, SNDRV_RAWMIDI_STREAM_XXX
 * @ops: the operator table
 *
 * Sets the rawmidi operators for the given stream direction.
 */
void snd_rawmidi_set_ops(struct snd_rawmidi *rmidi, int stream,
			 const struct snd_rawmidi_ops *ops)
{
	struct snd_rawmidi_substream *substream;

	list_for_each_entry(substream, &rmidi->streams[stream].substreams, list)
		substream->ops = ops;
}
EXPORT_SYMBOL(snd_rawmidi_set_ops);

/*
 *  ENTRY functions
 */

static int __init alsa_rawmidi_init(void)
{

	snd_ctl_register_ioctl(snd_rawmidi_control_ioctl);
	snd_ctl_register_ioctl_compat(snd_rawmidi_control_ioctl);
#ifdef CONFIG_SND_OSSEMUL
	{ int i;
	/* check device map table */
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (midi_map[i] < 0 || midi_map[i] >= SNDRV_RAWMIDI_DEVICES) {
			pr_err("ALSA: rawmidi: invalid midi_map[%d] = %d\n",
			       i, midi_map[i]);
			midi_map[i] = 0;
		}
		if (amidi_map[i] < 0 || amidi_map[i] >= SNDRV_RAWMIDI_DEVICES) {
			pr_err("ALSA: rawmidi: invalid amidi_map[%d] = %d\n",
			       i, amidi_map[i]);
			amidi_map[i] = 1;
		}
	}
	}
#endif /* CONFIG_SND_OSSEMUL */
	return 0;
}

static void __exit alsa_rawmidi_exit(void)
{
	snd_ctl_unregister_ioctl(snd_rawmidi_control_ioctl);
	snd_ctl_unregister_ioctl_compat(snd_rawmidi_control_ioctl);
}

module_init(alsa_rawmidi_init)
module_exit(alsa_rawmidi_exit)
