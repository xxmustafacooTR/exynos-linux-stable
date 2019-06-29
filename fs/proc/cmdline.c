#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <asm/setup.h>

static char new_command_line[COMMAND_LINE_SIZE];

static int cmdline_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", new_command_line);
	return 0;
}

static int cmdline_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cmdline_proc_show, NULL);
}

static const struct file_operations cmdline_proc_fops = {
	.open		= cmdline_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int remove_flag(const char *flag)
{
	char *start_flag, *end_flag, *next_flag;
	char *last_char = new_command_line + COMMAND_LINE_SIZE;
	size_t rest_len, flag_len;
	int ret = 0;

	/* Ensure all instances of a flag are removed */
	while ((start_flag = strstr(new_command_line, flag))) {
		end_flag = strchr(start_flag, ' ');

		/* this may happend when copied cmdline is filled up fully */
		if (end_flag > last_char)
			end_flag = last_char;

		if (end_flag) {
			next_flag = end_flag + 1;
			rest_len = (size_t)(last_char - end_flag);
			flag_len = (size_t)(end_flag - start_flag);

			memmove(start_flag, next_flag, rest_len);
			memset(last_char - flag_len, '\0', flag_len);
		} else {
			memset(start_flag - 1, '\0', last_char - start_flag);
		}

		ret++;
	}

	return ret;
}

static void remove_safetynet_flags(void)
{
	remove_flag("androidboot.enable_dm_verity=");
	remove_flag("androidboot.secboot=");
	remove_flag("androidboot.verifiedbootstate=");
	remove_flag("androidboot.veritymode=");
}

static int __init proc_cmdline_init(void)
{
	memcpy(new_command_line, saved_command_line,
		min((size_t)COMMAND_LINE_SIZE, strlen(saved_command_line)));

	/*
	 * Remove various flags from command line seen by userspace in order to
	 * pass SafetyNet CTS check.
	 */
	remove_safetynet_flags();

	proc_create("cmdline", 0, NULL, &cmdline_proc_fops);
	return 0;
}
fs_initcall(proc_cmdline_init);
