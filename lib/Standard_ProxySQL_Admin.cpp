#include "proxysql.h"
#include "cpp.h"

#include <search.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <resolv.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "SpookyV2.h"

extern MySQL_Authentication *GloMyAuth;

//#define PANIC(msg)  { perror(msg); return -1; }
#define PANIC(msg)  { perror(msg); exit(EXIT_FAILURE); }

int rc, arg_on=1, arg_off=0;

pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;

#define LINESIZE	2048

//#define ADMIN_SQLITE_TABLE_MYSQL_SERVER_STATUS "CREATE TABLE mysql_server_status ( status INT NOT NULL PRIMARY KEY, status_desc VARCHAR NOT NULL, UNIQUE(status_desc) )"
//#define ADMIN_SQLITE_TABLE_MYSQL_SERVERS "CREATE TABLE mysql_servers ( hostname VARCHAR NOT NULL , port INT NOT NULL DEFAULT 3306 , status INT NOT NULL DEFAULT 0 REFERENCES server_status(status) , PRIMARY KEY(hostname, port) )"
#define ADMIN_SQLITE_TABLE_MYSQL_SERVERS "CREATE TABLE mysql_servers ( hostname VARCHAR NOT NULL , port INT NOT NULL DEFAULT 3306 , status VARCHAR CHECK (status IN ('OFFLINE_HARD', 'OFFLINE_SOFT', 'SHUNNED', 'ONLINE')) NOT NULL DEFAULT 'OFFLINE_HARD', PRIMARY KEY(hostname, port) )"
#define ADMIN_SQLITE_TABLE_MYSQL_HOSTGROUPS "CREATE TABLE mysql_hostgroups ( hostgroup_id INT NOT NULL , description VARCHAR, PRIMARY KEY(hostgroup_id) )"
#define ADMIN_SQLITE_TABLE_MYSQL_HOSTGROUP_ENTRIES "CREATE TABLE mysql_hostgroup_entries ( hostgroup_id INT NOT NULL DEFAULT 0, hostname VARCHAR NOT NULL , port INT NOT NULL DEFAULT 3306, FOREIGN KEY (hostname, port) REFERENCES servers (hostname, port) , FOREIGN KEY (hostgroup_id) REFERENCES mysql_hostgroups (hostgroup_id) , PRIMARY KEY (hostgroup_id, hostname, port) )"
#define ADMIN_SQLITE_TABLE_MYSQL_USERS "CREATE TABLE mysql_users ( username VARCHAR NOT NULL , password VARCHAR , active INT CHECK (active IN (0,1)) NOT NULL DEFAULT 1 , use_ssl INT CHECK (use_ssl IN (0,1)) NOT NULL DEFAULT 0, backend INT CHECK (backend IN (0,1)) NOT NULL DEFAULT 1, frontend INT CHECK (frontend IN (0,1)) NOT NULL DEFAULT 1, PRIMARY KEY (username, backend), UNIQUE (username, frontend))"

#ifdef DEBUG
#define ADMIN_SQLITE_TABLE_DEBUG_LEVELS "CREATE TABLE debug_levels (module VARCHAR NOT NULL PRIMARY KEY, verbosity INT NOT NULL DEFAULT 0)"
#endif /* DEBUG */
__thread l_sfp *__thr_sfp=NULL;


//extern "C" MySQL_Thread * create_MySQL_Thread_func();
//extern "C" void destroy_MySQL_Thread_func();
//create_MySQL_Thread_t * create_MySQL_Thread = NULL;

#define CMD1	1
#define CMD2	2
#define CMD3	3
#define CMD4	4
#define CMD5	5

typedef struct { uint32_t hash; uint32_t key; } t_symstruct;

typedef struct { char * table_name; char * table_def; } table_def_t;


static t_symstruct lookuptable[] = {
    { SpookyHash::Hash32("SHOW",4,0), CMD1 },
    { SpookyHash::Hash32("SET",3,0), CMD2 },
    { SpookyHash::Hash32("FLUSH",5,0), CMD3 },
};

#define NKEYS (sizeof(lookuptable)/sizeof(t_symstruct))


static uint32_t keyfromhash(uint32_t hash) {
	uint32_t i;
	for (i=0; i < NKEYS; i++) {
		//t_symstruct *sym = lookuptable + i*sizeof(t_symstruct);
		t_symstruct *sym = lookuptable + i;
		if (sym->hash==hash) {
			return sym->key;
		}
	}
	return -1;
}



//constexpr uint32_t admin_hash(const char *s) {
//	return (constexpr)SpookyHash::Hash32(s,strlen(s),0);
//};

//SQLite3DB db1;

class Standard_ProxySQL_Admin: public ProxySQL_Admin {
	private:
	volatile int main_shutdown;
	SQLite3DB *admindb;	// in memory
	SQLite3DB *monitordb;	// in memory
	SQLite3DB *configdb; // on disk
//SQLite3DB *db3;

	std::vector<table_def_t *> *tables_defs_admin;
	std::vector<table_def_t *> *tables_defs_monitor;
	std::vector<table_def_t *> *tables_defs_config;


	pthread_t admin_thr;

	int main_poll_nfds;
	struct pollfd *main_poll_fds;
	int *main_callback_func;

	void insert_into_tables_defs(std::vector<table_def_t *> *, const char *table_name, const char *table_def);
	void check_and_build_standard_tables(SQLite3DB *db, std::vector<table_def_t *> *tables_defs);
	//void fill_table__server_status(SQLite3DB *db);

#ifdef DEBUG
	void flush_debug_levels_mem_to_db(SQLite3DB *db, bool replace);
	int flush_debug_levels_db_to_mem(SQLite3DB *db);
#endif /* DEBUG */

	void __insert_or_ignore_maintable_select_disktable();
	void __delete_disktable();
	void __insert_or_replace_disktable_select_maintable();
	void __attach_configdb_to_admindb();

	void __add_active_users(enum cred_username_type usertype);
	void __delete_inactive_users(enum cred_username_type usertype);
	void __refresh_users();
	
	public:
	Standard_ProxySQL_Admin();
	virtual ~Standard_ProxySQL_Admin();
	virtual void print_version();
	virtual bool init();
	virtual void init_users();
	virtual void admin_shutdown();
	bool is_command(std::string);
};

static Standard_ProxySQL_Admin *SPA=NULL;

static void * (*child_func[3]) (void *arg);

typedef struct _main_args {
	int nfds;
	struct pollfd *fds;
	int *callback_func;
	volatile int *shutdown;
} main_args;



void admin_session_handler(MySQL_Session *sess) {
}


void *child_mysql(void *arg) {

	int client = *(int *)arg;
	__thr_sfp=l_mem_init();

	struct pollfd fds[1];
	nfds_t nfds=1;
	int rc;

//	MySQL_Thread *mysql_thr=create_MySQL_Thread_func();
	Standard_MySQL_Thread *mysql_thr=new Standard_MySQL_Thread();
	MySQL_Session *sess=mysql_thr->create_new_session_and_client_data_stream(client);
	sess->admin=true;
	sess->admin_func=admin_session_handler;
	MySQL_Data_Stream *myds=sess->client_myds;

	fds[0].fd=client;
	fds[0].revents=0;	
	fds[0].events=POLLIN|POLLOUT;	

	//sess->myprot_client.generate_pkt_initial_handshake(sess->client_myds,true,NULL,NULL);
	sess->myprot_client.generate_pkt_initial_handshake(true,NULL,NULL);
	
	while (glovars.shutdown==0) {
		if (myds->available_data_out()) {
			fds[0].events=POLLIN|POLLOUT;	
		} else {
			fds[0].events=POLLIN;	
		}
		fds[0].revents=0;	
		rc=poll(fds,nfds,2000);
		if (rc == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				goto __exit_child_mysql;
			}
		}
		myds->revents=fds[0].revents;
		myds->read_from_net();
		myds->read_pkts();
		sess->to_process=1;
		sess->handler();
	}

__exit_child_mysql:
	delete sess;

	l_mem_destroy(__thr_sfp);	
	return NULL;
}

void* child_telnet(void* arg)
{ 
	int bytes_read;
	//int i;
//	struct timeval tv;
	char line[LINESIZE+1];
	int client = *(int *)arg;
	free(arg);
	pthread_mutex_unlock(&sock_mutex);
//	gettimeofday(&tv, NULL);
//	printf("Client %d connected at %d.%d\n", client, (int)tv.tv_sec, (int)tv.tv_usec);
	memset(line,0,LINESIZE+1);
	while ((strncmp(line, "quit", 4) != 0) && glovars.shutdown==0) {
		bytes_read = recv(client, line, LINESIZE, 0);
		  if (bytes_read==-1) { 
			 break;
			 }
		  char *eow = strchr(line, '\n');
			if (eow) *eow=0;
			SPA->is_command(line);
			if (strncmp(line,"shutdown",8)==0) glovars.shutdown=1;
		  if (send(client, line, strlen(line), MSG_NOSIGNAL)==-1) break;
		  if (send(client, "\nOK\n", 4, MSG_NOSIGNAL)==-1) break;
	}
//	gettimeofday(&tv, NULL);
//	printf("Client %d disconnected at %d.%d\n", client, (int)tv.tv_sec, (int)tv.tv_usec);
	shutdown(client,SHUT_RDWR);
	close(client);
	return arg;
}

void* child_telnet_also(void* arg)
{ 
	int bytes_read;
	//int i;
//	struct timeval tv;
	char line[LINESIZE+1];
	int client = *(int *)arg;
	free(arg);
	pthread_mutex_unlock(&sock_mutex);
//	gettimeofday(&tv, NULL);
//	printf("Client %d connected at %d.%d\n", client, (int)tv.tv_sec, (int)tv.tv_usec);
	memset(line,0,LINESIZE+1);
	while ((strncmp(line, "quit", 4) != 0) && glovars.shutdown==0) {
		bytes_read = recv(client, line, LINESIZE, 0);
		  if (bytes_read==-1) { 
			 break;
			 }
		  char *eow = strchr(line, '\n');
			if (eow) *eow=0;
			if (strncmp(line,"shutdown",8)==0) glovars.shutdown=1;
		  if (send(client, line, strlen(line), MSG_NOSIGNAL)==-1) break;
		  if (send(client, "\nNOT OK\n", 8, MSG_NOSIGNAL)==-1) break;
	}
//	gettimeofday(&tv, NULL);
//	printf("Client %d disconnected at %d.%d\n", client, (int)tv.tv_sec, (int)tv.tv_usec);
	shutdown(client,SHUT_RDWR);
	close(client);
	return arg;
}





static void * admin_main_loop(void *arg)
{
	int i;
	//size_t c;
	//int sd;	
	struct sockaddr_in addr;
	size_t mystacksize=256*1024;
	struct pollfd *fds=((struct _main_args *)arg)->fds;
	int nfds=((struct _main_args *)arg)->nfds;
	int *callback_func=((struct _main_args *)arg)->callback_func;
	volatile int *shutdown=((struct _main_args *)arg)->shutdown;
	pthread_attr_t attr; 
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize (&attr, mystacksize);

	while (glovars.shutdown==0 && *shutdown==0)
	{
		int *client;
		int client_t;
		socklen_t addr_size = sizeof(addr);
		pthread_t child;
		size_t stacks;
		rc=poll(fds,nfds,1000);
		if ((rc == -1 && errno == EINTR) || rc==0) {
        // poll() timeout, try again
        continue;
		}
		for (i=0;i<nfds;i++) {
			if (fds[i].revents==POLLIN) {
				client_t = accept(fds[i].fd, (struct sockaddr*)&addr, &addr_size);
//		printf("Connected: %s:%d  sock=%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), client_t);
				pthread_attr_getstacksize (&attr, &stacks);
//		printf("Default stack size = %d\n", stacks);
				pthread_mutex_lock (&sock_mutex);
				client=(int *)malloc(sizeof(int));
				*client= client_t;
				if ( pthread_create(&child, &attr, child_func[callback_func[i]], client) != 0 )
					perror("Thread creation");
			}
			fds[i].revents=0;
		}
	}
	//if (__sync_add_and_fetch(shutdown,0)==0) __sync_add_and_fetch(shutdown,1);
	free(arg);
	return NULL;
}


#define PROXYSQL_ADMIN_VERSION "0.1.0815"

//class Standard_ProxySQL_Admin: public ProxySQL_Admin {
/*
private:
volatile int main_shutdown;
SQLite3DB *admindb;	// in memory
SQLite3DB *monitordb;	// in memory
SQLite3DB *configdb; // on disk
//SQLite3DB *db3;

pthread_t admin_thr;

int main_poll_nfds;
struct pollfd *main_poll_fds;
int *main_callback_func;

public:
*/
Standard_ProxySQL_Admin::Standard_ProxySQL_Admin() {
	int i;

	SPA=this;

	i=sqlite3_config(SQLITE_CONFIG_URI, 1);
	if (i!=SQLITE_OK) {
  	fprintf(stderr,"SQLITE: Error on sqlite3_config(SQLITE_CONFIG_URI,1)\n");
		assert(i==SQLITE_OK);
		exit(EXIT_FAILURE);
	}
};

void Standard_ProxySQL_Admin::print_version() {
  fprintf(stderr,"Standard ProxySQL Admin rev. %s -- %s -- %s\n", PROXYSQL_ADMIN_VERSION, __FILE__, __TIMESTAMP__);
};

bool Standard_ProxySQL_Admin::init() {
	int i;
	size_t mystacksize=256*1024;

	child_func[0]=child_mysql;
	child_func[1]=child_telnet;
	child_func[2]=child_telnet_also;
	main_shutdown=0;
	main_poll_nfds=0;
	main_poll_fds=NULL;
	main_callback_func=NULL;

	main_poll_nfds=10;
	main_callback_func=(int *)malloc(sizeof(int)*main_poll_nfds);
	main_poll_fds=(struct pollfd *)malloc(sizeof(struct pollfd)*main_poll_nfds);
	for (i=0;i<main_poll_nfds-1;i++) {
		main_poll_fds[i].fd=listen_on_port((char *)"127.0.0.1",9900+i, 50);
		main_poll_fds[i].events=POLLIN;
		main_poll_fds[i].revents=0;
		main_callback_func[i]=rand()%2+1;
		//main_callback_func[i]=0;
	}
	main_poll_fds[i].fd=listen_on_port((char *)"127.0.0.1",6032, 50);
	main_poll_fds[i].events=POLLIN;
	main_poll_fds[i].revents=0;
	main_callback_func[i]=0;
	

	pthread_attr_t attr; 
  pthread_attr_init(&attr);
//  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize (&attr, mystacksize);

	admindb=new SQLite3DB();
	admindb->open((char *)"file:mem_admindb?mode=memory&cache=shared", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);
	monitordb=new SQLite3DB();
	monitordb->open((char *)"file:mem_monitordb?mode=memory&cache=shared", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);
	configdb=new SQLite3DB();
	configdb->open((char *)"proxysql.db", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);


	tables_defs_admin=new std::vector<table_def_t *>;
	tables_defs_monitor=new std::vector<table_def_t *>;
	tables_defs_config=new std::vector<table_def_t *>;

//	insert_into_tables_defs(tables_defs_admin,"mysql_server_status", ADMIN_SQLITE_TABLE_MYSQL_SERVER_STATUS);
	insert_into_tables_defs(tables_defs_admin,"mysql_servers", ADMIN_SQLITE_TABLE_MYSQL_SERVERS);
	insert_into_tables_defs(tables_defs_admin,"mysql_hostgroups", ADMIN_SQLITE_TABLE_MYSQL_HOSTGROUPS);
	insert_into_tables_defs(tables_defs_admin,"mysql_hostgroup_entries", ADMIN_SQLITE_TABLE_MYSQL_HOSTGROUP_ENTRIES);
	insert_into_tables_defs(tables_defs_admin,"mysql_users", ADMIN_SQLITE_TABLE_MYSQL_USERS);
#ifdef DEBUG
	insert_into_tables_defs(tables_defs_admin,"debug_levels", ADMIN_SQLITE_TABLE_DEBUG_LEVELS);
#endif /* DEBUG */

//	insert_into_tables_defs(tables_defs_config,"mysql_server_status", ADMIN_SQLITE_TABLE_MYSQL_SERVER_STATUS);
	insert_into_tables_defs(tables_defs_config,"mysql_servers", ADMIN_SQLITE_TABLE_MYSQL_SERVERS);
	insert_into_tables_defs(tables_defs_config,"mysql_hostgroups", ADMIN_SQLITE_TABLE_MYSQL_HOSTGROUPS);
	insert_into_tables_defs(tables_defs_config,"mysql_hostgroup_entries", ADMIN_SQLITE_TABLE_MYSQL_HOSTGROUP_ENTRIES);
	insert_into_tables_defs(tables_defs_config,"mysql_users", ADMIN_SQLITE_TABLE_MYSQL_USERS);
#ifdef DEBUG
	insert_into_tables_defs(tables_defs_config,"debug_levels", ADMIN_SQLITE_TABLE_DEBUG_LEVELS);
#endif /* DEBUG */


	check_and_build_standard_tables(admindb, tables_defs_admin);
	check_and_build_standard_tables(configdb, tables_defs_config);

	__attach_configdb_to_admindb();
#ifdef DEBUG
	flush_debug_levels_mem_to_db(configdb, true);
#endif /* DEBUG */
	__insert_or_ignore_maintable_select_disktable();

	
	//fill_table__server_status(admindb);
	//fill_table__server_status(configdb);

	//__refresh_users();

//	pthread_t admin_thr;
	struct _main_args *arg=(struct _main_args *)malloc(sizeof(struct _main_args));
	arg->nfds=main_poll_nfds;
	arg->fds=main_poll_fds;
	arg->shutdown=&main_shutdown;
	arg->callback_func=main_callback_func;
	if (pthread_create(&admin_thr, &attr, admin_main_loop, (void *)arg) !=0 ) {
		perror("Thread creation");
		exit(EXIT_FAILURE);
	}
	return true;
};


void Standard_ProxySQL_Admin::admin_shutdown() {
	int i;
//	do { usleep(50); } while (main_shutdown==0);
	pthread_join(admin_thr, NULL);
	delete admindb;
	delete monitordb;
	delete configdb;
	sqlite3_shutdown();
	if (main_poll_fds) {
		for (i=0;i<main_poll_nfds;i++) {
			shutdown(main_poll_fds[i].fd,SHUT_RDWR);
			close(main_poll_fds[i].fd);
		}
		free(main_poll_fds);
	}
	if (main_callback_func) {
		free(main_callback_func);
	}
};

Standard_ProxySQL_Admin::~Standard_ProxySQL_Admin() {
	admin_shutdown();
};


bool Standard_ProxySQL_Admin::is_command(std::string s) {
	std::string cps;
	std::size_t found = s.find_first_of("\n\r\t ");
	if (found!=std::string::npos) {
		cps=s.substr(0,found);
	} else {
		cps=s;
	}
	transform(cps.begin(), cps.end(), cps.begin(), toupper);
	uint32 cmd_hash=SpookyHash::Hash32(cps.c_str(),cps.length(),0);
	std::cout<<cps<<"  "<<cmd_hash<<"  "<<std::endl;
	switch (keyfromhash(cmd_hash)) {
		case CMD1:
			std::cout<<"This is a SHOW command"<<std::endl;
			break;
		case CMD2:
			std::cout<<"This is a SET command"<<std::endl;
			break;
		case CMD3:
			std::cout<<"This is a FLUSH command"<<std::endl;
			break;
		default:
			return false;
			break;
	}
	return true;
};

void Standard_ProxySQL_Admin::check_and_build_standard_tables(SQLite3DB *db, std::vector<table_def_t *> *tables_defs) {
//	int i;
	table_def_t *td;
	db->execute("PRAGMA foreign_keys = OFF");
	for (std::vector<table_def_t *>::iterator it=tables_defs->begin(); it!=tables_defs->end(); ++it) {
		td=*it;
		db->check_and_build_table(td->table_name, td->table_def);
	}
/*
	for (i=0;i<sizeof(table_defs)/sizeof(admin_sqlite_table_def_t);i++) {
		admin_sqlite_table_def_t *table_def=table_defs+i;
		proxy_debug(PROXY_DEBUG_SQLITE, 6, "SQLITE: checking definition of table %s against \"%s\"\n" , table_def->table_name , table_def->table_def);
		int match=__admin_sqlite3__check_table_structure(db, table_def->table_name , table_def->table_def);
		if (match==0) {
			proxy_debug(PROXY_DEBUG_SQLITE, 1, "SQLITE: Table %s does not exist or is corrupted. Creating!\n", table_def->table_name);
			__admin_sqlite3__build_table_structure(db, table_def->table_name , table_def->table_def);
		}
	}
	sqlite3_exec_exit_on_failure(db, "PRAGMA foreign_keys = ON");
*/
	db->execute("PRAGMA foreign_keys = ON");
};



void Standard_ProxySQL_Admin::insert_into_tables_defs(std::vector<table_def_t *> *tables_defs, const char *table_name, const char *table_def) {
	table_def_t *td = new table_def_t;
	td->table_name=strdup(table_name);
	td->table_def=strdup(table_def);
	tables_defs->push_back(td);
};

/*
// Function outdate because mysql_server_status is being removed
void Standard_ProxySQL_Admin::fill_table__server_status(SQLite3DB *db) {
	db->execute("PRAGMA foreign_keys = OFF");
  db->execute("DELETE FROM mysql_server_status");
  db->execute("INSERT INTO mysql_server_status VALUES (0, \"OFFLINE_HARD\")");
  db->execute("INSERT INTO mysql_server_status VALUES (1, \"OFFLINE_SOFT\")");
	db->execute("INSERT INTO mysql_server_status VALUES (2, \"SHUNNED\")");
	db->execute("INSERT INTO mysql_server_status VALUES (3, \"ONLINE\")");
	db->execute("PRAGMA foreign_keys = ON");
}
*/

#ifdef DEBUG
void Standard_ProxySQL_Admin::flush_debug_levels_mem_to_db(SQLite3DB *db, bool replace) {
	int i;
	char *a=NULL;
	db->execute("DELETE FROM debug_levels WHERE verbosity=0");
  if (replace) {
    a=(char *)"REPLACE INTO debug_levels(module,verbosity) VALUES(\"%s\",%d)";
  } else {
    a=(char *)"INSERT OR IGNORE INTO debug_levels(module,verbosity) VALUES(\"%s\",%d)";
  }
  int l=strlen(a)+100;
  for (i=0;i<PROXY_DEBUG_UNKNOWN;i++) {
    char *query=(char *)malloc(l);
    sprintf(query, a, GloVars.global.gdbg_lvl[i].name, GloVars.global.gdbg_lvl[i].verbosity);
    //proxy_debug(PROXY_DEBUG_SQLITE, 3, "SQLITE: %s\n",buff);
    db->execute(query);
    free(query);
  }
}
#endif /* DEBUG */

#ifdef DEBUG
int Standard_ProxySQL_Admin::flush_debug_levels_db_to_mem(SQLite3DB *db) {
  int i;
  char *query="SELECT verbosity FROM debug_levels WHERE module=\"%s\"";
  int l=strlen(query)+100;
  int rownum=0;
  int result;
	sqlite3 *_db=db->get_db();
  for (i=0;i<PROXY_DEBUG_UNKNOWN;i++) {
    sqlite3_stmt *statement;
    char *buff=(char *)malloc(l);
    sprintf(buff,query,GloVars.global.gdbg_lvl[i].name);
    if(sqlite3_prepare_v2(_db, buff, -1, &statement, 0) != SQLITE_OK) {
      proxy_debug(PROXY_DEBUG_SQLITE, 1, "SQLITE: Error on sqlite3_prepare_v2() running query \"%s\" : %s\n", buff, sqlite3_errmsg(_db));
      sqlite3_finalize(statement);
      free(buff);
      return 0;
    }
    while ((result=sqlite3_step(statement))==SQLITE_ROW) {
      GloVars.global.gdbg_lvl[i].verbosity=sqlite3_column_int(statement,0);
      rownum++;
    }
    sqlite3_finalize(statement);
    free(buff);
  }
  return rownum;
}
#endif /* DEBUG */


void Standard_ProxySQL_Admin::__insert_or_ignore_maintable_select_disktable() {
  admindb->execute("PRAGMA foreign_keys = OFF");
  admindb->execute("INSERT OR IGNORE INTO main.mysql_servers SELECT * FROM disk.mysql_servers");
  admindb->execute("INSERT OR IGNORE INTO main.mysql_hostgroups SELECT * FROM disk.mysql_hostgroups");
//  admindb->execute("INSERT OR IGNORE INTO main.query_rules SELECT * FROM disk.query_rules");
  admindb->execute("INSERT OR IGNORE INTO main.mysql_users SELECT * FROM disk.mysql_users");
//  admindb->execute("INSERT OR IGNORE INTO main.default_hostgroups SELECT * FROM disk.default_hostgroups");
#ifdef DEBUG
  admindb->execute("INSERT OR IGNORE INTO main.debug_levels SELECT * FROM disk.debug_levels");
#endif /* DEBUG */
  admindb->execute("PRAGMA foreign_keys = ON");
}

void Standard_ProxySQL_Admin::__delete_disktable() {
  admindb->execute("DELETE FROM disk.mysql_servers");
  admindb->execute("DELETE FROM disk.mysql_hostgroups");
//  admindb->execute("DELETE FROM disk.query_rules");
  admindb->execute("DELETE FROM disk.mysql_users");
//  admindb->execute("DELETE FROM disk.default_hostgroups");
#ifdef DEBUG
  admindb->execute("DELETE FROM disk.debug_levels");
#endif /* DEBUG */
}

void Standard_ProxySQL_Admin::__insert_or_replace_disktable_select_maintable() {
  admindb->execute("INSERT OR REPLACE INTO disk.mysql_servers SELECT * FROM main.mysql_servers");
  admindb->execute("INSERT OR REPLACE INTO disk.mysql_hostgroups SELECT * FROM main.mysql_hostgroups");
//  admindb->execute("INSERT OR REPLACE INTO disk.query_rules SELECT * FROM main.query_rules");
  admindb->execute("INSERT OR REPLACE INTO disk.mysql_users SELECT * FROM main.mysql_users");
//  admindb->execute("INSERT OR REPLACE INTO disk.default_hostgroups SELECT * FROM main.default_hostgroups");
#ifdef DEBUG
  admindb->execute("INSERT OR REPLACE INTO disk.debug_levels SELECT * FROM main.debug_levels");
#endif /* DEBUG */
}

void Standard_ProxySQL_Admin::__attach_configdb_to_admindb() {
	const char *a="ATTACH DATABASE '%s' AS disk";
	int l=strlen(a)+strlen(configdb->get_url())+5;
	char *cmd=(char *)malloc(l);
	sprintf(cmd,a,configdb->get_url());
	admindb->execute(cmd);
	free(cmd);
}


void Standard_ProxySQL_Admin::init_users() {
	__refresh_users();
}

void Standard_ProxySQL_Admin::__refresh_users() {
	__delete_inactive_users(USERNAME_BACKEND);
	__delete_inactive_users(USERNAME_FRONTEND);
	__add_active_users(USERNAME_BACKEND);
	__add_active_users(USERNAME_FRONTEND);
}

void Standard_ProxySQL_Admin::__delete_inactive_users(enum cred_username_type usertype) {
	char *error=NULL;
	int cols=0;
	int affected_rows=0;
	SQLite3_result *resultset=NULL;
	char *str=(char *)"SELECT username FROM main.mysql_users WHERE %s=1 AND active=0";
	char *query=(char *)malloc(strlen(str)+15);
	sprintf(query,str,(usertype==USERNAME_BACKEND ? "backend" : "frontend"));
	fprintf(stderr,"%s\n", query);	
admindb->execute_statement(query, &error , &cols , &affected_rows , &resultset);
	if (error) {
		proxy_error("Error on %s : %s\n", query, error);
	} else {
		for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
      SQLite3_row *r=*it;
			GloMyAuth->del(r->fields[0], usertype);
		}
	}
//	if (error) free(error);
	if (resultset) delete resultset;
	free(query);
}

void Standard_ProxySQL_Admin::__add_active_users(enum cred_username_type usertype) {
	char *error=NULL;
	int cols=0;
	int affected_rows=0;
	SQLite3_result *resultset=NULL;
	char *str=(char *)"SELECT username,password,use_ssl FROM main.mysql_users WHERE %s=1 AND active=1";
	char *query=(char *)malloc(strlen(str)+15);
	sprintf(query,str,(usertype==USERNAME_BACKEND ? "backend" : "frontend"));
	fprintf(stderr,"%s\n", query);	
admindb->execute_statement(query, &error , &cols , &affected_rows , &resultset);
	if (error) {
		proxy_error("Error on %s : %s\n", query, error);
	} else {
		for (std::vector<SQLite3_row *>::iterator it = resultset->rows.begin() ; it != resultset->rows.end(); ++it) {
      SQLite3_row *r=*it;
			GloMyAuth->add(r->fields[0], r->fields[1], usertype, (strcmp(r->fields[2],"1")==0 ? true : false) );
		}
	}
//	if (error) free(error);
	if (resultset) delete resultset;
	free(query);
}


extern "C" ProxySQL_Admin * create_ProxySQL_Admin_func() {
	return new Standard_ProxySQL_Admin();
}

extern "C" void destroy_Admin(ProxySQL_Admin * pa) {
	delete pa;
}