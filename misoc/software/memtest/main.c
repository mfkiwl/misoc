#include <stdio.h>
#include <stdlib.h>

#include <irq.h>
#include <uart.h>
#include <time.h>
#include <generated/csr.h>
#include <generated/mem.h>
#include <hw/flags.h>
#include <console.h>
#include <system.h>

static unsigned int log2(unsigned int v)
{
  unsigned int r;
  r = 0;
  while(v>>=1) r++;
  return r;
}

static void membw_service(void)
{
	static int last_event;
	unsigned long long int nr, nw;
	unsigned int rdb, wrb;
	unsigned int dw;

	if(elapsed(&last_event, CONFIG_CLOCK_FREQUENCY)) {
		sdram_controller_bandwidth_update_write(1);
		nr = sdram_controller_bandwidth_nreads_read();
		nw = sdram_controller_bandwidth_nwrites_read();
		dw = sdram_controller_bandwidth_data_width_read();
		rdb = (nr*CONFIG_CLOCK_FREQUENCY >> (24 - log2(dw)))/1000000ULL;
		wrb = (nw*CONFIG_CLOCK_FREQUENCY >> (24 - log2(dw)))/1000000ULL;
		printf("read:%5dMbps  write:%5dMbps  all:%5dMbps\n", rdb, wrb, rdb + wrb);
	}
}

//#define DEBUG

static void memtest_service(void)
{
	static unsigned int test_buffer[(MAIN_RAM_SIZE/2)/4] __attribute__((aligned(16)));
	static unsigned char reading;
	static unsigned int err, total_err;
#ifdef DEBUG
	int i;
#endif

	if(reading) {
		if(!memtest_w_busy_read()) {
#ifdef DEBUG
			flush_l2_cache();
			flush_cpu_dcache();
			printf("starting read\n");
			for(i=0;i<64;i++) {
				printf("%08x", test_buffer[i]);
				if((i % 4) == 3)
					printf("\n");
			}
#endif
			memtest_r_reset_write(1);
			memtest_r_base_write((unsigned int)test_buffer);
			memtest_r_length_write(sizeof(test_buffer));
			memtest_r_shoot_write(1);
			reading = 0;
		}
	} else {
		if(!memtest_r_busy_read()) {
			err = memtest_r_error_count_read();
			total_err += err;
			printf("err=%d\t\ttotal=%d\n", err, total_err);
			memtest_w_reset_write(1);
			memtest_w_base_write((unsigned int)test_buffer);
			memtest_w_length_write(sizeof(test_buffer));
			memtest_w_shoot_write(1);
			reading = 1;
		}
	}
}

int main(void)
{
	irq_setmask(0);
	irq_setie(1);
	uart_init();

	puts("Memory testing software built "__DATE__" "__TIME__"\n");

	if((memtest_w_magic_read() != 0x361f) || (memtest_r_magic_read() != 0x361f)) {
		printf("Memory test cores not detected\n");
		while(1);
	}

	time_init();

	flush_l2_cache();
	while(1) {
		memtest_service();
		membw_service();
	}

	return 0;
}
