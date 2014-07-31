#include "utils.h"
#include "params.h"
#include "parser.h"
#include "networking.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <time.h>

#include <unistd.h>
#include <sys/file.h>
#include <limits.h> 

id_t cli_id;

file_t buf;
unsigned buf_line_cnt;
unsigned cur_line;

file_t txt;
file_t xds;

char *program_name; /* not null-terminated */
id_t program_id;
size_t program_len;

int pipe_w; /* pipe write end */

pid_t fork_parser(id_t id, const char *cce_output, int pipe)
{
	assert(cce_output != NULL);
	assert(pipe >= 0);

	pid_t pid = fork();

	if (pid < 0)
	{
		_perror("fork");
		return -1;
	}
	else if (pid != 0)
	{
		c_log(id, "Parser forked, pid=%d\n", pid);
		return pid;
	}

	cli_id = id;
	pipe_w = pipe;

	if (open_buf_file() < 0)
		exit(EXIT_FAILURE);

	if (parser_loop(cce_output) < 0)
		exit(EXIT_FAILURE);

	exit(EXIT_SUCCESS);
}

int parser_loop(const char *cce_output)
{
	FILE *fp = fopen(cce_output, "w+");
	if (NULL == fp)
	{
		_perror("fopen");
		return -1;
	}

	char *line = NULL;
	size_t len = 0;
	int rc;

	fpos_t pos;
	while (1)
	{
		if (fgetpos(fp, &pos) < 0)
		{
			_perror("fgetpos");
			exit(EXIT_FAILURE);
		}

		if ((rc = getline(&line, &len, fp)) <= 0)
		{
			clearerr(fp);
			if (fsetpos(fp, &pos) < 0)
			{
				_perror("fgetpos");
				exit(EXIT_FAILURE);
			}

			if (nanosleep((struct timespec[]){{0, INF_READ_DELAY}}, NULL) < 0)
			{
				_perror("nanosleep");
				exit(EXIT_FAILURE);
			}

			continue;
		}

		if (parse_line(line, rc) < 0)
			return -1;
	}

	return 1;
}

int parse_line(const char *line, size_t len)
{
	int mode = CAPTIONS;
	const char *rc;
	if ((rc = is_xds(line)) != NULL)
	{
		mode = XDS;
		if ((is_program_changed(rc)) && handle_program_change() < 0)
			return -1;
	}
	else if (append_to_txt(line, len) < 0)
		return -1;

	if (append_to_xds(line, len) < 0)
		return -1;

	if (append_to_buf(line, len, mode) < 0)
		return -1;


	return 1;
}

int is_program_changed(const char *line)
{
	static const char *pr = "Program name: ";
	size_t len = strlen(pr);
	if (strncmp(line, pr, len) != 0)
		return FALSE;

	const char *name = line + len;
	len = strlen(name);

	char *nice_name = nice_str(name, &len);
	if (NULL == nice_name)
		return FALSE;

	if (NULL == program_name || len != program_len)
		goto ok;

#if 0
	if (strncmp(program_name, name, len) == 0)
	{
		free(nice_name);
		return FALSE;
	}
#endif

ok:
	if (program_name != NULL)
		free(program_name);
	program_name = nice_name;
	program_len = len;
	return TRUE;
}

const char *is_xds(const char *line)
{
	char *pipe; 
	if ((pipe = strchr(line, '|')) == NULL)
		return NULL;
	pipe++;
	if ((pipe = strchr(pipe, '|')) == NULL)
		return NULL;
	pipe++;

	static const char *s = "XDS";
	if (strncmp(pipe, s, 3) == 0)
		return pipe + 4; /* without '|' */

	return NULL;
}

int handle_program_change()
{
	c_log(cli_id, "Program changed\n");

	if (send_prgm_to_parent() < 0)
		return -1;

	if (send_prgm_to_buf() < 0)
		return -1;

	if (reopen_txt_file() < 0)
		return -1;

	if (reopen_xds_file() < 0)
		return -1;

	program_id++;

	return 1;
}

int send_prgm_to_parent()
{
	char id_str[INT_LEN] = {0};
	sprintf(id_str, "%u", program_id);

	int rc;
	if ((rc = write_block(pipe_w, PROGRAM_ID, id_str, INT_LEN)) < 0)
	{
		m_perror("write_block", rc);
		return -1;
	}

	int c = PROGRAM_CHANGED;
	if (0 == program_id)
		c = PROGRAM_NEW;

	if ((rc = write_block(pipe_w, c, program_name, program_len)) < 0)
	{
		m_perror("write_block", rc);
		return -1;
	}

	return 1;
}

int send_prgm_to_buf()
{
	char id_str[INT_LEN] = {0};
	sprintf(id_str, "%u", program_id);

	int rc;
	if ((rc = append_to_buf(id_str, INT_LEN, PROGRAM_ID)) < 0)
		return -1;

	int c = PROGRAM_CHANGED;
	if (0 == program_id)
		c = PROGRAM_NEW;

	if ((rc = append_to_buf(program_name, program_len, c)) < 0)
		return -1;

	return 1;
}

int append_to_txt(const char *line, size_t len)
{
	if (txt.fp == NULL && open_txt_file() < 0)
		return -1;

	fwrite(line, sizeof(char), len, txt.fp);

	return 1;
}

int append_to_xds(const char *line, size_t len)
{
	if (xds.fp == NULL && open_xds_file() < 0)
		return -1;

	fwrite(line, sizeof(char), len, xds.fp);

	return 1;
}

int append_to_buf(const char *line, size_t len, char mode)
{
	char *tmp = nice_str(line, &len);
	if (NULL == tmp && line != NULL)
		return -1;

	if (0 == len)
	{
		free(tmp);
		return 1;
	}

	int rc; 
	if (buf_line_cnt >= cfg.buf_max_lines)
	{
		if ((rc = delete_n_lines(&buf.fp, buf.path,
					buf_line_cnt - cfg.buf_min_lines)) < 0)
		{
			m_perror("delete_n_lines", rc);
			free(tmp);
			return -1;
		}
		buf_line_cnt = cfg.buf_min_lines;
	}

	if (0 != flock(fileno(buf.fp), LOCK_EX)) 
	{
		_perror("flock");
		free(tmp);
		return -1;
	}

	fprintf(buf.fp, "%d %d ", cur_line, mode);

	if (tmp != NULL && len > 0)
	{
		fprintf(buf.fp, "%zd ", len);
		fwrite(tmp, sizeof(char), len, buf.fp);
	}

	fprintf(buf.fp, "\r\n");

	cur_line++;
	buf_line_cnt++;

	if (0 != flock(fileno(buf.fp), LOCK_UN))
	{
		_perror("flock");
		free(tmp);
		return -1;
	}

	free(tmp);

	return 1;
}

int open_txt_file()
{
	assert(txt.fp == NULL);

	if (file_path(&txt.path, "txt", cli_id, program_id) < 0)
		return -1;

	if ((txt.fp = fopen(txt.path, "w+x")) == NULL)
	{
		_perror("fopen");
		return -1;
	}

	if (setvbuf(txt.fp, NULL, _IOLBF, 0) < 0) 
	{
		_perror("setvbuf");
		return -1;
	}

	return 1;
}

int reopen_txt_file()
{
	assert(txt.fp != NULL);
	assert(txt.path != NULL);

	fclose(txt.fp);
	txt.fp = NULL;

	free(txt.path);
	txt.path = NULL;

	return open_txt_file();
}

int open_xds_file()
{
	assert(xds.fp == NULL);

	if (file_path(&xds.path, "xds.txt", cli_id, program_id) < 0)
		return -1;

	if ((xds.fp = fopen(xds.path, "w+")) == NULL)
	{
		_perror("fopen");
		return -1;
	}

	if (setvbuf(xds.fp, NULL, _IOLBF, 0) < 0) 
	{
		_perror("setvbuf");
		return -1;
	}

	return 1;
}

int reopen_xds_file()
{
	assert(xds.fp != NULL);
	assert(xds.path != NULL);

	fclose(xds.fp);
	xds.fp = NULL;

	free(xds.path);
	xds.path = NULL;

	return open_xds_file();
}

int open_buf_file()
{
	assert(buf.path == NULL);
	assert(buf.fp == NULL);

	if ((buf.path = (char *) malloc (PATH_MAX)) == NULL)
	{
		_perror("malloc");
		return -1;
	}

	snprintf(buf.path, PATH_MAX, "%s/%u.txt", cfg.buf_dir, cli_id);

	if ((buf.fp = fopen(buf.path, "w+")) == NULL)
	{
		_perror("fopen");
		return -1;
	}

	if (setvbuf(buf.fp, NULL, _IOLBF, 0) < 0) 
	{
		_perror("setvbuf");
		return -1;
	}

	c_log(cli_id, "Buffer file opened: %s\n", buf.path);

	return 1;
}

int file_path(char **path, const char *ext, id_t cli_id, id_t prgm_id)
{
	assert(ext != NULL);

	if (NULL == *path && (*path = (char *) malloc(PATH_MAX)) == NULL) 
	{
		_perror("malloc");
		return -1;
	}

	time_t t = time(NULL);
	struct tm *t_tm = localtime(&t);
	char time_buf[30] = {0};
	strftime(time_buf, 30, "%G/%m-%b/%d", t_tm);

	char dir[PATH_MAX] = {0};
	snprintf(dir, PATH_MAX, "%s/%s", cfg.arch_dir, time_buf);

	if (_mkdir(dir, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH ) < 0)
	{
		_perror("_mkdir");
		return -1;
	}

	memset(time_buf, 0, sizeof(time_buf));
	strftime(time_buf, 30, "%H%M%S", t_tm);

	snprintf(*path, PATH_MAX, "%s/%s-%u-%u.%s", dir, time_buf, cli_id, prgm_id, ext); 

	return 0;
}

/* int append_to_arch_info() */
/* { */
/* 	time_t t = time(NULL); */
/* 	struct tm *t_tm = localtime(&t); */
/*  */
/* 	char time_buf[30] = {0}; */
/* 	strftime(time_buf, 30, "%G/%m-%b/%d", t_tm); */
/*  */
/* 	char info_filepath[PATH_MAX]; */
/* 	snprintf(info_filepath, PATH_MAX, "%s/%s/%s",  */
/*     		cfg.arch_dir, */
/*     		time_buf, */
/*     		cfg.arch_info_filename); */
/*  */
/* 	FILE *info_fp = fopen(info_filepath, "a"); */
/* 	if (NULL == info_fp) */
/* 	{ */
/* 		_log("fopen() error: %s\n", strerror(errno)); */
/* 		return -1; */
/* 	} */
/*  */
/* 	fprintf(info_fp, "%d %u %s:%s %s %s %s\n", */
/* 			(int) t, */
/* 			cli->id, */
/* 			cli->host, */
/* 			cli->serv, */
/* 			cli_chld->bin_filepath, */
/* 			cli_chld->txt.path, */
/*  */
/* 	fclose(info_fp); */
/*  */
/* 	return 0; */
/* } */