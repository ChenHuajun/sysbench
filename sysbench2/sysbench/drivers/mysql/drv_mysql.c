/* Copyright (C) 2004 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifdef STDC_HEADERS
# include <stdio.h>
#endif
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#ifdef _WIN32
#include <winsock2.h>
#endif

#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif
#include <stdio.h>

#include <mysql.h>
#include <mysqld_error.h>

#include "sb_options.h"
#include "db_driver.h"

/* Check MySQL version for prepared statements availability */
#if MYSQL_VERSION_ID >= 40103
# define HAVE_PS
#endif

/* Check if we should use the TYPE= (for 3.23) or ENGINE= syntax */
#if MYSQL_VERSION_ID >= 40000
# define ENGINE_CLAUSE "ENGINE"
#else
# define ENGINE_CLAUSE "TYPE"
#endif

#define DEBUG(format, ...) do { if (args.debug) log_text(LOG_DEBUG, format, __VA_ARGS__); } while (0)

#define SAFESTR(s) ((s != NULL) ? (s) : "(null)")

/* FIXME */
db_bind_t *gresults;

/* MySQL driver arguments */

static sb_arg_t mysql_drv_args[] =
{
  {"mysql-host", "MySQL server host", SB_ARG_TYPE_LIST, "localhost"},
  {"mysql-port", "MySQL server port", SB_ARG_TYPE_INT, "3306"},
  {"mysql-socket", "MySQL socket", SB_ARG_TYPE_LIST, NULL},
  {"mysql-user", "MySQL user", SB_ARG_TYPE_STRING, "sbtest"},
  {"mysql-password", "MySQL password", SB_ARG_TYPE_STRING, ""},
  {"mysql-db", "MySQL database name", SB_ARG_TYPE_STRING, "sbtest"},
  {"mysql-table-engine", "storage engine to use for the test table {myisam,innodb,bdb,heap,ndbcluster,federated}",
   SB_ARG_TYPE_STRING, "innodb"},
  {"mysql-engine-trx", "whether storage engine used is transactional or not {yes,no,auto}",
   SB_ARG_TYPE_STRING, "auto"},
  {"mysql-ssl", "use SSL connections, if available in the client library", SB_ARG_TYPE_FLAG, "off"},
  {"myisam-max-rows", "max-rows parameter for MyISAM tables", SB_ARG_TYPE_INT, "1000000"},
  {"mysql-debug", "dump all client library calls", SB_ARG_TYPE_FLAG, "off"},
  
  {NULL, NULL, SB_ARG_TYPE_NULL, NULL}
};

typedef struct
{
  sb_list_t          *hosts;
  unsigned int       port;
  sb_list_t          *sockets;
  char               *user;
  char               *password;
  char               *db;
  unsigned char      use_ssl;
  unsigned char      debug;
} mysql_drv_args_t;

#ifdef HAVE_PS
/* Structure used for DB-to-MySQL bind types map */

typedef struct
{
  db_bind_type_t   db_type;
  int              my_type;
} db_mysql_bind_map_t;

/* DB-to-MySQL bind types map */
db_mysql_bind_map_t db_mysql_bind_map[] =
{
  {DB_TYPE_TINYINT,   MYSQL_TYPE_TINY},
  {DB_TYPE_SMALLINT,  MYSQL_TYPE_SHORT},
  {DB_TYPE_INT,       MYSQL_TYPE_LONG},
  {DB_TYPE_BIGINT,    MYSQL_TYPE_LONGLONG},
  {DB_TYPE_FLOAT,     MYSQL_TYPE_FLOAT},
  {DB_TYPE_DOUBLE,    MYSQL_TYPE_DOUBLE},
  {DB_TYPE_DATETIME,  MYSQL_TYPE_DATETIME},
  {DB_TYPE_TIMESTAMP, MYSQL_TYPE_TIMESTAMP},
  {DB_TYPE_CHAR,      MYSQL_TYPE_STRING},
  {DB_TYPE_VARCHAR,   MYSQL_TYPE_VAR_STRING},
  {DB_TYPE_NONE,      0}
};

#endif /* HAVE_PS */

/* MySQL driver capabilities */

static drv_caps_t mysql_drv_caps =
{
  1,
  0,
  1,
  0,
  0,
  1
};



static mysql_drv_args_t args;          /* driver args */

static char use_ps; /* whether server-side prepared statemens should be used */

/* Positions in the list of hosts/sockets protected by hosts_mutex */
static sb_list_item_t *hosts_pos;
static sb_list_item_t *sockets_pos;

static pthread_mutex_t hosts_mutex;

/* MySQL driver operations */

static int mysql_drv_init(void);
static int mysql_drv_describe(drv_caps_t *);
static int mysql_drv_connect(db_conn_t *);
static int mysql_drv_disconnect(db_conn_t *);
static int mysql_drv_prepare(db_stmt_t *, const char *);
static int mysql_drv_bind_param(db_stmt_t *, db_bind_t *, unsigned int);
static int mysql_drv_bind_result(db_stmt_t *, db_bind_t *, unsigned int);
static int mysql_drv_execute(db_stmt_t *, db_result_set_t *);
static int mysql_drv_fetch(db_result_set_t *);
static int mysql_drv_fetch_row(db_result_set_t *, db_row_t *);
static unsigned long long mysql_drv_num_rows(db_result_set_t *);
static int mysql_drv_query(db_conn_t *, const char *, db_result_set_t *);
static int mysql_drv_free_results(db_result_set_t *);
static int mysql_drv_close(db_stmt_t *);
static int mysql_drv_store_results(db_result_set_t *);
static int mysql_drv_done(void);

/* MySQL driver definition */

static db_driver_t mysql_driver =
{
  "mysql",
  "MySQL driver",
  mysql_drv_args,
  {
    mysql_drv_init,
    mysql_drv_describe,
    mysql_drv_connect,
    mysql_drv_disconnect,
    mysql_drv_prepare,
    mysql_drv_bind_param,
    mysql_drv_bind_result,
    mysql_drv_execute,
    mysql_drv_fetch,
    mysql_drv_fetch_row,
    mysql_drv_num_rows,
    mysql_drv_free_results,
    mysql_drv_close,
    mysql_drv_query,
    mysql_drv_store_results,
    mysql_drv_done
  },
  {0,0}
};


/* Local functions */

#ifdef HAVE_PS
static int get_mysql_bind_type(db_bind_type_t);
#endif

/* Register MySQL driver */


int register_driver_mysql(sb_list_t *drivers)
{
  SB_LIST_ADD_TAIL(&mysql_driver.listitem, drivers);

  return 0;
}


/* MySQL driver initialization */


int mysql_drv_init(void)
{
  args.hosts = sb_get_value_list("mysql-host");
  if (SB_LIST_IS_EMPTY(args.hosts))
  {
    log_text(LOG_FATAL, "No MySQL hosts specified, aborting");
    return 1;
  }
  hosts_pos = args.hosts;
  pthread_mutex_init(&hosts_mutex, NULL);

  args.sockets = sb_get_value_list("mysql-socket");
  sockets_pos = args.sockets;

  args.port = (unsigned int)sb_get_value_int("mysql-port");
  args.user = sb_get_value_string("mysql-user");
  args.password = sb_get_value_string("mysql-password");
  args.db = sb_get_value_string("mysql-db");
  args.use_ssl = sb_get_value_flag("mysql-ssl");
  args.debug = sb_get_value_flag("mysql-debug");
  if (args.debug)
    sb_globals.verbosity = LOG_DEBUG;
  
  use_ps = 0;
#ifdef HAVE_PS
  mysql_drv_caps.prepared_statements = 1;
  if (db_globals.ps_mode != DB_PS_MODE_DISABLE)
    use_ps = 1;
#endif

  DEBUG("mysql_library_init(%d, %p, %p)", 0, NULL, NULL);
  mysql_library_init(0, NULL, NULL);
  
  return 0;
}


/* Describe database capabilities */


int mysql_drv_describe(drv_caps_t *caps)
{
  *caps = mysql_drv_caps;
  
  return 0;
}


/* Connect to MySQL database */


int mysql_drv_connect(db_conn_t *sb_conn)
{
  MYSQL          *con;
  char           *host;
  char           *socket;
  const char     *ssl_key;
  const char     *ssl_cert;
  const char     *ssl_ca;
  
  con = (MYSQL *)malloc(sizeof(MYSQL));
  if (con == NULL)
    return 1;
  sb_conn->ptr = con;
  
  DEBUG("mysql_init(%p)", con);
  mysql_init(con);

  pthread_mutex_lock(&hosts_mutex);

  if (SB_LIST_IS_EMPTY(args.sockets))
  {
    socket = NULL;
    hosts_pos = SB_LIST_ITEM_NEXT(hosts_pos);
    if (hosts_pos == args.hosts)
      hosts_pos = SB_LIST_ITEM_NEXT(hosts_pos);
    host = SB_LIST_ENTRY(hosts_pos, value_t, listitem)->data;
  }
  else
  {
    host = "localhost";
    sockets_pos = SB_LIST_ITEM_NEXT(sockets_pos);
    if (sockets_pos == args.sockets)
      sockets_pos = SB_LIST_ITEM_NEXT(sockets_pos);
    socket = SB_LIST_ENTRY(sockets_pos, value_t, listitem)->data;
  }
  pthread_mutex_unlock(&hosts_mutex);

#if 0
  /*
    FIXME: the following leads to crash in the client lib.
    http://bugs.mysql.com/?id=40552
  */
  mysql_options(con, MYSQL_READ_DEFAULT_GROUP, "sysbench");
  DEBUG("mysql_options(%p, MYSQL_READ_DEFAULT_GROUP, \"sysbench\")", con);
#endif

  if (args.use_ssl)
  {
    ssl_key= "client-key.pem";
    ssl_cert= "client-cert.pem";
    ssl_ca= "cacert.pem";

    DEBUG("mysql_ssl_set(%p,\"%s\", \"%s\", \"%s\", NULL, NULL)", con, ssl_key,
          ssl_cert, ssl_ca);
    mysql_ssl_set(con, ssl_key, ssl_cert, ssl_ca, NULL, NULL);
  }
  
  DEBUG("mysql_real_connect(%p, \"%s\", \"%s\", \"%s\", \"%s\", %u, \"%s\", %s)",
        con,
        SAFESTR(host),
        SAFESTR(args.user),
        SAFESTR(args.password),
        SAFESTR(args.db),
        args.port,
        SAFESTR(socket),
        (MYSQL_VERSION_ID >= 50000) ? "CLIENT_MULTI_STATEMENTS" : "0"
        );
  if (!mysql_real_connect(con,
                          host,
                          args.user,
                          args.password,
                          args.db,
                          args.port,
                          socket,
#if MYSQL_VERSION_ID >= 50000
                          CLIENT_MULTI_STATEMENTS)
#else
                          0)
#endif
      )
  {
    log_text(LOG_FATAL, "unable to connect to MySQL server, aborting...");
    log_text(LOG_FATAL, "error %d: %s", mysql_errno(con),
           mysql_error(con));
    free(con);
    return 1;
  }

  return 0;
}


/* Disconnect from MySQL database */


int mysql_drv_disconnect(db_conn_t *sb_conn)
{
  MYSQL *con = sb_conn->ptr;

  if (con != NULL)
  {
    DEBUG("mysql_close(%p)", con);
    mysql_close(con);
  }
  free(con);
  
  return 0;
}


/* Prepare statement */


int mysql_drv_prepare(db_stmt_t *stmt, const char *query)
{
#ifdef HAVE_PS
  MYSQL      *con = (MYSQL *)stmt->connection->ptr;
  MYSQL_STMT *mystmt;
  unsigned int rc;

  if (con == NULL)
    return 1;

  if (use_ps)
  {
    mystmt = mysql_stmt_init(con);
    DEBUG("mysql_stmt_init(%p) = %p", con, mystmt);
    if (mystmt == NULL)
    {
      log_text(LOG_FATAL, "mysql_stmt_init() failed");
      return 1;
    }
    stmt->ptr = (void *)mystmt;
    DEBUG("mysql_stmt_prepare(%p, \"%s\", %d) = %p", mystmt, query, strlen(query), stmt->ptr);
    if (mysql_stmt_prepare(mystmt, query, strlen(query)))
    {
      /* Check if this statement in not supported */
      rc = mysql_errno(con);
      DEBUG("mysql_errno(%p) = %u", con, rc);
      if (rc == ER_UNSUPPORTED_PS)
      {
        log_text(LOG_INFO,
                 "Failed to prepare query \"%s\" (%d: %s), using emulation",
                 query, rc, mysql_error(con));
        goto emulate;
      }
      else
      {
        log_text(LOG_FATAL, "mysql_stmt_prepare() failed");
        log_text(LOG_FATAL, "MySQL error: %d \"%s\"", rc,
                 mysql_error(con));
        DEBUG("mysql_stmt_close(%p)", mystmt);
        mysql_stmt_close(mystmt);
        return 1;
      }
    }
    stmt->query = strdup(query);
    
    return 0;
  }

 emulate:
#endif /* HAVE_PS */

  /* Use client-side PS */
  stmt->emulated = 1;
  stmt->query = strdup(query);

  return 0;
}


/* Bind parameters for prepared statement */


int mysql_drv_bind_param(db_stmt_t *stmt, db_bind_t *params, unsigned int len)
{
  MYSQL        *con = (MYSQL *)stmt->connection->ptr;
#ifdef HAVE_PS
  MYSQL_BIND   *bind;
  unsigned int i;
  my_bool rc;
  unsigned long param_count;
#endif
  
  if (con == NULL)
    return 1;

#ifdef HAVE_PS
  if (!stmt->emulated)
  {
    if (stmt->ptr == NULL)
      return 1;
    /* Validate parameters count */
    param_count = mysql_stmt_param_count(stmt->ptr);
    DEBUG("mysql_stmt_param_count(%p) = %lu", stmt->ptr, param_count);
    if (param_count != len)
    {
      log_text(LOG_FATAL, "Wrong number of parameters to mysql_stmt_bind_param");
      return 1;
    }
    /* Convert SysBench bind structures to MySQL ones */
    bind = (MYSQL_BIND *)calloc(len, sizeof(MYSQL_BIND));
    if (bind == NULL)
      return 1;
    for (i = 0; i < len; i++)
    {
      bind[i].buffer_type = get_mysql_bind_type(params[i].type);
      bind[i].buffer = params[i].buffer;
      bind[i].buffer_length = params[i].max_len;
      bind[i].length = params[i].data_len;
      bind[i].is_null = params[i].is_null;
    }

    rc = mysql_stmt_bind_param(stmt->ptr, bind);
    DEBUG("mysql_stmt_bind_param(%p, %p) = %d", stmt->ptr, bind, rc);
    if (rc)
    {
      log_text(LOG_FATAL, "mysql_stmt_bind_param() failed");
      log_text(LOG_FATAL, "MySQL error: %d \"%s\"", mysql_errno(con),
                 mysql_error(con));
      free(bind);
      return 1;
    }
    free(bind);

    return 0;
  }
#endif /* HAVE_PS */

  /* Use emulation */
  if (stmt->bound_param != NULL)
    free(stmt->bound_param);
  stmt->bound_param = (db_bind_t *)malloc(len * sizeof(db_bind_t));
  if (stmt->bound_param == NULL)
    return 1;
  memcpy(stmt->bound_param, params, len * sizeof(db_bind_t));
  stmt->bound_param_len = len;
  
  return 0;

}


/* Bind results for prepared statement */


int mysql_drv_bind_result(db_stmt_t *stmt, db_bind_t *params, unsigned int len)
{
#ifndef HAVE_PS
  /* No support for prepared statements */
  (void)stmt;   /* unused */
  (void)params; /* unused */
  (void)len;    /* unused */

  return 1;
#else
  MYSQL        *con = (MYSQL *)stmt->connection->ptr;
  MYSQL_BIND   *bind;
  unsigned int i;
  my_bool rc;
  
  if (con == NULL || stmt->ptr == NULL)
    return 1;

  /* Convert SysBench bind structures to MySQL ones */
  bind = (MYSQL_BIND *)calloc(len, sizeof(MYSQL_BIND));
  if (bind == NULL)
    return 1;
  for (i = 0; i < len; i++)
  {
    bind[i].buffer_type = get_mysql_bind_type(params[i].type);
    bind[i].buffer = params[i].buffer;
    bind[i].buffer_length = params[i].max_len;
    bind[i].length = params[i].data_len;
    bind[i].is_null = params[i].is_null;
  }

  /* FIXME */
  gresults = params;
  
  rc = mysql_stmt_bind_result(stmt->ptr, bind);
  DEBUG("mysql_stmt_bind_result(%p, %p) = %d", stmt->ptr, bind, rc);
  if (rc)
  {
    free(bind);
    return 1;
  }
  free(bind);

  return 0;
#endif /* HAVE_PS */
}


/* Execute prepared statement */


int mysql_drv_execute(db_stmt_t *stmt, db_result_set_t *rs)
{
  db_conn_t       *con = stmt->connection;
  char            *buf = NULL;
  unsigned int    buflen = 0;
  unsigned int    i, j, vcnt;
  char            need_realloc;
  int             n;
  unsigned int    rc;
 
#ifdef HAVE_PS
  (void)rs; /* unused */

  if (!stmt->emulated)
  {
    if (stmt->ptr == NULL)
    {
      log_text(LOG_DEBUG, "ERROR: exiting mysql_drv_execute(), uninitialized statement");
      return SB_DB_ERROR_FAILED;
    }
    rc = (unsigned int)mysql_stmt_execute(stmt->ptr);
    DEBUG("mysql_stmt_execute(%p) = %u", stmt->ptr, rc);
    if (rc)
    {
      rc = mysql_errno(con->ptr);
      DEBUG("mysql_errno(%p) = %u", con->ptr, rc);
      if (rc == ER_LOCK_DEADLOCK || rc == ER_LOCK_WAIT_TIMEOUT ||
          rc == ER_CHECKREAD)
        return SB_DB_ERROR_DEADLOCK;
      log_text(LOG_ALERT, "mysql_stmt_execute() for query '%s' failed: %d %s",
               stmt->query,
               mysql_errno(con->ptr),
               mysql_error(con->ptr));
      return SB_DB_ERROR_FAILED;
    }
    return SB_DB_ERROR_NONE;
  }
#else
  (void)rc; /* unused */
#endif /* HAVE_PS */

  /* Use emulation */
  /* Build the actual query string from parameters list */
  need_realloc = 1;
  vcnt = 0;
  for (i = 0, j = 0; stmt->query[i] != '\0'; i++)
  {
  again:
    if (j+1 >= buflen || need_realloc)
    {
      buflen = (buflen > 0) ? buflen * 2 : 256;
      buf = realloc(buf, buflen);
      if (buf == NULL)
      {
        log_text(LOG_DEBUG, "ERROR: exiting mysql_drv_execute(), memory allocation failure");
        return SB_DB_ERROR_FAILED;
      }
      need_realloc = 0;
    }

    if (stmt->query[i] != '?')
    {
      buf[j++] = stmt->query[i];
      continue;
    }

    n = db_print_value(stmt->bound_param + vcnt, buf + j, (int)(buflen - j));
    if (n < 0)
    {
      need_realloc = 1;
      goto again;
    }
    j += (unsigned int)n;
    vcnt++;
  }
  buf[j] = '\0';
  
  con->db_errno = mysql_drv_query(con, buf, rs);
  free(buf);
  if (con->db_errno != SB_DB_ERROR_NONE)
  {
    log_text(LOG_DEBUG, "ERROR: exiting mysql_drv_execute(), database error");
    return con->db_errno;
  }
  
  return SB_DB_ERROR_NONE;
}


/* Execute SQL query */


int mysql_drv_query(db_conn_t *sb_conn, const char *query,
                      db_result_set_t *rs)
{
  MYSQL *con = sb_conn->ptr;
  unsigned int rc;

  (void)rs; /* unused */
  rc = (unsigned int)mysql_real_query(con, query, strlen(query));
  DEBUG("mysql_real_query(%p, \"%s\", %u) = %u", con, query, strlen(query), rc);
  if (rc)
  {
    rc = mysql_errno(con);
    DEBUG("mysql_errno(%p) = %u", con, rc);
    if (rc == ER_LOCK_DEADLOCK || rc == ER_LOCK_WAIT_TIMEOUT ||
        rc == ER_CHECKREAD)
      return SB_DB_ERROR_DEADLOCK;
    log_text(LOG_ALERT, "failed to execute MySQL query: `%s`:", query);
    log_text(LOG_ALERT, "Error %d %s", mysql_errno(con), mysql_error(con));
    return SB_DB_ERROR_FAILED; 
  }  

  return SB_DB_ERROR_NONE;
}


/* Fetch row from result set of a prepared statement */


int mysql_drv_fetch(db_result_set_t *rs)
{
  /* NYI */
  (void)rs;  /* unused */
  
  return 1;
}


/* Fetch row from result set of a query */


int mysql_drv_fetch_row(db_result_set_t *rs, db_row_t *row)
{
  row->ptr = mysql_fetch_row(rs->ptr);
  DEBUG("mysql_fetch_row(%p) = %p", rs->ptr, row->ptr);
  if (row->ptr == NULL)
  {
    log_text(LOG_FATAL, "mysql_fetch_row() failed: %s",
             mysql_error(rs->connection->ptr));
    return 1;
  }
  
  return 0;
}


/* Return the number of rows in a result set */


unsigned long long mysql_drv_num_rows(db_result_set_t *rs)
{
  return rs->nrows;
}


/* Store results from the last query */


int mysql_drv_store_results(db_result_set_t *rs)
{
  MYSQL        *con = rs->connection->ptr;
  MYSQL_RES    *res;
  MYSQL_ROW    row;
  unsigned int rc;

#ifdef HAVE_PS
  /* Is this result set from prepared statement? */
  if (rs->statement != NULL && rs->statement->emulated == 0)
  {
    if (rs->statement->ptr == NULL)
      return 1;

    rc = (unsigned int)mysql_stmt_store_result(rs->statement->ptr);
    DEBUG("mysql_stmt_store_result(%p) = %d", rs->statement->ptr, rc);
    if (rc)
    {
      rc = mysql_errno(con);
      DEBUG("mysql_errno(%p) = %d", con, rc);
      if (rc == ER_LOCK_DEADLOCK || rc == ER_LOCK_WAIT_TIMEOUT ||
          rc == ER_CHECKREAD)
      {
        log_text(LOG_WARNING,
                 "mysql_stmt_store_result() failed with error: (%d) %s", rc,
                 mysql_error(con));
        return SB_DB_ERROR_DEADLOCK;
      }
      else if (mysql_stmt_field_count(rs->statement->ptr) == 0)
        return SB_DB_ERROR_NONE;
      log_text(LOG_ALERT, "MySQL error: %s\n", mysql_error(con));
      return SB_DB_ERROR_FAILED;
    }
    rs->nrows = mysql_stmt_num_rows(rs->statement->ptr);
    DEBUG("mysql_stmt_num_rows(%p) = %d", rs->statement->ptr, rs->nrows);
    do {
      rc = (unsigned int)mysql_stmt_fetch(rs->statement->ptr);
      DEBUG("mysql_stmt_fetch(%p) = %d", rs->statement->ptr, rc);
    } while(rc == 0);

    return SB_DB_ERROR_NONE;
  }
#endif
  
  if (con == NULL)
    return SB_DB_ERROR_FAILED;
  
  /* using store results for speed will not work for large sets */
  res = mysql_store_result(con);
  DEBUG("mysql_store_result(%p) = %p", con, res);
  if (res == NULL)
  {
      rc = mysql_errno(con);
      DEBUG("mysql_errno(%p) = %u", con, rc);
      if (rc == ER_LOCK_DEADLOCK || rc == ER_LOCK_WAIT_TIMEOUT ||
          rc == ER_CHECKREAD)
      {
        log_text(LOG_WARNING,
                 "mysql_store_result() failed with error: (%u) %s", rc,
                 mysql_error(con));
        return SB_DB_ERROR_DEADLOCK;
      }
      else if (mysql_field_count(con) == 0)
        return SB_DB_ERROR_NONE;
      log_text(LOG_ALERT, "MySQL error: %s", mysql_error(con));
      return SB_DB_ERROR_FAILED; 
  }
  rs->ptr = (void *)res;

  rs->nrows = mysql_num_rows(res);
  DEBUG("mysql_num_rows(%p) = %u", res, rs->nrows);
  
  /* just fetch result */
  while((row = mysql_fetch_row(res)))
    DEBUG("mysql_fetch_row(%p) = %p", res, row);
  return SB_DB_ERROR_NONE;
}


/* Free result set */


int mysql_drv_free_results(db_result_set_t *rs)
{
#ifdef HAVE_PS
  /* Is this a result set of a prepared statement */
  if (rs->statement != NULL && rs->statement->emulated == 0)
  {
    DEBUG("mysql_stmt_free_result(%p)", rs->statement->ptr);
    return mysql_stmt_free_result(rs->statement->ptr);
  }
#endif

  if (rs->ptr != NULL)
  {
    DEBUG("mysql_free_result(%p)", rs->ptr);
    mysql_free_result((MYSQL_RES *)rs->ptr);
    return 0;
  }

  return 1;
}


/* Close prepared statement */


int mysql_drv_close(db_stmt_t *stmt)
{
  if (stmt->query)
  {
    free(stmt->query);
    stmt->query = NULL;
  }
#ifdef HAVE_PS
  if (stmt->ptr == NULL)
    return 1;

  DEBUG("mysql_stmt_close(%p)", stmt->ptr);
  return mysql_stmt_close(stmt->ptr);
#else
  (void)stmt; /* unused */
  return 0;
#endif
}


/* Uninitialize driver */
int mysql_drv_done(void)
{
  mysql_library_end();
  
  return 0;
}


#ifdef HAVE_PS
/* Map SQL data type to bind_type value in MYSQL_BIND */


int get_mysql_bind_type(db_bind_type_t type)
{
  unsigned int i;

  for (i = 0; db_mysql_bind_map[i].db_type != DB_TYPE_NONE; i++)
    if (db_mysql_bind_map[i].db_type == type)
      return db_mysql_bind_map[i].my_type;

  return -1;
}

#endif /* HAVE_PS */
