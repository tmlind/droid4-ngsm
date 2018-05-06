/*
 * droid4-ngsm utility to talk to ts27010 mux uart
 *
 * Copyright (C) 2018 Tony Lindgren <tony@atomide.com>
 * License: GPL v2
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/gsmmux.h>
#include <linux/tty.h>

#define BUF_SZ		4096

enum modem_state {
	MODEM_STATE_DISCONNECTED,
	MODEM_STATE_CONNECTED,
	MODEM_STATE_CALLING,
};

static unsigned short msg_id;
static int signal_received;

static void signal_handler(int sigal) {
	signal_received = 1;
}

/*
 * Needs n_gsm kernel module that will create the /dev/gsmtty*
 * device nodes. To debug, modprobe n_gsm debug=0xff.
 *
 * Note that eventually we should have n_gsm enabled via serdev,
 * and then this code won't be needed.
 *
 * Please assume in various apps that /dev/gsmtty* nodes have been
 * already created and the line discipline configured by this
 * function or kernel serdev driver.
 *
 * Also note that it takes a several seconds for the /dev/gsmtty*
 * ports to start responding after loading phy-mapphone-mdm6600
 * kernel module.
 */
static int start_ngsm(int fd)
{
	int error;
	struct gsm_config c;
	int ldisc = N_GSM0710;

	error = ioctl(fd, TIOCSETD, &ldisc);
	if (error) {
		fprintf(stderr, "Could not set line discipline: %s\n",
			strerror(errno));
		return -1;
	}

	error = ioctl(fd, GSMIOC_GETCONF, &c);
	if (error) {
		fprintf(stderr, "Could not get conf: %s\n",
			strerror(errno));
		return -1;
	}

	c.i = 1;		/* 1 = UIH, 2 = UI */
	c.initiator = 1;
	c.encapsulation = 0;	/* basic mode */
	c.adaption = 1;
	c.mru = 1024;		/* from android ts27010 driver */
	c.mtu = 1024;		/* from android ts27010 driver */
	c.t1 = 10;		/* ack timer, default 10ms */
	c.t2 = 34;		/* response timer, default 34 */
	c.n2 = 20;		/* retransmissions, default 3 */

	fprintf(stderr, "Setting initial n2 retransmissions to %i..\n",
		c.n2);
	error = ioctl(fd, GSMIOC_SETCONF, &c);
	if (error) {
		fprintf(stderr, "Could not set conf: %s\n",
			strerror(errno));
		return -1;
	}

	/*
	 * Wait a bit for n_gsm to detect the ADM mode based on
	 * control channel timeouts.
	 */
	sleep(3);

	c.n2 = 3;		/* change back to default value */

	fprintf(stderr, "Setting n2 retransmissions back to default %i..\n",
		c.n2);
	error = ioctl(fd, GSMIOC_SETCONF, &c);
	if (error) {
		fprintf(stderr, "Could not set conf: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int stop_ngsm(int fd)
{
	int error;
	int ldisc = N_TTY;

	error = ioctl(fd, TIOCSETD, &ldisc);
	if (error) {
		fprintf(stderr, "Could not set line discipline: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Format is: "UNNNNAT+FOO\r\0" where NNNN is incrementing message ID
 *
 * FIXME: Use select and keep the ports open that W3GLTE needs for
 *	  proxying
 *
 * FIXME: Actually check that we get some response back
 */
static int test_ngsm(int dlci_nr, char *msg, char *buf, size_t buf_sz)
{
	int fd, len;
	char dlci[16];

	if (dlci_nr > 256)
		dlci_nr = 256;

	sprintf(dlci, "/dev/gsmtty%i", dlci_nr);

	fd = open(dlci, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
			dlci, strerror(errno));

		return fd;
	}

	sprintf(buf, "U%04i%s\r", msg_id++, msg);
	fprintf(stdout, "%i> %s\n", dlci_nr, buf);
	write(fd, buf, strlen(buf));
	memset(buf, '\0', buf_sz);
	sleep(1);
	len = read(fd, buf, buf_sz);
	if (!len)
		goto close;

	fprintf(stdout, "%i< %s\n", dlci_nr, buf);

close:
	close(fd);

	return 0;
}

static int enable_speaker_phone(char *buf, size_t buf_sz)
{
	int error;

	/* Enable microphone */
	error = test_ngsm(2, "AT+EACC=3,0", buf, BUF_SZ);
	if (error)
		return error;

	/* Unmute microphone */
	error = test_ngsm(2, "AT+CMUT=0", buf, BUF_SZ);
	if (error)
		return error;

	/* Enable noise reduction and echo cancelling */
	error = test_ngsm(2, "AT+NREC=1", buf, BUF_SZ);
	if (error)
		return error;

	/* Set speaker volume level 0 to 7 */
	error = test_ngsm(2, "AT+CLVL=6", buf, BUF_SZ);
	if (error)
		return error;

	return 0;
}

static int disable_speaker_phone(char *buf, size_t buf_sz)
{
	int error;

	/* Disable microphone */
	error = test_ngsm(2, "AT+EACC=0,0", buf, BUF_SZ);
	if (error)
		return error;

	/* Mute microphone */
	error = test_ngsm(2, "AT+CMUT=1", buf, BUF_SZ);
	if (error)
		return error;

	/* Disable noise reduction and echo cancelling */
	error = test_ngsm(2, "AT+NREC=0", buf, BUF_SZ);
	if (error)
		return error;

	/* Set speaker volume level 0 to 7 */
	error = test_ngsm(2, "AT+CLVL=0", buf, BUF_SZ);
	if (error)
		return error;

	return 0;
}

static int start_phone_call(char *buf, size_t buf_sz,
			    char *phone_number)
{
	char cmd[256];
	int error;

	if (strlen(phone_number) > 256 - 3)
		return -EINVAL;

	if (strlen(phone_number) > 7 &&
	    !strncmp("--call=", phone_number, 7))
		sprintf(cmd, "ATD%s", phone_number + 7);
	else
		sprintf(cmd, "ATD%s", phone_number);

	/* Enable radio */
	error = test_ngsm(1, "AT+CFUN=1", buf, BUF_SZ);
	if (error)
		return error;

	error = test_ngsm(1, cmd, buf, BUF_SZ);
	if (error)
		return error;

	return 0;
}

static int stop_phone_call(char *buf, size_t buf_sz)
{
	int error;

	error = test_ngsm(1, "ATH", buf, BUF_SZ);
	if (error)
		fprintf(stderr, "Could not hang up call: %s\n",
			strerror(-error));

	/* List current calls */
	error = test_ngsm(1, "AT+CLCC", buf, BUF_SZ);
	if (error)
		fprintf(stderr, "Could not list current calls: %s\n",
			strerror(-error));

	/* Disable radio */
	error = test_ngsm(1, "AT+CFUN=0", buf, BUF_SZ);
	if (error)
		return error;

	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction handler;
	struct termios t;
	const char *port = "/dev/ttyS0";
	enum modem_state state;
	int fd, error, i;
	char *buf;

	if (argc > 1 && !strncmp("--help", argv[1], 6)) {
		fprintf(stdout,
			"usage: %s [--call=number]\n", argv[0]);

		return 0;
	}

	fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
			port, strerror(errno));

		return fd;
	}

	tcgetattr(fd, &t);
	cfsetispeed(&t, B115200);
	cfsetospeed(&t, B115200);
	t.c_iflag &= ~(IXON | IXOFF);
	t.c_lflag |= CRTSCTS;

	error = tcsetattr(fd, TCSANOW, &t);
	if (error < 0) {
		fprintf(stderr, "Failed to tcsetattr: %s\n",
			strerror(errno));
		goto close;
	}

	buf = malloc(BUF_SZ);
	if (!buf)
		goto close;

	handler.sa_handler = signal_handler;
	error = sigfillset(&handler.sa_mask);
	if (error < 0) {
		fprintf(stderr, "Could not sigfillset: %s\n",
			strerror(errno));
		goto close;
	}

	handler.sa_flags = 0;

	error = sigaction(SIGINT, &handler, 0);
	if (error < 0) {
		fprintf(stderr, "Could not set sigaction: %s\n",
			strerror(errno));
		goto close;
	}

	for (i = 0; i < 10; i++) {
		fprintf(stdout, "Starting ngsm..\n");
		error = start_ngsm(fd);
		if (error < 0) {
			fprintf(stderr, "Could not start ngsm: %s\n",
				strerror(-error));
			goto free;
		}

		fprintf(stdout, "Testing ngsm.. (few failures are normal)\n");
		error = test_ngsm(1, "AT+CFUN?", buf, BUF_SZ);
		if (!error) {
			fprintf(stdout, "Enable speaker phone..\n");
			error = enable_speaker_phone(buf, BUF_SZ);
			if (error)
				goto disable;

			if (argc < 2)
				break;

			if (!strncmp("--call=", argv[1], 7)) {
				fprintf(stdout, "Starting phone call..\n");
				error = start_phone_call(buf, BUF_SZ, argv[1]);
				if (error)
					goto disable;
				state = MODEM_STATE_CALLING;

				break;
			}

			break;
		}

		fprintf(stderr, "Trying to start ngsm again: %s\n",
			strerror(-error));

		error = stop_ngsm(fd);
		if (error < 0) {
			fprintf(stderr, "Could not stop ngsm: %s\n",
				strerror(-error));
			goto free;
		}
		sleep(1);
	}

	if (error < 0) {
		fprintf(stderr, "Timed out starting ngsm\n");
		goto free;
	}

	fprintf(stdout, "Started ngsm, press Ctrl-C to exit when done\n");
	while (1) {
		if (state == MODEM_STATE_CALLING) {
			error = test_ngsm(1, "AT+CLCC", buf, BUF_SZ);
			if (error)
				break;
		}
		if (signal_received)
			break;
		sleep(2);
	}

	if (state == MODEM_STATE_CALLING) {
		fprintf(stdout, "Hanging up..\n");
		error = stop_phone_call(buf, BUF_SZ);
	}

disable:
	fprintf(stdout, "Disable speaker phone..\n");
	error = disable_speaker_phone(buf, BUF_SZ);
	if (error)
		fprintf(stderr, "Could not disable speaker phone: %s\n",
			strerror(-error));

free:
	free(buf);

close:
	close(fd);

	return error;
}
