// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <linux/mshv.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../kselftest.h"

int main(int argc, char **argv)
{
	const char *path = "/dev/mshv";
	struct mshv_create_partition args = {};
	int mshv_fd;
	int vm_fd;

	ksft_print_header();
	ksft_set_plan(2);

	if (argc > 1)
		path = argv[1];

	mshv_fd = open(path, O_RDWR | O_CLOEXEC);
	if (mshv_fd < 0) {
		if (errno == ENOENT || errno == ENODEV)
			ksft_exit_skip("%s is not available\n", path);

		ksft_exit_fail_msg("open(%s) failed: %s\n", path,
				   strerror(errno));
	}

	ksft_test_result_pass("open(%s)\n", path);

	vm_fd = ioctl(mshv_fd, MSHV_CREATE_PARTITION, &args);
	if (vm_fd < 0) {
		close(mshv_fd);
		ksft_exit_fail_msg("MSHV_CREATE_PARTITION failed: %s\n",
				   strerror(errno));
	}

	ksft_test_result_pass("MSHV_CREATE_PARTITION\n");

	close(vm_fd);
	close(mshv_fd);

	ksft_finished();
}
