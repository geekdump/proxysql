/*

MySQL_Backend module

Classes:
MySQL_Server
MySQL_Hostgroup_Entry
MySQL_Hostgroup
MySQL_HostGroups


MySQL_HostGroups is a collections of MySQL_Hostgroup and MySQL_Server
MySQL_Hostgroup is a collections of MySQL_Hostgroup_Entry
MySQL_Hostgroup_Entry is a reference to MySQL_Server with further data


In other words:
- MySQL_Hostgroup_Entry links a MySQL_Server to a MySQL_Hostgroup
- MySQL_Hostgroups is a high-level class for the management of several MySQL_Hostgroup and several MySQL_Server

Classes MySQL_Server and MySQL_Hostgroup_Entry have very few functions, mainly constructor and deconstructor






MySQL_Hostgroup has the following private functions:
void add(MySQL_Hostgroup_Entry *)
		adds a MySQL_Hostgroup_Entry
void add(MySQL_Server *)
		creates a new MySQL_Hostgroup_Entry from a given MySQL_Server and adds it
void del(MySQL_Hostgroup_Entry *mshge)
		if exists, delete a given MySQL_Hostgroup_Entry
bool del(MySQL_Server *msptr)
		if exists, delete the MySQL_Hostgroup_Entry pointing to the given MySQL_Server

MySQL_Hostgroup has the following public functions:
MySQL_Hostgroup_Entry * MSHGE_find(MySQL_Server *)
		finds a MySQL_Hostgroup_Entry from a given MySQL_Server
MySQL_Hostgroup_Entry * server_add(MySQL_Server *)
		if the server is not already in the Hostgroup,
		creates a new MySQL_Hostgroup_Entry from a given MySQL_Server and adds it
size_t servers_in_hostgroup()
		returns the number of MySQL_Hostgroup_Entry
MySQL_Hostgroup_Entry * set_HG_entry_status(MySQL_Server *, enum proxysql_server_status)
		If a MySQL_Hostgroup_Entry exists using the given MySQL_Server, set the server_status and returns it




MySQL_HostGroups has the following private functions:
void reset()
		deletes all hostgroups and servers. This is called by the destructor
void insert_hostgroup(MySQL_Hostgroup *)
		adds a new hostgroup
void create_hostgroup(unsigned int)
		creates a new hostgroup and calls insert_hostgroup to add it
MySQL_Server * server_add(MySQL_Server *)
		addes and returns given MySQL_Server

MySQL_HostGroups has the following public functions:
size_t servers_in_hostgroup(int)
		returns the number of MySQL_Hostgroup_Entry from a given hostgroup id
MySQL_Server * server_find(char *, uint16_t)
		if exists, returns the MySQL_Server with the given address/port
MySQL_Server * server_add(char *add, uint16_t)
		if not exists, creates and add a new MySQL_Server with the given address/port
		returns the existing or new MySQL_Server
MySQL_Hostgroup_Entry * server_add_hg(unsigned int, char *add, uint16_t)
		adds a new server if doesn't exist, and then adds it the correct hostgroup
MySQL_Hostgroup_Entry * MSHGE_find(unsigned int hid, MySQL_Server *srv)
		finds and returns a MySQL_Hostgroup_Entry from a given MySQL_Server in the specified hostgroup id
MySQL_Hostgroup_Entry * MSHGE_find(unsigned int, char *, uint16_t)
		finds and returns a MySQL_Hostgroup_Entry from a given address/port in the specified hostgroup id




*/

#ifndef __CLASS_MYSQL_BACKEND_H
#define __CLASS_MYSQL_BACKEND_H
#include "proxysql.h"
#include "cpp.h"

class MySQL_Server {
	public:
	char *address;
	uint16_t port;
	uint16_t flags;
	unsigned int hostgroup_references;
	unsigned int connections;
	unsigned char alive;
	MySQL_Server(char *add=NULL, uint16_t p=3306) {
		address=( add ? strdup(add) : NULL);
		port=p;
		flags=0;
		hostgroup_references=0;
		connections=0;
		alive=0;
	};
	void set_addr(char *add) {
		if (address) free(address);
		address=( add ? strdup(add) : NULL);
	};
	~MySQL_Server() {
		if (address) free(address);
	};
};

class MySQL_Hostgroup_Entry {
	public:
	unsigned int hostgroup_id;
	MySQL_Server *MSptr;
	unsigned int weight;
	unsigned int connections_created;
	unsigned int connections_active;
	unsigned int references;
	enum proxysql_server_status status;
	unsigned long long status_change_time;
	MySQL_Hostgroup_Entry(unsigned int hid, MySQL_Server *msptr) {
		hostgroup_id=hid;
		MSptr=msptr;
		weight=0;
		connections_created=0;
		connections_active=0;
		references=0;
		MSptr->hostgroup_references++;
		status=PROXYSQL_SERVER_STATUS_OFFLINE_HARD;
	};
	~MySQL_Hostgroup_Entry() {
		MSptr->hostgroup_references--;
	};
	enum proxysql_server_status set_status(enum proxysql_server_status _s) {
		if (status != _s) {
			status=_s;
			status_change_time=monotonic_time();
		};
		return status;
	};
};

class MySQL_Hostgroup {
	private:
	void add(MySQL_Hostgroup_Entry *mshge) {
		MSHGEs.push_back(mshge);
	};
	void add(MySQL_Server *msptr) {
		MySQL_Hostgroup_Entry *mshge=new MySQL_Hostgroup_Entry(hostgroup_id, msptr);
		this->add(mshge);
	};
	bool del(MySQL_Hostgroup_Entry *mshge) {
		std::vector<MySQL_Hostgroup_Entry *>::iterator it = find(MSHGEs.begin(), MSHGEs.end(), mshge);
		if (it != MSHGEs.end()) {
			MSHGEs.erase(it);
			delete mshge;
			return true;
		}
		return false;
	};
	bool del(MySQL_Server *msptr) {
		for (std::vector<MySQL_Hostgroup_Entry *>::iterator it = MSHGEs.begin() ; it != MSHGEs.end(); ++it) {
			MySQL_Hostgroup_Entry *mshge=*it;
			if (mshge->MSptr==msptr) {
				MSHGEs.erase(it);
				delete mshge;
				return true;
			}
		}
		return false;
	};
	public:
	unsigned int hostgroup_id;
	std::vector<MySQL_Hostgroup_Entry *> MSHGEs;
	MySQL_Hostgroup(unsigned int hid) {
		hostgroup_id=hid;
	};
	~MySQL_Hostgroup() {
		for (std::vector<MySQL_Hostgroup_Entry *>::iterator it = MSHGEs.begin() ; it != MSHGEs.end(); ++it) {
			MySQL_Hostgroup_Entry *mshge=*it;
			delete mshge;
		};
	};
	MySQL_Hostgroup_Entry * MSHGE_find(MySQL_Server *msptr) {
		for (std::vector<MySQL_Hostgroup_Entry *>::iterator it = MSHGEs.begin(); it != MSHGEs.end(); ++it) {
			MySQL_Hostgroup_Entry *mshge=*it;
			if (mshge->MSptr==msptr) {
				return mshge;
			};
		}
		return NULL;
	};
	MySQL_Hostgroup_Entry * server_add(MySQL_Server *msptr) {
		MySQL_Hostgroup_Entry *mshge=NULL;
		mshge=MSHGE_find(msptr);
		if (mshge==NULL) {
			MySQL_Hostgroup_Entry *mshge=new MySQL_Hostgroup_Entry(hostgroup_id, msptr);
			this->add(mshge);
		}
		return mshge;
	};
	size_t servers_in_hostgroup() {
		return MSHGEs.size();
	};
	void set_HG_entry_status(MySQL_Hostgroup_Entry *mshge, enum proxysql_server_status _status) {
		mshge->set_status(_status);
	};
	MySQL_Hostgroup_Entry * set_HG_entry_status(MySQL_Server *msptr, enum proxysql_server_status _status) {
		MySQL_Hostgroup_Entry *mshge=NULL;
		mshge=MSHGE_find(msptr);
		if (mshge) {
			set_HG_entry_status(mshge,_status);
			//mshge->status=_status;
		}
		return mshge;
	};
};

class MySQL_HostGroups {
	private:
	pthread_rwlock_t rwlock;
	std::vector<MySQL_Hostgroup *> MyHostGroups;
	std::vector<MySQL_Server *> Servers;
	void reset() {
		for (std::vector<MySQL_Hostgroup *>::iterator it = MyHostGroups.begin(); it != MyHostGroups.end(); ++it) {
			MySQL_Hostgroup *myhg=*it;
			delete myhg;
		}
		for (std::vector<MySQL_Server *>::iterator it = Servers.begin(); it != Servers.end(); ++it) {
			MySQL_Server *srv=*it;
			delete srv;
		}
	};
	void insert_hostgroup(MySQL_Hostgroup *myhg) {
		unsigned int p=myhg->hostgroup_id;
		MyHostGroups.insert(MyHostGroups.begin()+p,myhg);
	};
	void create_hostgroup(unsigned int hid) {
			MySQL_Hostgroup *myhg=new MySQL_Hostgroup(hid);	
			insert_hostgroup(myhg);
	};
	MySQL_Server * server_add(MySQL_Server *srv) {
		Servers.push_back(srv);
		return srv;
	};
	public:
	MySQL_HostGroups() {
		int rc=pthread_rwlock_init(&rwlock, NULL);
		assert(rc==0);
	};
	~MySQL_HostGroups() {
		reset();
	};
	void wrlock() {
		int rc=pthread_rwlock_wrlock(&rwlock);
		assert(rc==0);
	};
	void rdlock() {
		int rc=pthread_rwlock_rdlock(&rwlock);
		assert(rc==0);
	};
	void wrunlock() {
		int rc=pthread_rwlock_unlock(&rwlock);
		assert(rc==0);
	};
	void rdunlock() {
		int rc=pthread_rwlock_unlock(&rwlock);
		assert(rc==0);
	};
	size_t servers_in_hostgroup(int hid) {
		return MyHostGroups[hid]->servers_in_hostgroup();
	};
	MySQL_Server * server_find(char *add, uint16_t p) {
		for (std::vector<MySQL_Server *>::iterator it = Servers.begin(); it != Servers.end(); ++it) {
			MySQL_Server *srv=*it;
			if (strcmp(srv->address,add)==0 && srv->port==p) {
				return srv;
			};
		}
		return NULL;
	};
	MySQL_Server * server_add(char *add=NULL, uint16_t p=3306) {
		MySQL_Server *srv=NULL;
		srv=server_find(add,p);
		if (srv==NULL) {
			srv=new MySQL_Server(add,p);
			server_add(srv);
		}
		return srv;
	};
	MySQL_Hostgroup_Entry * server_add_hg(unsigned int hid, char *add=NULL, uint16_t p=3306) {
		if (hid>=MyHostGroups.size()) {
			create_hostgroup(hid);
		};
		MySQL_Server *srv=server_add(add,p);
		return MyHostGroups[hid]->server_add(srv);
	};
	MySQL_Hostgroup_Entry * MSHGE_find(unsigned int hid, MySQL_Server *srv) {
		return MyHostGroups[hid]->MSHGE_find(srv);
	};
	MySQL_Hostgroup_Entry * MSHGE_find(unsigned int hid, char *add, uint16_t p) {
		MySQL_Server *srv=server_find(add,p);
		if (srv==NULL) return NULL;	// server not found
		return MSHGE_find(hid,srv);
	};
	MySQL_Hostgroup_Entry * set_HG_entry_status(unsigned int hid, MySQL_Server *msptr, enum proxysql_server_status _status) {
		return MyHostGroups[hid]->set_HG_entry_status(msptr,_status);
	};
	MySQL_Hostgroup_Entry * set_HG_entry_status(unsigned int hid, char *add, uint16_t p, enum proxysql_server_status _status) {
		MySQL_Server *msptr=server_find(add,p);
		if (msptr==NULL) return NULL;	// server not found
		return MyHostGroups[hid]->set_HG_entry_status(msptr,_status);
	};
};



class MySQL_Backend
{
	public:
	void * operator new(size_t);
	void operator delete(void *);
	int hostgroup_id;
	MySQL_Data_Stream *server_myds;
//  mysql_cp_entry_t *server_mycpe;
  bytes_stats_t server_bytes_at_cmd;
	MySQL_Connection *myconn;
	MySQL_Backend();
	~MySQL_Backend();
	void reset();
};

#endif /* __CLASS_MYSQL_BACKEND_H */