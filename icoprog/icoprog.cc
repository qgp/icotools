#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <wiringPi.h>
#include <vector>

#define RPI_ICE_CLK      7 // PIN  7, GPIO.7
#define RPI_ICE_CDONE    2 // PIN 13, GPIO.2
#define RPI_ICE_MOSI    21 // PIN 29, GPIO.21
#define RPI_ICE_MISO    22 // PIN 31, GPIO.22
#define LOAD_FROM_FLASH 23 // PIN 33, GPIO.23
#define RPI_ICE_CRESET  25 // PIN 37, GPIO.25
#define RPI_ICE_CS      10 // PIN 24, CE0
#define RPI_ICE_SELECT  26 // PIN 32, GPIO.26

#define RASPI_D8   0 // PIN 11, GPIO.0
#define RASPI_D7   1 // PIN 12, GPIO.1
#define RASPI_D6   3 // PIN 15, GPIO.3
#define RASPI_D5   4 // PIN 16, GPIO.4
#define RASPI_D4  12 // PIN 19, MOSI
#define RASPI_D3  13 // PIN 21, MISO
#define RASPI_D2  11 // PIN 26, CE1
#define RASPI_D1  24 // PIN 35, GPIO.24
#define RASPI_D0  27 // PIN 36, GPIO.27
#define RASPI_DIR 28 // PIN 38, GPIO.28
#define RASPI_CLK 29 // PIN 40, GPIO.29

bool verbose = false;
bool send_zero = false;
bool recv_zero = false;
char current_send_recv_mode = 0;
int current_recv_ep = -1;

int last_recv_v = -1;
int last_recv_rep = 0;

void fpga_reset()
{
	pinMode(RPI_ICE_CRESET,  OUTPUT);
	digitalWrite(RPI_ICE_CRESET, LOW);
	usleep(2000);
	digitalWrite(RPI_ICE_CRESET, HIGH);
	usleep(500000);
	if (digitalRead(RPI_ICE_CDONE) != HIGH)
		fprintf(stderr, "Warning: cdone is low\n");
}

int get_time_ms()
{
	static struct timespec spec_start;
	static bool spec_start_initialized = false;

	struct timespec spec_now;
	clock_gettime(CLOCK_REALTIME, &spec_now);
	if (!spec_start_initialized) {
		spec_start = spec_now;
		spec_start_initialized = true;
	}

	int s = spec_now.tv_sec - spec_start.tv_sec;
	int ns = spec_now.tv_nsec - spec_start.tv_nsec;

	return s*1000 + ns/1000000;
}

void prog_bitstream(bool reset_only = false)
{
	pinMode(RPI_ICE_CLK,     OUTPUT);
	pinMode(RPI_ICE_MOSI,    OUTPUT);
	pinMode(LOAD_FROM_FLASH, OUTPUT);
	pinMode(RPI_ICE_CRESET,  OUTPUT);
	pinMode(RPI_ICE_CS,      OUTPUT);
	pinMode(RPI_ICE_SELECT,  OUTPUT);

	fprintf(stderr, "reset..\n");

	// enable reset
	digitalWrite(RPI_ICE_CRESET, LOW);

	// start clock high
	digitalWrite(RPI_ICE_CLK, HIGH);

	// select SRAM programming mode
	digitalWrite(LOAD_FROM_FLASH, LOW);
	digitalWrite(RPI_ICE_SELECT, LOW);
	digitalWrite(RPI_ICE_CS, LOW);
	usleep(100);

	// release reset
	digitalWrite(RPI_ICE_CRESET, HIGH);
	usleep(2000);

	fprintf(stderr, "cdone: %s\n", digitalRead(RPI_ICE_CDONE) == HIGH ? "high" : "low");

	if (reset_only)
		return;

	fprintf(stderr, "programming..\n");

	while (1)
	{
		int byte = getchar();
		if (byte < 0)
			break;
		for (int i = 7; i >= 0; i--) {
			digitalWrite(RPI_ICE_MOSI, ((byte >> i) & 1) ? HIGH : LOW);
			digitalWrite(RPI_ICE_CLK, LOW);
			digitalWrite(RPI_ICE_CLK, HIGH);
		}
	}

	for (int i = 0; i < 49; i++) {
		digitalWrite(RPI_ICE_CLK, LOW);
		digitalWrite(RPI_ICE_CLK, HIGH);
	}

	usleep(2000);
#if 0
	for (int i = 2; i <= 512; i+=2) {
		usleep(2000);
		if (((i-1) & i) == 0)
			fprintf(stderr, "cdone (after %3d ms): %s\n", i, digitalRead(RPI_ICE_CDONE) == HIGH ? "high" : "low");
	}
#else
	fprintf(stderr, "cdone: %s\n", digitalRead(RPI_ICE_CDONE) == HIGH ? "high" : "low");
#endif
}

void spi_begin()
{
	digitalWrite(RPI_ICE_CS, LOW);
	// fprintf(stderr, "SPI_BEGIN\n");
}

void spi_end()
{
	digitalWrite(RPI_ICE_CS, HIGH);
	// fprintf(stderr, "SPI_END\n");
}

uint32_t spi_xfer(uint32_t data, int nbits = 8)
{
	uint32_t rdata = 0;

	for (int i = nbits-1; i >= 0; i--)
	{
		digitalWrite(RPI_ICE_MOSI, (data & (1 << i)) ? HIGH : LOW);

		if (digitalRead(RPI_ICE_MISO) == HIGH)
			rdata |= 1 << i;

		digitalWrite(RPI_ICE_CLK, HIGH);

		digitalWrite(RPI_ICE_CLK, LOW);
	}

	// fprintf(stderr, "SPI:%d %02x %02x\n", nbits, data, rdata);

	return rdata;
}

void flash_write_enable()
{
	spi_begin();
	spi_xfer(0x06);
	spi_end();
}

void flash_erase_64kB(int addr)
{
	spi_begin();
	spi_xfer(0xd8);
	spi_xfer(addr >> 16);
	spi_xfer(addr >> 8);
	spi_xfer(addr);
	spi_end();
}

void flash_write(int addr, uint8_t *data, int n)
{
	spi_begin();
	spi_xfer(0x02);
	spi_xfer(addr >> 16);
	spi_xfer(addr >> 8);
	spi_xfer(addr);
	while (n--)
		spi_xfer(*(data++));
	spi_end();
}

void flash_read(int addr, uint8_t *data, int n)
{
	spi_begin();
	spi_xfer(0x03);
	spi_xfer(addr >> 16);
	spi_xfer(addr >> 8);
	spi_xfer(addr);
	while (n--)
		*(data++) = spi_xfer(0);
	spi_end();
}

int flash_wait()
{
	int ms_start = get_time_ms();

	while (1)
	{
		spi_begin();
		spi_xfer(0x05);
		int status = spi_xfer(0);
		spi_end();

		if ((status & 0x01) == 0)
			break;

		usleep(1000);
	}

	return get_time_ms() - ms_start;
}

void prog_flashmem(int pageoffset)
{
	pinMode(RPI_ICE_CLK,     OUTPUT);
	pinMode(RPI_ICE_MOSI,    OUTPUT);
	pinMode(LOAD_FROM_FLASH, OUTPUT);
	pinMode(RPI_ICE_CS,      OUTPUT);
	pinMode(RPI_ICE_SELECT,  OUTPUT);

	// connect flash to Raspi
	digitalWrite(LOAD_FROM_FLASH, LOW);
	digitalWrite(RPI_ICE_SELECT, HIGH);
	digitalWrite(RPI_ICE_CS, HIGH);
	digitalWrite(RPI_ICE_CLK, LOW);
	usleep(100);

	// power_up
	spi_begin();
	spi_xfer(0xab);
	spi_end();

	// read flash id
	spi_begin();
	spi_xfer(0x9f);
	fprintf(stderr, "flash id:");
	for (int i = 0; i < 20; i++)
		fprintf(stderr, " %02x", spi_xfer(0x00));
	fprintf(stderr, "\n");
	spi_end();

	// load prog data into buffer
	std::vector<uint8_t> prog_data;
	while (1) {
		int byte = getchar();
		if (byte < 0)
			break;
		prog_data.push_back(byte);
	}

	int ms_timer = 0;
	fprintf(stderr, "writing %.2fkB..", double(prog_data.size()) / 1024);

	for (int addr = 0; addr < int(prog_data.size()); addr += 256)
	{
		if (addr % (64*1024) == 0)
		{
			fprintf(stderr, "\n%3d%% @%06x ", 100*addr/int(prog_data.size()), addr);
			fprintf(stderr, "erasing 64kB sector..");

			flash_write_enable();
			flash_erase_64kB(addr + pageoffset * 0x10000);
			ms_timer += flash_wait();
		}

		if (addr % (32*256) == 0) {
			fprintf(stderr, "\n%3d%% @%06x writing: ", 100*addr/int(prog_data.size()), addr);
		}

		int n = std::min(256, int(prog_data.size()) - addr);
		uint8_t buffer[256];

		for (int retry_count = 0; retry_count < 100; retry_count++)
		{
			flash_write_enable();
			flash_write(addr + pageoffset * 0x10000, &prog_data[addr], n);
			ms_timer += flash_wait();

			flash_read(addr + pageoffset * 0x10000, buffer, n);

			if (!memcmp(buffer, &prog_data[addr], n)) {
				fprintf(stderr, "o");
				goto written_ok;
			}

			fprintf(stderr, "X");
		}

		// restart erasing and writing this 64kB sector
		addr -= addr % (64*1024);
		addr -= 256;
	
	written_ok:;
	}

	fprintf(stderr, "\n100%% total wait time: %d ms\n", ms_timer);

	// power_down
	spi_begin();
	spi_xfer(0xb9);
	spi_end();
}

void read_flashmem(int n)
{
	pinMode(RPI_ICE_CLK,     OUTPUT);
	pinMode(RPI_ICE_MOSI,    OUTPUT);
	pinMode(LOAD_FROM_FLASH, OUTPUT);
	pinMode(RPI_ICE_CS,      OUTPUT);
	pinMode(RPI_ICE_SELECT,  OUTPUT);

	// connect flash to Raspi
	digitalWrite(LOAD_FROM_FLASH, LOW);
	digitalWrite(RPI_ICE_SELECT, HIGH);
	digitalWrite(RPI_ICE_CS, HIGH);
	digitalWrite(RPI_ICE_CLK, LOW);
	usleep(100);

	// power_up
	spi_begin();
	spi_xfer(0xab);
	spi_end();

	// read flash id
	spi_begin();
	spi_xfer(0x9f);
	fprintf(stderr, "flash id:");
	for (int i = 0; i < 20; i++)
		fprintf(stderr, " %02x", spi_xfer(0x00));
	fprintf(stderr, "\n");
	spi_end();

	fprintf(stderr, "reading %.2fkB..\n", double(n) / 1024);
	for (int addr = 0; addr < n; addr += 256) {
		uint8_t buffer[256];
		flash_read(addr, buffer, std::min(256, n - addr));
		fwrite(buffer, std::min(256, n - addr), 1, stdout);
	}

	// power_down
	spi_begin();
	spi_xfer(0xb9);
	spi_end();
}

void epsilon_sleep()
{
	for (int i = 0; i < 1000; i++)
		asm volatile ("");
}

void send_word(int v)
{
	if (current_send_recv_mode != 's')
	{
		digitalWrite(RASPI_DIR, HIGH);
		epsilon_sleep();

		pinMode(RASPI_D8, OUTPUT);
		pinMode(RASPI_D7, OUTPUT);
		pinMode(RASPI_D6, OUTPUT);
		pinMode(RASPI_D5, OUTPUT);
		pinMode(RASPI_D4, OUTPUT);
		pinMode(RASPI_D3, OUTPUT);
		pinMode(RASPI_D2, OUTPUT);
		pinMode(RASPI_D1, OUTPUT);
		pinMode(RASPI_D0, OUTPUT);

		current_send_recv_mode = 's';
	}

	if (verbose) {
		last_recv_v = -1;
		last_recv_rep = 0;
		fprintf(stderr, "<%03x>", v);
		fflush(stderr);
	}

	digitalWrite(RASPI_D8, (v & 0x100) ? HIGH : LOW);
	digitalWrite(RASPI_D7, (v & 0x080) ? HIGH : LOW);
	digitalWrite(RASPI_D6, (v & 0x040) ? HIGH : LOW);
	digitalWrite(RASPI_D5, (v & 0x020) ? HIGH : LOW);
	digitalWrite(RASPI_D4, (v & 0x010) ? HIGH : LOW);
	digitalWrite(RASPI_D3, (v & 0x008) ? HIGH : LOW);
	digitalWrite(RASPI_D2, (v & 0x004) ? HIGH : LOW);
	digitalWrite(RASPI_D1, (v & 0x002) ? HIGH : LOW);
	digitalWrite(RASPI_D0, (v & 0x001) ? HIGH : LOW);

	epsilon_sleep();
	digitalWrite(RASPI_CLK, HIGH);
	epsilon_sleep();
	digitalWrite(RASPI_CLK, LOW);
	epsilon_sleep();
}

int recv_word(int timeout = 0)
{
	if (current_send_recv_mode != 'r')
	{
		pinMode(RASPI_D8, INPUT);
		pinMode(RASPI_D7, INPUT);
		pinMode(RASPI_D6, INPUT);
		pinMode(RASPI_D5, INPUT);
		pinMode(RASPI_D4, INPUT);
		pinMode(RASPI_D3, INPUT);
		pinMode(RASPI_D2, INPUT);
		pinMode(RASPI_D1, INPUT);
		pinMode(RASPI_D0, INPUT);

		digitalWrite(RASPI_DIR, LOW);
		epsilon_sleep();

		current_send_recv_mode = 'r';
	}

	int v = 0;

	if (digitalRead(RASPI_D8) == HIGH) v |= 0x100;
	if (digitalRead(RASPI_D7) == HIGH) v |= 0x080;
	if (digitalRead(RASPI_D6) == HIGH) v |= 0x040;
	if (digitalRead(RASPI_D5) == HIGH) v |= 0x020;
	if (digitalRead(RASPI_D4) == HIGH) v |= 0x010;
	if (digitalRead(RASPI_D3) == HIGH) v |= 0x008;
	if (digitalRead(RASPI_D2) == HIGH) v |= 0x004;
	if (digitalRead(RASPI_D1) == HIGH) v |= 0x002;
	if (digitalRead(RASPI_D0) == HIGH) v |= 0x001;

	epsilon_sleep();
	digitalWrite(RASPI_CLK, HIGH);
	epsilon_sleep();
	digitalWrite(RASPI_CLK, LOW);
	epsilon_sleep();

	if (verbose)
	{
		if (v != last_recv_v) {
			last_recv_v = v;
			last_recv_rep = 0;
		} else {
			last_recv_rep++;
		}

		if ((v == 0x1fe || v == 0x1ff) && last_recv_rep > 0) {
			if (last_recv_rep == 1) {
				fprintf(stderr, "[%03x..]", v);
				if (v == 0x1ff)
					fprintf(stderr, "\r\n");
				fflush(stderr);
			}
		} else {
			fprintf(stderr, "[%03x]", v);
			if (v == 0x1ff)
				fprintf(stderr, "\r\n");
			fflush(stderr);
		}
	}

	if (v >= 0x100)
		current_recv_ep = v & 0xff;

	if (timeout && (v == 0x1ff || v == 0x1fe)) {
		if (timeout == 1) {
			fprintf(stderr, "Timeout!\n");
			exit(1);
		}
		return recv_word(timeout - 1);
	}

	return v;
}

void link_sync(int trignum = -1)
{
	while (recv_word() != 0x1ff) { }

	send_word(0x1ff);
	send_word(0x0ff);

	if (trignum >= 0)
		send_word(trignum);

	while (recv_word() == 0x1fe) { }
}

void test_link()
{
	link_sync();
	send_word(0x100);

	srandom(time(NULL));

	for (int k = 0; k < 1000; k++)
	{
		int data_out[20], data_in[20], data_exp[20];

		fprintf(stderr, "Round %d:\n", k);

		for (int i = 0; i < 20; i++) {
			data_out[i] = random() & 255;
			data_exp[i] = (((data_out[i] << 5) + data_out[i]) ^ 7) & 255;
			send_word(data_out[i]);
		}

		for (int i = 0; i < 20; i++)
			fprintf(stderr, "%5d", data_out[i]);
		fprintf(stderr, "\n");

		for (int i = 0; i < 20; i++)
			do {
				data_in[i] = recv_word(64);
			} while (data_in[i] >= 0x100 || current_recv_ep != 0);

		for (int i = 0; i < 20; i++)
			fprintf(stderr, "%5d", data_in[i]);
		fprintf(stderr, "\n");

		for (int i = 0; i < 20; i++)
			if (data_in[i] == data_exp[i])
				fprintf(stderr, "%5s", "ok");
			else
				fprintf(stderr, " E%3d", data_exp[i]);
		fprintf(stderr, "\n");

		for (int i = 0; i < 20; i++)
			if (data_in[i] != data_exp[i]) {
				fprintf(stderr, "Test(s) failed!\n");
				exit(1);
			}
	}

	fprintf(stderr, "All tests passed.\n");
}

void test_bw()
{
	fprintf(stderr, "Sending and receiving 1 MB of test data ..\n");

	link_sync();
	send_word(0x100);

	for (int k = 0; k < 1024*1024/64; k++)
	{
		for (int i = 0; i < 64; i++)
			send_word(k ^ i);

		while (recv_word() != 0x1ff) { }
	}
}

void write_endpoint(int epnum, int trignum)
{
	link_sync(trignum);
	send_word(0x100 + epnum);

	for (int i = 0;; i++)
	{
		int byte = getchar();
		if (byte < 0 || 255 < byte)
			break;
		send_word(byte);

		if (i % 128 == 127)
			while (recv_word() != 0x1ff) { }
	}

	if (send_zero)
		send_word(0);

	link_sync();
}

void read_endpoint(int epnum, int trignum)
{
	link_sync(trignum);

	bool pending_fflush = false;

	for (int timeout = 0; timeout < 1000 || recv_zero; timeout++) {
		int byte = recv_word();
		if (current_recv_ep == epnum && byte < 0x100) {
			if (recv_zero && byte == 0)
				break;
			putchar(byte);
			pending_fflush = true;
			timeout = 0;
		} else if (pending_fflush) {
			pending_fflush = false;
			fflush(stdout);
		}
	}

	link_sync();
}

void console_endpoint(int epnum, int trignum)
{
	struct termios oldkey_stdin, oldkey_stdout, newkey;

	link_sync(trignum);
	send_word(0x100 + epnum);

	tcgetattr(STDIN_FILENO, &oldkey_stdin);
	tcgetattr(STDOUT_FILENO, &oldkey_stdout);
	memset(&newkey, 0, sizeof(newkey));
	newkey.c_cflag = B9600 | CS8 | CLOCAL | CREAD;
	newkey.c_iflag = IGNPAR;
	newkey.c_oflag = ONLCR;
	newkey.c_lflag = 0;
	newkey.c_cc[VMIN]=1;
	newkey.c_cc[VTIME]=0;
	tcflush(STDIN_FILENO, TCIFLUSH);
	tcflush(STDOUT_FILENO, TCIFLUSH);
	tcsetattr(STDIN_FILENO, TCSANOW, &newkey);
	tcsetattr(STDOUT_FILENO, TCSANOW, &newkey);

	bool running = true;
	int wrcount = 0;

	while (running)
	{
		struct timeval timeout = { 0, 10000 };
		int max_fd = STDIN_FILENO+1;
		fd_set fds;

		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);

		int ret = select(max_fd, &fds, NULL, NULL, &timeout);

		if (ret < 0)
			break;

		if (ret > 0) {
			char ch = 0;
			ret = read(STDIN_FILENO, &ch, 1);
			if (ret == 0 || ch == 3) {
				running = false;
				if (send_zero)
					send_word(ch);
				usleep(10000);
			} else {
				wrcount++;
				send_word(ch);
				if (wrcount < 200)
					continue;
			}
		}

		int stop_cnt = 0;
		while (1) {
			int v = recv_word();
			if ((v == 0x1ff) || (v == 0x1fe && wrcount < 100)) {
				if (v == 0x1ff)
					wrcount = 0;
				if (!running && stop_cnt < 10) {
					stop_cnt++;
					usleep(10000);
					continue;
				}
				if (!running && recv_zero)
					continue;
				break;
			}
			if (current_recv_ep == epnum && v < 0x100) {
				char ch = v;
				if (recv_zero && ch == 0)
					break;
				if (ch == '\n')
					write(STDOUT_FILENO, "\r", 1);
				write(STDOUT_FILENO, &ch, 1);
				stop_cnt = 0;
			}
		}
	}

	tcsetattr(STDIN_FILENO, TCSANOW, &oldkey_stdin);
	tcsetattr(STDOUT_FILENO, TCSANOW, &oldkey_stdout);

	link_sync();
}

void read_dbgvcd(int nbits)
{
	link_sync(1);

	fprintf(stderr, "Waiting for trigger: ");
	printf("$var event 1 ! clock $end\n");
	for (int i = 0; i < nbits; i++)
		printf("$var wire 1 n%d debug_%d $end\n", i, i);
	printf("$enddefinitions $end\n");

	int nbytes = (nbits+7) / 8;
	int clock_cnt = 0;
	int byte_cnt = 0;
	bool waiting = true;

	for (int timeout = 0; timeout < 1000; timeout++)
	{
		int byte = recv_word();

		if (waiting)
			timeout = 0;

		if (current_recv_ep != 1 || byte >= 0x100)
			continue;

		if (waiting) {
			fprintf(stderr, "Triggered.\n");
			waiting = false;
		}

		if (byte_cnt == 0)
			printf("#%d\n1!\n", clock_cnt);

		for (int bit = 0;  8*byte_cnt + bit < nbits && bit < 8; bit++)
			printf("b%d n%d\n", (byte >> bit) & 1, 8*byte_cnt + bit);

		if (++byte_cnt == nbytes) {
			byte_cnt = 0;
			clock_cnt++;
		}

		timeout = 0;
	}

	fprintf(stderr, "Received %d cycles of debug data.\n", clock_cnt);
	link_sync();
}

void reset_inout()
{
	pinMode(RPI_ICE_CLK,     INPUT);
	pinMode(RPI_ICE_CDONE,   INPUT);
	pinMode(RPI_ICE_MOSI,    INPUT);
	pinMode(RPI_ICE_MISO,    INPUT);
	pinMode(LOAD_FROM_FLASH, INPUT);
	pinMode(RPI_ICE_CRESET,  INPUT);
	pinMode(RPI_ICE_CS,      INPUT);
	pinMode(RPI_ICE_SELECT,  INPUT);

	pinMode(RASPI_D8, INPUT);
	pinMode(RASPI_D7, INPUT);
	pinMode(RASPI_D6, INPUT);
	pinMode(RASPI_D5, INPUT);
	pinMode(RASPI_D4, INPUT);
	pinMode(RASPI_D3, INPUT);
	pinMode(RASPI_D2, INPUT);
	pinMode(RASPI_D1, INPUT);
	pinMode(RASPI_D0, INPUT);

	pinMode(RASPI_DIR, OUTPUT);
	pinMode(RASPI_CLK, OUTPUT);

	digitalWrite(RASPI_DIR, LOW);
	digitalWrite(RASPI_CLK, LOW);

	current_send_recv_mode = 0;
}

void help(const char *progname)
{
	fprintf(stderr, "\n");
	fprintf(stderr, "Resetting FPGA:\n");
	fprintf(stderr, "    %s -R\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Resetting FPGA (reload from flash):\n");
	fprintf(stderr, "    %s -b\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Programming FPGA bit stream:\n");
	fprintf(stderr, "    %s -p < data.bin\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Programming serial flash:\n");
	fprintf(stderr, "    %s -f < data.bin\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Reading serial flash (first N bytes):\n");
	fprintf(stderr, "    %s -F N > data.bin\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Testing bit-parallel link (using ep0):\n");
	fprintf(stderr, "    %s -T\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Testing link bandwidth (using ep0):\n");
	fprintf(stderr, "    %s -B\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Writing a file to ep N:\n");
	fprintf(stderr, "    %s -w N < data.bin\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Reading a file from ep N:\n");
	fprintf(stderr, "    %s -r N > data.bin\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Console at ep N:\n");
	fprintf(stderr, "    %s -c N\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Dumping a VCD file (from ep1, using trig1)\n");
	fprintf(stderr, "  with a debug core with N bits width:\n");
	fprintf(stderr, "    %s -V N > dbg_trace.vcd\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Additional options:\n");
	fprintf(stderr, "    -v      verbose output\n");
	fprintf(stderr, "    -O N    offset (in 64 kB pages) for -f\n");
	fprintf(stderr, "    -z      send a terminating zero byte with -w/-c\n");
	fprintf(stderr, "    -Z      wait for terminating zero byte in -r/-c\n");
	fprintf(stderr, "    -t N    send trigger N before -w/-r/-c\n");
	fprintf(stderr, "\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int opt, n = -1, t = -1;
	int pageoffset = 0;
	char mode = 0;

	while ((opt = getopt(argc, argv, "RbpfF:TBw:r:c:vzZt:O:V:")) != -1)
	{
		switch (opt)
		{
		case 'w':
		case 'r':
		case 'c':
		case 'V':
		case 'F':
			n = atoi(optarg);
			// fall through

		case 'R':
		case 'b':
		case 'p':
		case 'f':
		case 'T':
		case 'B':
			if (mode)
				help(argv[0]);
			mode = opt;
			break;

		case 'v':
			verbose = true;
			break;

		case 'z':
			send_zero = true;
			break;

		case 'Z':
			recv_zero = true;
			break;

		case 't':
			t = atoi(optarg);
			break;

		case 'O':
			pageoffset = atoi(optarg);
			break;

		default:
			help(argv[0]);
		}
	}

	if (optind != argc || !mode)
		help(argv[0]);

	if (mode == 'R') {
		wiringPiSetup();
		reset_inout();
		prog_bitstream(true);
		reset_inout();
	}

	if (mode == 'b') {
		wiringPiSetup();
		reset_inout();
		fpga_reset();
		reset_inout();
	}
	
	if (mode == 'p') {
		wiringPiSetup();
		reset_inout();
		prog_bitstream();
		reset_inout();
	}

	if (mode == 'f') {
		wiringPiSetup();
		reset_inout();
		prog_flashmem(pageoffset);
		reset_inout();
	}

	if (mode == 'F') {
		wiringPiSetup();
		reset_inout();
		read_flashmem(n);
		reset_inout();
	}

	if (mode == 'T') {
		wiringPiSetup();
		reset_inout();
		test_link();
		reset_inout();
	}

	if (mode == 'B') {
		wiringPiSetup();
		reset_inout();
		test_bw();
		reset_inout();
	}

	if (mode == 'w') {
		wiringPiSetup();
		reset_inout();
		write_endpoint(n, t);
		reset_inout();
	}

	if (mode == 'r') {
		wiringPiSetup();
		reset_inout();
		read_endpoint(n, t);
		reset_inout();
	}

	if (mode == 'c') {
		wiringPiSetup();
		reset_inout();
		console_endpoint(n, t);
		reset_inout();
	}

	if (mode == 'V') {
		wiringPiSetup();
		reset_inout();
		read_dbgvcd(n);
		reset_inout();
	}

	if (verbose)
		fprintf(stderr, "\n");

	return 0;
}

