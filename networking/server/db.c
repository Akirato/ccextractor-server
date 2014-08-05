#include "utils.h"
#include "params.h"
#include "db.h"

#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/file.h>

#include <my_global.h>
#include <mysql.h>

MYSQL *con;
int cli_lock = -1;
int pr_lock = -1;

int init_db() {
	con = mysql_init(NULL);
	if (NULL == con)
	{
		mysql_perror("mysql_init", con);
		return -1;
	}

	if (mysql_real_connect(con, cfg.db_host, cfg.db_user, cfg.db_passwd,
				NULL, 0, NULL, 0) == NULL)
	{
		mysql_perror("mysql_real_connect", con);
		mysql_close(con);
		return -1;
	}

	if (creat_tables() < 0)
		return -1;

	if (query("TRUNCATE active_clients ;") < 0)
		return -1;

	if ((cli_lock = open(CLI_TBL_LOCK, O_RDWR | O_CREAT, S_IRWXU)) < 0)
	{
		_perror("open");
		mysql_close(con);
		return -1;
	}

	if ((pr_lock = open(PR_TBL_LOCK, O_RDWR | O_CREAT, S_IRWXU)) < 0)
	{
		_perror("open");
		mysql_close(con);
		return -1;
	}

	return 1;
}

int creat_tables()
{
	char q[QUERY_LEN];
	sprintf(q, "CREATE DATABASE IF NOT EXISTS %s ;", cfg.db_dbname);
	if (query(q) < 0)
		return -1;

	sprintf(q, "USE %s ;", cfg.db_dbname);
	if (query(q) < 0)
		return -1;


	if (query(
		"CREATE TABLE IF NOT EXISTS `clients` ("
		"  `id` int(11) NOT NULL AUTO_INCREMENT,"
		"  `address` varchar(256) COLLATE utf8_bin NOT NULL,"
		"  `port` varchar(6) COLLATE utf8_bin NOT NULL,"
		"  `date` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,"
		"  PRIMARY KEY (`id`)"
		") ENGINE=InnoDB  DEFAULT CHARSET=utf8 COLLATE=utf8_bin AUTO_INCREMENT=1 ;"
		) < 0)
	{
		return -1;
	}

	if (query(
		" CREATE TABLE IF NOT EXISTS `programs` ("
		"   `id` int(11) NOT NULL AUTO_INCREMENT,"
		"   `client_id` int(11) NOT NULL,"
		"   `start_date` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,"
		"   `end_date` timestamp NULL DEFAULT NULL,"
		"   `name` varchar(300) COLLATE utf8_bin DEFAULT NULL,"
		"   `cc` text COLLATE utf8_bin,"
		"   PRIMARY KEY (`id`),"
		"   FOREIGN KEY (`client_id`) REFERENCES clients(id)"
		" ) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8_bin AUTO_INCREMENT=1 ;"
		) < 0)
	{
		return -1;
	}

	if (query(
		" CREATE TABLE IF NOT EXISTS `active_clients` ("
		"   `id` int(11) NOT NULL,"
		"   `program_id` int(11) DEFAULT NULL,"
		"   FOREIGN KEY (`id`) REFERENCES clients(id),"
		"   FOREIGN KEY (`program_id`) REFERENCES programs(id)"
		" ) ENGINE=InnoDB DEFAULT CHARSET=latin1;"
		) < 0)
	{
		return -1;
	}

	return 1;
}

int db_add_cli(const char *host, const char *serv, id_t *new_id)
{
	assert(host != NULL);
	assert(serv != NULL);

	char q[QUERY_LEN] = {0};
	char *end = q;

	end += strmov(end, "INSERT INTO clients (address, port, date) VALUES(\'");

	end += mysql_real_escape_string(con, end, host, strlen(host));

	end += strmov(end, "\', \'");

	end += mysql_real_escape_string(con, end, serv, strlen(serv));

	end += strmov(end, "\', NOW()) ;");

	if (lock_cli_tbl() < 0)
		return -1;

	if (query(q) < 0)
	{
		unlock_cli_tbl();
		return -1;
	}

	if (db_get_last_id(new_id) < 0)
	{
		unlock_cli_tbl();
		return -1;
	}

	if (unlock_cli_tbl() < 0)
		return -1;

	return 1;
}

int db_get_last_id(id_t *new_id)
{
	if (query("SELECT MAX(id) FROM clients ;") < 0)
		return -1;

	MYSQL_RES *result = mysql_store_result(con);
	if (NULL == result)
	{
		mysql_perror("mysql_store_result", con);
		return -1;
	}

	MYSQL_ROW row = mysql_fetch_row(result);
	if (NULL == row[0])
	{
		_log("%s:%d error: row[0] == NULL", __FILE__, __LINE__);
		return -1;
	}

	*new_id = atoi(row[0]);

	return 1;
}

int db_add_active_cli(id_t id)
{
	assert(id > 0);

	char q[QUERY_LEN] = {0};
	sprintf(q, "INSERT INTO active_clients(id) VALUES(\'%u\') ;", id);

	if (query(q) < 0)
		return -1;

	return 1;
}

int db_set_pr_arctive_cli(id_t id, id_t pr_id)
{
	assert(id > 0);
	assert(pr_id > 0);

	char q[QUERY_LEN] = {0};

	sprintf(q, "UPDATE active_clients SET program_id = %u WHERE id = %u LIMIT 1 ;",
			pr_id, id);

	if (query(q) < 0)
		return -1;

	return 1;
}

int db_remove_active_cli(id_t id)
{
	assert(id > 0);

	char q[QUERY_LEN] = {0};
	sprintf(q, "DELETE FROM active_clients WHERE id = \'%u\' LIMIT 1 ;", id);

	if (query(q) < 0)
		return -1;

	return 1;
}

int db_add_program(id_t cli_id, id_t *pr_id, time_t start, char *name)
{
	assert(cli_id > 0);
	assert(pr_id != NULL);

	if (lock_pr_tbl() < 0)
		return -1;

	char q[QUERY_LEN] = {0};
	char *end = q;

	if (NULL == name)
		end += strmov(end, "INSERT INTO programs (client_id, start_date) VALUES(\'");
	else
		end += strmov(end, "INSERT INTO programs (client_id, start_date, name) VALUES(\'");

	end += sprintf(end, "%u", cli_id);

	end += sprintf(end, "\', FROM_UNIXTIME(%lu)", (unsigned long) start);

	if (name != NULL)
	{
		end += strmov(end, ", \'");
		end += mysql_real_escape_string(con, end, name, strlen(name));
		*end++ = '\'';
	}

	end += strmov(end, ") ;");

	if (query(q) < 0)
	{
		unlock_pr_tbl();
		return -1;
	}

	if (db_get_last_pr_id(pr_id) < 0)
	{
		unlock_pr_tbl();
		return -1;
	}

	if (unlock_pr_tbl() < 0)
		return -1;

	return 1;
}

int db_get_last_pr_id(id_t *pr_id)
{
	if (query("SELECT MAX(id) FROM programs ;") < 0)
		return -1;

	MYSQL_RES *result = mysql_store_result(con);
	if (NULL == result)
	{
		mysql_perror("mysql_store_result", con);
		return -1;
	}

	MYSQL_ROW row = mysql_fetch_row(result);
	if (NULL == row[0])
	{
		_log("%s:%d error: row[0] == NULL", __FILE__, __LINE__);
		return -1;
	}

	*pr_id = atoi(row[0]);

	return 1;
}

int db_set_pr_name(id_t pr_id, char *name)
{
	assert(pr_id > 0);
	assert(name != NULL);

	char q[QUERY_LEN] = {0};
	char *end = q;

	end += strmov(end, "UPDATE programs SET name = \'");

	end += mysql_real_escape_string(con, end, name, strlen(name));

	end += sprintf(end, "\' WHERE id = \'%u\' LIMIT 1 ;", pr_id);

	if (query(q) < 0)
		return -1;

	return 1;
}

int db_set_pr_endtime(id_t pr_id)
{
	assert(pr_id > 0);

	char q[QUERY_LEN] = {0};

	sprintf(q, "UPDATE programs SET end_date = NOW() WHERE id = \'%u\' LIMIT 1 ;",
			pr_id);

	if (query(q) < 0)
		return -1;

	return 1;
}

int db_append_cc(id_t pr_id, char *cc, size_t len)
{
	assert(pr_id > 0);
	assert(len > 0);
	assert(cc != NULL);

	char q[QUERY_LEN] = {0};
	char *e = q;

	e += strmov(e, "UPDATE programs SET cc = IFNULL(CONCAT(cc, \'");

	e += mysql_real_escape_string(con, e, cc, len);

	e += strmov(e, "\'), \'");

	e += mysql_real_escape_string(con, e, cc, len);

	e += sprintf(e, "\') WHERE id = \'%u\' LIMIT 1 ;", pr_id);

	if (query(q) < 0)
		return -1;

	return 1;
}

int lock_cli_tbl()
{
	assert(cli_lock >= 0);

	if (0 != flock(cli_lock, LOCK_EX)) 
	{
		_perror("flock");
		return -1;
	}

	return 1;
}

int lock_pr_tbl()
{
	assert(pr_lock >= 0);

	if (0 != flock(pr_lock, LOCK_EX)) 
	{
		_perror("flock");
		return -1;
	}

	return 1;
}

int unlock_cli_tbl()
{
	assert(cli_lock >= 0);

	if (0 != flock(cli_lock, LOCK_UN)) 
	{
		_perror("flock");
		return -1;
	}

	return 1;
}

int unlock_pr_tbl()
{
	assert(pr_lock >= 0);

	if (0 != flock(pr_lock, LOCK_UN)) 
	{
		_perror("flock");
		return -1;
	}

	return 1;
}

int query(const char *q)
{
	if (mysql_real_query(con, q, strlen(q)))
	{
		_log("MySQL query: %s\n", q);
		mysql_perror("mysql_real_query", con);
		return -1;
	}

	return 1;
}
