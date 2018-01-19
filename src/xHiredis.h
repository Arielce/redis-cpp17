#pragma once
#include "xSds.h"
#include "xTcpconnection.h"
#include "xObject.h"
#include "xTcpClient.h"
#include "xSocket.h"
#include "xLog.h"
#include "xThreadPool.h"

class xRedisAsyncContext;
typedef void (redisCallbackFn)(const xRedisAsyncContextPtr &ac, void*, void*);
typedef struct redisReply :noncopyable
{
    int type;
    long long integer;
    int len;
    char *str;
    size_t elements;
    struct redisReply **element;
} redisReply;


typedef struct redisReadTask:noncopyable
{
    int type;
    int elements;
    int idx;
    void *obj;
    struct redisReadTask *parent;
    void *privdata;
} redisReadTask;


redisReply * createReplyObject(int type);
void * createString(const redisReadTask * task, const char * str, size_t len);
void * createArray(const redisReadTask * task, int elements);
void * createInteger(const redisReadTask * task, long long value);
void * createNil(const redisReadTask * task);
void freeReply(void *reply);


typedef struct redisReplyObjectFunctions:noncopyable
{
	redisReplyObjectFunctions()
	{
		createStringFuc = createString;
	   	createArrayFuc = createArray;
		createIntegerFuc = createInteger;
		createNilFuc = createNil;
		freeObjectFuc = freeReply;
	}
	
	std::function<void *(const redisReadTask *,const char *,size_t)> createStringFuc;
	std::function<void *(const redisReadTask*, int)> createArrayFuc;
	std::function<void *(const redisReadTask*, long long)> createIntegerFuc;
	std::function<void *(const redisReadTask*)> createNilFuc;
	std::function<void (void*)> freeObjectFuc;
} redisFunc;



class xRedisReader:noncopyable
{
public:
	xRedisReader()
	{
		fn = std::shared_ptr<redisFunc>(new redisFunc());
		pos = 0;
		err = 0;
		errstr[0] = '\0';
		ridx 	= -1;
		buf = &buffer;
	}
	
	int err;
	char errstr[128];
	size_t pos;
	xBuffer  *buf;
	xBuffer buffer;
	redisReadTask rstack[9];
	int ridx;
	void *reply;
	std::shared_ptr<redisFunc> fn;
    void *privdata;
};


typedef struct redisCallback
{
    redisCallbackFn *fn;
    void *privdata;
} redisCallback;

typedef struct redisClusterCallback
{
	redisClusterCallback()
	{
		data = nullptr;
	}
	char * data;
};

typedef std::list<redisCallback> RedisCallbackList;
typedef std::list<redisClusterCallback> RedisClusterCallbackList;

class xRedisContext: noncopyable
{
public:
	xRedisContext():reader(new xRedisReader())
	{
		init();
		sender =  std::shared_ptr<xBuffer>(new xBuffer());
		reader = std::shared_ptr<xRedisReader>(new xRedisReader());
	}

	~xRedisContext()
	{
		if(fd > 0)
		{
			::close(fd);
		}
	}

	void init()
	{
		flags	&= ~REDIS_BLOCK;
		err = 0;
		errstr[0] = '\0';
		fd = 0 ;
	}
	
	int err;
	char errstr[128];
	int fd;
	int flags;
	const char *addr;
	int port;
	xBufferPtr sender;
	xRedisReaderPtr reader;
};

class xRedisAsyncContext: noncopyable
{
public:
	xRedisAsyncContext():c(new (xRedisContext))
	{
		conn = nullptr;
		err = 0;
		errstr = nullptr;
		data = nullptr;
		c->flags &= ~REDIS_CONNECTED;
	}

	~xRedisAsyncContext()
	{
		if(conn != nullptr)
		{
			conn->forceClose();
		}

		for(auto it = clus.begin(); it != clus.end(); ++it)
		{
			if((*it).data)
			{
				zfree((*it).data);
			}

		}
	}

	int err;
	char *errstr;
	void *data;
	xRedisContextPtr c;
	xTcpconnectionPtr conn;
	RedisCallbackList replies;
	RedisClusterCallbackList clus;
	std::mutex hiMutex;

	struct
	{
		RedisCallbackList invalid;
		std::unordered_map<rObj *,redisCallback> channels;
		std::unordered_map<rObj *,redisCallback> patterns;
	}sub;
};


class xHiredis : noncopyable
{
public:
	xHiredis(xEventLoop *loop):
		pool(loop),
		clusterMode(false),
		count(0)
	{

	}
	void clusterReadCallBack(const xTcpconnectionPtr& conn, xBuffer* recvBuf,void *data);
	void clusterAskConnCallBack(const xTcpconnectionPtr& conn,void *data);
	void clusterMoveConnCallBack(const xTcpconnectionPtr& conn,void *data);
	void clusterErrorConnCallBack(void *data);
	void redisReadCallBack(const xTcpconnectionPtr& conn, xBuffer* recvBuf,void *data);

	void eraseTcpMap(int data);
	void eraseRedisMap(int32_t sockfd);

	void insertRedisMap(int sockfd, xRedisAsyncContextPtr ac);
	void insertTcpMap(int data,xTcpClientPtr tc);

	xThreadPool & getPoll() { return pool; }
	void setCount(){ count ++; }
	int getCount(){ return count; }
	std::mutex &getMutex(){ return rtx; }
	
	std::unordered_map<int32_t,xRedisAsyncContextPtr> & getRedisMap() { return redisMaps; }
private:
	
	std::unordered_map<int32_t,xTcpClientPtr> tcpClientMaps;
	std::unordered_map<int32_t,xRedisAsyncContextPtr> redisMaps;
	std::unordered_map<int32_t,redisClusterCallback> clusterMaps;
	
	xThreadPool pool;
	bool clusterMode;
	int count;
	std::mutex rtx;
};



int redisContextConnectTcp(const xRedisContextPtr &c, const char *addr, int port, const struct timeval *timeout);
xRedisContextPtr redisConnectWithTimeout(const char *ip, int port, const struct timeval tv);
xRedisContextPtr redisConnect(const char *ip, int port);

int redisAppendCommand(xRedisContextPtr c, const char *format, ...);

int __redisAsyncCommand(const xRedisAsyncContextPtr &ac,redisCallbackFn *fn, void *privdata, char *cmd, size_t len);
int redisvFormatCommand(char * *target, const char * format, va_list ap);
int redisvAsyncCommand(const xRedisAsyncContextPtr &ac, redisCallbackFn *fn, void *privdata, const char *format, va_list ap);
int redisAsyncCommand(const xRedisAsyncContextPtr &ac, redisCallbackFn *fn, void *privdata, const char *format, ...);

int redisAppendFormattedCommand(xRedisContextPtr c, const char *cmd, size_t len);
int redisFormatCommand(char **target, const char *format, ...);
int redisFormatCommandArgv(char * *target, int argc, const char * *argv, const size_t * argvlen);
int redisAppendCommandArgv(const xRedisContextPtr  & c, int argc, const char * *argv, const size_t * argvlen);
int redisFormatSdsCommandArgv(sds *target, int argc, const char ** argv, const size_t *argvlen);

int redisvAppendCommand(const xRedisContextPtr  & c, const char * format, va_list ap);
int __redisAppendCommand(const xRedisContextPtr & c, const char * cmd, size_t len);


void * redisCommand(const xRedisContextPtr  & c, const char * format, ...);
void * redisvCommand(const xRedisContextPtr  & c, const char * format, va_list ap);
void * redisCommandArgv(const xRedisContextPtr  & c, int argc, const char * *argv, const size_t * argvlen);

int redisBufferRead(const xRedisContextPtr & c);
int redisBufferWrite(const xRedisContextPtr & c, int * done);
int redisGetReply(const xRedisContextPtr & c, void * *reply);
int redisGetReplyFromReader(const xRedisContextPtr & c, void * *reply);
int redisReaderGetReply(const xRedisReaderPtr & r,void * *reply);
void __redisSetError(const xRedisContextPtr & c, int type, const char * str);
