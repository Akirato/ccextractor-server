#include "networking.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <locale.h>

#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define BUF_SIZE 20480

#define ull unsigned long long

int main(int argc, char *argv[])
{
	(void) setlocale(LC_ALL, "");

	if (4 != argc) {
		fprintf(stderr, "Usage: %s host port file\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	connect_to_srv(argv[1], argv[2]);

	FILE *fp = fopen(argv[3], "r");
	if (NULL == fp)
	{
		perror("fopen() error");
		exit(EXIT_FAILURE);
	}

	char buf[BUF_SIZE] = {0};
	char *p = buf;

	size_t rc = 0;

	if ((rc = fread(buf, sizeof(char), 11, fp)) != 11)
	{
		printf("%d, Premature end of file!\n", __LINE__);
		exit(EXIT_FAILURE);
	}

	if (memcmp(buf, "\xCC\xCC\xED", 3))
	{
		printf("no header\n");
		exit(0);
	}

	net_send_header(buf, 11);

	uint16_t blk_cnt;
	size_t len;


	while (1)
	{
		p = buf;

		if ((rc = fread(p, sizeof(char), 10, fp)) != 10)
		{
			printf("%d, Premature end of file!\n", __LINE__);
			exit(EXIT_FAILURE);
		}
		
		p += rc;
		if ((blk_cnt = *((uint16_t *)(p - 2))) == 0)
			continue;


		if ((len = blk_cnt * 3) > BUF_SIZE) 
		{
			fprintf(stderr, "buffer overflow\n");
			exit(EXIT_FAILURE);
		}

		if ((rc = fread(p, sizeof(char), len, fp)) != len)
		{
			printf("%d, Premature end of file!\n", __LINE__);
			exit(EXIT_FAILURE);
		}
		p += rc;

		net_send_cc(buf, len + 10);

		sleep(1);
	}

	fclose(fp);

	return 0;
}
