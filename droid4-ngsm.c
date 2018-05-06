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
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <linux/gsmmux.h>
#include <linux/tty.h>

#define BUF_SZ		4096
#define CMD_BUF_SZ	256

enum modem_state {
	MODEM_STATE_NONE,
	MODEM_STATE_DISCONNECTED,
	MODEM_STATE_CONNECTED,
	MODEM_STATE_ENABLED,
	MODEM_STATE_CALLING,
	MODEM_STATE_DISABLED,
	MODEM_STATE_EXITING,
};

enum dlci_index {
	DLCI1,
	DLCI2,
	DLCI3,
	DLCI4,
	DLCI5,
	DLCI6,
	DLCI7,
	DLCI8,
	DLCI9,
	DLCI10,
	DLCI11,
	DLCI12,
	NR_DLCI,
};

enum modem_command {
	MODEM_COMMAND_NONE,
	MODEM_ENABLE_SPEAKER,
	MODEM_DISABLE_SPEAKER,
	MODEM_START_CALL,
};

struct dlci_cmd {
	const char *cmd;
	const char *res;
};

struct dlci {
	int id;
	int fd;
	struct timespec ts;
	const struct dlci_cmd *cmd;
	enum modem_state next_state;
	int nr_cmds;
	int cur_cmd;
	int cmd_id;
};

struct modem {
	struct dlci *dlcis;
	enum modem_state state;
	fd_set read_set;
	struct timespec last_dlci;
	struct timespec last_check;
	unsigned short msg_id;
	enum modem_command cmd;
	char cmd_buf[CMD_BUF_SZ];
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

static int dlci_wait(const char *name) {
	int timeout = 20, fd;

	while (timeout-- > 0) {
		fd = open(name, O_RDONLY | O_NOCTTY | O_NDELAY);
		if (fd >= 0) {
			close(fd);
			fprintf(stderr, "Found dlci\n");

			return 0;
		}
		fprintf(stderr, "Waiting for dlci..\n");
		sleep(1);
	}

	return -ETIMEDOUT;
}

static int dlci_open_all(struct modem *modem)
{
	struct dlci *dlci;
	int error, i, fd;
	char name[16];

	sprintf(name, "/dev/gsmtty%i", 1);
	error = dlci_wait(name);
	if (error) {
		fprintf(stderr,
			"Timed out, is phy-mapphone-mdm6600 loaded?\n");

		return error;
	}

	for (i = 0; i < NR_DLCI; i++) {
		dlci = &modem->dlcis[i];
		dlci->id = i + 1;

		sprintf(name, "/dev/gsmtty%i", dlci->id);
		fd = open(name, O_RDWR | O_NOCTTY | O_NDELAY);
		if (fd < 0)
			fprintf(stderr, "Could not open %s: %s\n",
				name, strerror(errno));
		dlci->fd = fd;
	}

	return 0;
}

static void dlci_close_all(struct modem *modem)
{
	struct dlci *dlci;
	int i, error;

	for (i = 0; i < NR_DLCI; i++) {
		dlci = &modem->dlcis[i];
		if (dlci->fd < 0)
			continue;

		error = close(dlci->fd);
		if (error)
			fprintf(stderr, "Could not close dlci%i: %s\n",
				i, strerror(errno));

		dlci->fd = -ENODEV;
	}
}

static int dlci_lock(struct modem *modem, struct dlci *dlci)
{
	if (dlci->ts.tv_sec)
		return -EBUSY;

	dlci->cmd_id = modem->msg_id++;

	return clock_gettime(CLOCK_REALTIME, &dlci->ts);
}

static void dlci_unlock(struct dlci *dlci)
{
	dlci->ts.tv_sec = 0;
	dlci->cmd_id = 0;
}

static int dlci_busy(struct dlci *dlci)
{
	if (dlci->ts.tv_sec)
		return 1;

	return 0;
}

/*
 * Format is: "UNNNNAT+FOO\r\0" where NNNN is incrementing message ID
 */
static int dlci_send_cmd(struct modem *modem, const int dlci_nr,
			 const char *cmd)
{
	struct dlci *dlci;
	int error;

	if (dlci_nr < 1 || dlci_nr > NR_DLCI)
		return -EINVAL;

	dlci = &modem->dlcis[dlci_nr - 1];

	error = dlci_lock(modem, dlci);
	if (error)
		return error;

	fprintf(stdout, "%i> U%04i%s\r\n", dlci->id, dlci->cmd_id, cmd);
	dprintf(dlci->fd, "U%04i%s\r", dlci->cmd_id, cmd);
	fsync(dlci->fd);

	return 0;
}

static int dlci_handle_response(struct modem *modem, struct dlci *dlci,
				char *buf, int buf_sz)
{
	ssize_t len;
	char cmd_id[5];
	int resp = 0, resp_len = 0;

	len = read(dlci->fd, buf, BUF_SZ);
	if (!len)
		return 0;

	printf("%i< %s", dlci->id, buf);

	/*
	 * FIXME: Parse incoming "~+WAKEUP" and notify for "~+CLIP"
	 * incoming call
	 */
	if (len > 6 && buf[5] == '~')
		return  0;

	if (!dlci->cmd)
		goto done;

	if (len > 6 && buf[0] == 'U') {
		snprintf(cmd_id, 5, "%s", buf + 1);
		resp = atoi(cmd_id);
	}

	if (resp != dlci->cmd_id)
		return 0;

	if (dlci->cmd[dlci->cur_cmd].res)
		resp_len = strlen(dlci->cmd[dlci->cur_cmd].res);

	if (strncmp(dlci->cmd[dlci->cur_cmd].res,
		     buf + 5, resp_len)) {
		fprintf(stderr, "No match for U%04i command %s\n",
		       dlci->cmd_id, dlci->cmd[dlci->cur_cmd].res);
		return 0;
	}

	/* No further commends to trigger? */
	dlci->cur_cmd++;
	if (dlci->cur_cmd >= dlci->nr_cmds) {
		if (dlci->next_state) {
			modem->state = dlci->next_state;
			dlci->next_state = MODEM_STATE_NONE;
		}
		dlci->cmd = NULL;
		dlci->nr_cmds = 0;
		dlci->cur_cmd = 0;

		goto done;
	}

	dlci_unlock(dlci);

	return dlci_send_cmd(modem, dlci->id, dlci->cmd[dlci->cur_cmd].cmd);

done:
	dlci_unlock(dlci);

	return 0;
}

#define NSEC_PER_SEC		1000000000LL

static int dlci_handle_timeout(struct modem *modem, struct dlci *dlci,
			       struct timespec *now, char *buf, int len)
{
	long long nsec;

	if (!dlci->ts.tv_sec)
		return 0;

	nsec = NSEC_PER_SEC * now->tv_sec + now->tv_nsec;
	nsec -= NSEC_PER_SEC * dlci->ts.tv_sec + dlci->ts.tv_nsec;

	if (nsec / NSEC_PER_SEC < 5)
		return 0;

	fprintf(stderr, "Timed out on dlci%i for command U%04i\n",
		dlci->id, dlci->cmd_id);
	dlci->cmd = NULL;
	dlci_unlock(dlci);

	return 0;
}

static const struct dlci_cmd dlci1_modem_found[] = {
	{ .cmd = "AT+CFUN?", .res = "+CFUN=", },
};

static int modem_test_connected(struct modem *modem)
{
	struct dlci *dlci;
	const struct dlci_cmd *cmds;

	dlci = &modem->dlcis[DLCI1];
	if (dlci_busy(dlci))
		return -EAGAIN;

	fprintf(stdout, "Testing if modem is available..\n");
	cmds = dlci1_modem_found;

	dlci->cmd = cmds;
	dlci->nr_cmds = 1;
	dlci->cur_cmd = 0;

	dlci->next_state = MODEM_STATE_CONNECTED;

	return dlci_send_cmd(modem, dlci->id, cmds[0].cmd);
}

/*
 * AT+EACC=3,0 enables microphone
 * AT+CMUT=0 unmutes microphone
 * AT+NREC=1 enables noise reduction and echo cancellation
 * AT+CLVL=6 sets volume level 0 to 7
 */
static const struct dlci_cmd dlci2_enable_speaker[] = {
	{ .cmd = "AT+EACC=3,0", .res = "+EACC:OK", },
	{ .cmd = "AT+CMUT=0", .res = "+CMUT:OK", },
	{ .cmd = "AT+NREC=1", .res = "+NREC:OK", },
	{ .cmd = "AT+CLVL=6", .res = "+CLVL:OK", },
};

static int modem_enable_speaker_phone(struct modem *modem)
{
	struct dlci *dlci;
	const struct dlci_cmd *cmds;

	fprintf(stdout, "Enabling speaker phone..\n");
	dlci = &modem->dlcis[DLCI2];
	cmds = dlci2_enable_speaker;

	dlci->cmd = cmds;
	dlci->nr_cmds = 4;
	dlci->cur_cmd = 0;

	return dlci_send_cmd(modem, dlci->id, cmds[0].cmd);
}

static const struct dlci_cmd dlci2_disable_speaker[] = {
	{ .cmd = "AT+EACC=0,0", .res = "+EACC:", },
	{ .cmd = "AT+CMUT=1", .res = "+CMUT:", },
	{ .cmd = "AT+NREC=0", .res = "+NREC:", },
	{ .cmd = "AT+CLVL=0", .res = "+CLVL:", },
};

static int modem_disable_speaker_phone(struct modem *modem)
{
	struct dlci *dlci;
	const struct dlci_cmd *cmds;

	fprintf(stdout, "Disabling speaker phone..\n");
	dlci = &modem->dlcis[DLCI2];
	cmds = dlci2_disable_speaker;

	dlci->cmd = cmds;
	dlci->nr_cmds = 4;
	dlci->cur_cmd = 0;

	return dlci_send_cmd(modem, dlci->id, cmds[0].cmd);
}

/*
 * AT+CFUN=1 enables radio
 * AT+CLCC lists current calls
 */
static const struct dlci_cmd dlci1_modem_enable[] = {
	{ .cmd = "AT+CFUN=1", .res = "+CFUN:OK", },
	{ .cmd = "AT+CLCC", .res = "+CLCC:", },
};

static int modem_radio_enable(struct modem *modem)
{
	struct dlci *dlci;
	const struct dlci_cmd *cmds;
	int error;

	dlci = &modem->dlcis[DLCI1];
	if (dlci_busy(dlci))
		return -EAGAIN;

	error = modem_enable_speaker_phone(modem);
	if (error)
		return error;

	cmds = dlci1_modem_enable;
	dlci->cmd = cmds;
	dlci->nr_cmds = 2;
	dlci->cur_cmd = 0;

	dlci->next_state = MODEM_STATE_ENABLED;

	return dlci_send_cmd(modem, dlci->id, cmds[0].cmd);
}

static int modem_start_phone_call(struct modem *modem)
{
	struct dlci *dlci;
	int error;

	dlci = &modem->dlcis[DLCI1];
	if (dlci_busy(dlci))
		return -EAGAIN;

	fprintf(stdout, "Starting phone call..\n");
	error = dlci_send_cmd(modem, dlci->id, modem->cmd_buf);
	if (error)
		return error;

	modem->cmd = MODEM_COMMAND_NONE;
	modem->state = MODEM_STATE_CALLING;

	return 0;
}

static const struct dlci_cmd dlci1_modem_list_calls[] = {
	{ .cmd = "AT+CLCC", .res = "+CLCC:", },
};

static int modem_list_calls(struct modem *modem)
{
	struct dlci *dlci;
	const struct dlci_cmd *cmds;

	dlci = &modem->dlcis[DLCI1];
	if (dlci_busy(dlci))
		return -EAGAIN;

	cmds = dlci1_modem_list_calls;
	dlci->cmd = cmds;
	dlci->nr_cmds = 1;
	dlci->cur_cmd = 0;

	return dlci_send_cmd(modem, dlci->id, cmds[0].cmd);
}

/*
 * ATH hangs up
 * AT+CLCC lists current calls
 * AT+CFUN=0 disables radio
 */
static const struct dlci_cmd dlci1_hang_up[] = {
	{ .cmd = "ATH", .res = "H:", },
	{ .cmd = "AT+CLCC", .res = "+CLCC:", },
	{ .cmd = "AT+CFUN=0", .res = "+CFUN:OK", },
};

static int modem_stop_phone_call(struct modem *modem)
{
	struct dlci *dlci;
	const struct dlci_cmd *cmds;
	int error;

	dlci = &modem->dlcis[DLCI1];
	if (dlci_busy(dlci))
		return -EAGAIN;

	cmds = dlci1_hang_up;
	dlci->cmd = cmds;
	dlci->nr_cmds = 3;
	dlci->cur_cmd = 0;

	dlci->next_state = MODEM_STATE_EXITING;

	error = dlci_send_cmd(modem, dlci->id, cmds[0].cmd);
	if (error) {
		fprintf(stderr, "Could not hang up: %s\n",
			strerror(-error));

		return error;
	}

        return modem_disable_speaker_phone(modem);
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

static int handle_stdin(struct modem *modem, char *buf, int buf_sz)
{
	struct dlci *dlci;
	ssize_t len;
	int dlci_nr, error;
	char *cmd;

	len = read(STDIN_FILENO, buf, buf_sz);
	if (len < 3)
		return 0;

	cmd = strchr(buf, ' ');
	if (!cmd)
		return 0;

	cmd[0] = '\0';
	cmd++;
	len = strlen(cmd);
	cmd[len - 1] = '\0';
	dlci_nr = atoi(buf);

	if (dlci_nr < 1 || dlci_nr > NR_DLCI)
		return 0;

	dlci = &modem->dlcis[dlci_nr - 1];

	error = dlci_send_cmd(modem, dlci->id, cmd);
	if (error)
		fprintf(stderr, "Error sending command: %i\n", error);

	return 0;
}

static int handle_dlci(struct modem *modem, char *buf, int len)
{
	struct dlci *dlci;
	int error, i;

	error = clock_gettime(CLOCK_REALTIME, &modem->last_dlci);
	if (error)
		return error;

	for (i = 0; i < NR_DLCI; i++) {
		dlci = &modem->dlcis[i];

		/* Data at DLCI? */
		if (FD_ISSET(dlci->fd, &modem->read_set)) {
			error = dlci_handle_response(modem, dlci, buf, len);
			if (error)
				fprintf(stderr, "Error handling response: %i\n",
					error);
		}

		/* Command for DLCI timed out? */
		error = dlci_handle_timeout(modem, dlci,
					    &modem->last_dlci, buf, len);
		if (error)
			fprintf(stderr, "Error handling timeout: %i\n",
				error);
	}

	return 0;
}

static int set_modem_state(struct modem *modem)
{
	int error = 0;

	switch (modem->state) {
	case MODEM_STATE_NONE:
	case MODEM_STATE_DISCONNECTED:
		error = modem_test_connected(modem);
		if (error && error != -EAGAIN)
			modem->state = MODEM_STATE_EXITING;
		break;
	case MODEM_STATE_CONNECTED:
		if (modem->cmd == MODEM_START_CALL) {
			error = modem_radio_enable(modem);
			if (error && error != -EAGAIN)
				modem->state = MODEM_STATE_EXITING;
		}
		break;
	case MODEM_STATE_ENABLED:
		if (modem->cmd == MODEM_START_CALL) {
			error = modem_start_phone_call(modem);
			if (error && error != -EAGAIN)
				modem->state = MODEM_STATE_DISABLED;
		}
		break;
	case MODEM_STATE_CALLING:
		if (&modem->last_dlci.tv_sec - &modem->last_check.tv_sec > 5)
			modem->last_check = modem->last_dlci;
		else
			break;
		error = modem_list_calls(modem);
		if (error && error != -EAGAIN)
			modem->state = MODEM_STATE_DISABLED;
		break;
	case MODEM_STATE_DISABLED:
		error = modem_stop_phone_call(modem);
		if (error && error != -EAGAIN)
			modem->state = MODEM_STATE_EXITING;
		break;
	case MODEM_STATE_EXITING:
		fprintf(stdout, "Exiting..\n");
		error = -EINTR;
	}

	return error;
}

static int handle_io(struct modem *modem)
{
	struct dlci *dlci;
	struct timespec ts;
	struct sigaction handler;
	sigset_t sigmask, emptymask;
	char *cmd_buf, *dlci_buf;
	int error, i, timeout;

	cmd_buf = malloc(BUF_SZ);
	if (!cmd_buf)
		return -ENOMEM;

	dlci_buf = malloc(BUF_SZ);
	if (!dlci_buf) {
		free(cmd_buf);

		return -ENOMEM;
	}

	handler.sa_handler = signal_handler;
	handler.sa_flags = 0;
	error = sigfillset(&handler.sa_mask);
	if (error < 0) {
		fprintf(stderr, "Could not sigfillset: %s\n",
			strerror(errno));
		goto free;
	}


	error = sigaction(SIGINT, &handler, 0);
	if (error < 0) {
		fprintf(stderr, "Could not set sigaction: %s\n",
			strerror(errno));
		goto free;
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);

	error = sigprocmask(SIG_BLOCK, &sigmask, NULL);
	if (error < 0) {
		fprintf(stderr, "Could not set sigprocmask: %s\n",
			strerror(errno));
		goto free;
	}

	sigemptyset(&emptymask);

	while (1) {
		FD_ZERO(&modem->read_set);
		FD_SET(STDIN_FILENO, &modem->read_set);

		for (i = 0; i < NR_DLCI; i++) {
			dlci = &modem->dlcis[i];
			if (dlci->fd > 0 && dlci->id != 8)
				FD_SET(dlci->fd, &modem->read_set);
		}

		switch (modem->state) {
		case MODEM_STATE_CONNECTED:
			timeout = 10;
		case MODEM_STATE_CALLING:
			timeout = 3;
		default:
			timeout = 1;
			break;
		}

		ts.tv_sec = timeout;
		ts.tv_nsec = 0;
		memset(cmd_buf, '\0', BUF_SZ);
		memset(dlci_buf, '\0', BUF_SZ);

		error = pselect(modem->dlcis[NR_DLCI - 1].fd + 1,
				&modem->read_set, NULL, NULL, &ts,
				&emptymask);
                if (signal_received || (error < 0)) {
			signal_received = 0;
			if (modem->state == MODEM_STATE_CALLING)
				modem->state = MODEM_STATE_DISABLED;
			else
				modem->state = MODEM_STATE_EXITING;
		}

		error = handle_dlci(modem, dlci_buf, BUF_SZ);
		if (error)
			break;

		/* Data at stdin? */
		if (FD_ISSET(STDIN_FILENO, &modem->read_set)) {
			error = handle_stdin(modem, cmd_buf, BUF_SZ);
			if (error)
				break;
		}

		error = set_modem_state(modem);
		if (error && error != -EAGAIN)
			break;
	}
free:
	free(dlci_buf);
	free(cmd_buf);

	return 0;
}

static int parse_params(struct modem *modem, int argc, char **argv)
{
	char *buf;

	if (argc > 1 && !strncmp("--call=", argv[1], 7)) {
		buf = argv[1];
		if (strlen(buf) > CMD_BUF_SZ - 3)
			return -EINVAL;

		if (strlen(buf) > 7 &&
		    !strncmp("--call=", buf, 7))
			sprintf(modem->cmd_buf, "ATD%s", buf + 7);
		else
			sprintf(modem->cmd_buf, "ATD%s", buf);

		modem->cmd = MODEM_START_CALL;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct modem *modem;
	struct termios t;
	const char *port = "/dev/ttyS0";
	int fd, error;
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

	modem = calloc(1, sizeof(*modem));
	if (!modem)
		goto close;

	modem->dlcis = calloc(NR_DLCI, sizeof(struct dlci));
	if (!modem->dlcis)
		goto free_modem;

	buf = malloc(BUF_SZ);
	if (!buf)
		goto free_dlci;

	fprintf(stdout, "Starting ngsm..\n");
	error = start_ngsm(fd);
	if (error < 0) {
		fprintf(stderr, "Could not start ngsm: %s\n",
			strerror(-error));
		goto free;
	}

	error = dlci_open_all(modem);
	if (error)
		goto close;

	error = parse_params(modem, argc, argv);
	if (error)
		goto close;

	fprintf(stdout, "Started ngsm, press Ctrl-C to exit when done\n");
	error = handle_io(modem);
	if (error)
		fprintf(stderr, "Got IO error: %i\n", error);

	dlci_close_all(modem);

	error = stop_ngsm(fd);
	if (error < 0)
		fprintf(stderr, "Could not stop ngsm: %s\n",
			strerror(-error));

free:
	free(buf);
free_dlci:
	free(modem->dlcis);
free_modem:
	free(modem);
close:
	close(fd);

	return error;
}
