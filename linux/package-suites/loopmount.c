/* Mount an ext4 image through a free loop device, for suites whose files
 * need more room or more file types than the card's FAT has: the image's
 * BusyBox mount has no loop support.
 *
 *   loopmount IMAGE DIRECTORY
 */
#include <fcntl.h>
#include <linux/loop.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	char device[32];
	int control, loop, image, n;

	if (argc != 3)
		return 2;
	control = open("/dev/loop-control", O_RDWR);
	n = control < 0 ? -1 : ioctl(control, LOOP_CTL_GET_FREE);
	if (n < 0) {
		perror("loopmount: loop-control");
		return 1;
	}
	snprintf(device, sizeof(device), "/dev/loop%d", n);
	loop = open(device, O_RDWR);
	image = open(argv[1], O_RDWR);
	if (loop < 0 || image < 0 || ioctl(loop, LOOP_SET_FD, image) < 0) {
		perror("loopmount: attach");
		return 1;
	}
	if (mount(device, argv[2], "ext4", 0, NULL) < 0) {
		perror("loopmount: mount");
		return 1;
	}
	return 0;
}
