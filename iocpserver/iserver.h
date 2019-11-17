#ifndef _TINY_IOCP_IOCPSERVER_ISERVER_H_
#define _TINY_IOCP_IOCPSERVER_ISERVER_H_

#include <WinSock2.h>
#include <Windows.h>
#include <MSWSock.h>
#include <list>

#define MAX_BUFFER_SIZE  (1024 * 4)	// 完成端口操作的数据缓冲区大小(4K)
#define EXIT_SERVER_CODE (-1)		// 传递给Worker线程的退出信号

//	完成端口投递操作类型
enum class IOCP_OPERATOR_TYPE
{
	IOCP_OPT_NONE = 0,	
	IOCP_OPT_ACCPEPT,	// 接受连接
	IOCP_OPT_SEND,		// 发送数据
	IOCP_OPT_RECV,		// 接受数据
};

//	完成端口OVERLAPPED的重叠结构
struct IOOverlappedContext 
{
	WSAOVERLAPPED wsaOverlapped;	// 重叠结构必须的成员且放置在第一个位置
	SOCKET ioSocket;
	WSABUF wsaBuffer;
	IOCP_OPERATOR_TYPE optType;
	// TODO: 也可以附加其他需要的数据成员

	IOOverlappedContext()
		: ioSocket(NULL)
		, optType(IOCP_OPERATOR_TYPE::IOCP_OPT_NONE)
	{
		::memset(&wsaOverlapped, 0, sizeof(wsaOverlapped));
		MallocWsaBuffer(wsaBuffer);
	}

	~IOOverlappedContext()
	{
		if (wsaBuffer.buf)
		{
			::HeapFree(::GetProcessHeap(), 0, wsaBuffer.buf);
			wsaBuffer.buf = nullptr;
		}
	}

	void ResetBufferAndOptType()
	{
		if (wsaBuffer.buf)
		{
			::memset(wsaBuffer.buf, 0, MAX_BUFFER_SIZE);
		}
		else
		{
			MallocWsaBuffer(wsaBuffer);
		}

		::memset(&wsaOverlapped, 0, sizeof(wsaOverlapped));
		optType = IOCP_OPERATOR_TYPE::IOCP_OPT_NONE;
	}

	void MallocWsaBuffer(WSABUF &wsaBuffer)
	{
		wsaBuffer.buf = (CHAR *)::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_BUFFER_SIZE);
		wsaBuffer.len = MAX_BUFFER_SIZE;
	}
};

// 关键段包装锁
class CriticalSectionLock
{
public:

	CriticalSectionLock()
	{
		::InitializeCriticalSection(&m_csLock);
	}

	~CriticalSectionLock()
	{
		::DeleteCriticalSection(&m_csLock);
	}

	void Lock()
	{
		::EnterCriticalSection(&m_csLock);
	}

	void UnLock()
	{
		::LeaveCriticalSection(&m_csLock);
	}

private:

	CRITICAL_SECTION m_csLock;
};


// 自动锁模板类
template<typename Lock>
class AutoLock
{
public:

	explicit AutoLock(Lock& lock) : m_lock(lock)
	{
		m_lock.Lock();
	}

	~AutoLock()
	{
		m_lock.UnLock();
	}

private:

	Lock& m_lock;
};

// OverlappedContext重叠结构共享池，避免频繁创建/释放IOOverlappedContext的操作
class IOOverlappedContextPool
{
public:

	explicit IOOverlappedContextPool(unsigned int nOverlappedContextNum)
	{
		for (size_t i = 0; i < nOverlappedContextNum; i++)
		{
			IOOverlappedContext *pOverlappedContext = new IOOverlappedContext();
			m_overlappedContextList.push_back(pOverlappedContext);
		}
	}

	~IOOverlappedContextPool()
	{
		while (!m_overlappedContextList.empty())
		{
			std::list<IOOverlappedContext*>::iterator iter = 
				m_overlappedContextList.begin();
			delete (*iter);
			m_overlappedContextList.erase(iter);
		}
	}

public:

	static IOOverlappedContextPool& GetInstance()
	{
		static IOOverlappedContextPool s_overlappedContextPool;
		return s_overlappedContextPool;
	}

	IOOverlappedContext* AllocIOOverlappedContext()
	{
		IOOverlappedContext* pOverlappedContext = nullptr;
		AutoLock<CriticalSectionLock> lock(m_criticalSectionLock);

		if (m_overlappedContextList.size())
		{
			pOverlappedContext = m_overlappedContextList.back();
			m_overlappedContextList.pop_back();
		}
		else
		{
			pOverlappedContext = new IOOverlappedContext();
		}
		return pOverlappedContext;
	}

	void ReleaseIOOverlappedContext(IOOverlappedContext* overlappedContext)
	{
		if (!overlappedContext)
		{
			return;
		}

		AutoLock<CriticalSectionLock> lock(m_criticalSectionLock);
		m_overlappedContextList.push_front(overlappedContext);
	}

private:

	IOOverlappedContextPool() = default;
	IOOverlappedContextPool(const IOOverlappedContextPool&) = delete;
	IOOverlappedContextPool& operator= (const IOOverlappedContextPool&) = delete;

private:

	std::list<IOOverlappedContext*> m_overlappedContextList;
	CriticalSectionLock m_criticalSectionLock;
};


// 每个连接对应的套接字上下文结构对象
class IOSocketContext
{
public:

	SOCKET connSocket;		// 连接的socket
	SOCKADDR_IN clientAddr;	// 连接的客户端地址

public:

	IOSocketContext()
		: connSocket(INVALID_SOCKET)
	{
		::memset(&clientAddr, 0, sizeof(clientAddr));
	}

	~IOSocketContext()
	{
		if (connSocket != INVALID_SOCKET)
		{
			::closesocket(connSocket);
			connSocket = INVALID_SOCKET;
		}

		while (!m_overlappedContextList.empty())
		{
			std::list<IOOverlappedContext*>::iterator iter =
				m_overlappedContextList.begin();
			IOOverlappedContextPool::GetInstance().ReleaseIOOverlappedContext(*iter);
			m_overlappedContextList.erase(iter);
		}
	}

	IOOverlappedContext* NewIOOverlappedContext()
	{
		IOOverlappedContext *pOverlappedContext =
			IOOverlappedContextPool::GetInstance().AllocIOOverlappedContext();
		if (pOverlappedContext)
		{
			AutoLock<CriticalSectionLock> lock(m_criticalSectionLock);
			m_overlappedContextList.push_back(pOverlappedContext);
		}
		return pOverlappedContext;
	}

	void ReleaseIOOverlappedContext(IOOverlappedContext* pOverlappedContext)
	{
		for (auto iter = m_overlappedContextList.begin();
			 iter != m_overlappedContextList.end();
			 ++iter)
		{
			if (*iter == pOverlappedContext)
			{
				IOOverlappedContextPool::GetInstance().ReleaseIOOverlappedContext(*iter);

				AutoLock<CriticalSectionLock> lock(m_criticalSectionLock);
				m_overlappedContextList.erase(iter);
				break;
			}
		}
	}

private:

	// 同一socket上的多个IO重叠请求上下文且管理此些上下文生命周期
	std::list<IOOverlappedContext*> m_overlappedContextList; 
	CriticalSectionLock m_criticalSectionLock;
};

// IOCP完成端口服务端抽象基类
// 子类可实现抽象接口实现自定义业务处理逻辑
class IServer
{
public:

	bool Start(USHORT nPort = 9988, unsigned int nMaxAcceptConn = 10);
	bool Stop();
	bool Send(IOSocketContext *pSocketContext, const char *buffer, int nLen);
	ULONG GetConnectCounts() const;

public:

	// 处理结果回调函数，子类可继承重写此类函数，以实现相应的业务处理逻辑
	virtual void OnEstablished(IOSocketContext *pSocketContext) = 0;
	virtual void OnClosed(IOSocketContext *pSocketContext) = 0;
	virtual void OnError(IOSocketContext *pSocketContext, DWORD dwError) = 0;
	virtual void OnRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext) = 0;
	virtual void OnSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext) = 0;

private:

	bool Init();
	bool UnInit();
	bool InitIOCP();
	bool InitListenSocket();
	DWORD GetNumOfProcessors();
	bool IsSocketAlive(SOCKET sock);

private:

	// 投递IO请求
	bool PostAccept(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool PostRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool PostSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);

	// IO处理函数
	bool DoAccpet(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool DoRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool DoSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool DoClose(IOSocketContext *pSocketContext);

	// 工作中线程函数
	static DWORD WINAPI WorkerThreadProc(LPVOID lpParam);

protected:

	IServer();
	virtual ~IServer();

private:

	USHORT m_nPort;							// 监听端口号
	unsigned int m_nMaxAcceptConn;			// 最大投递Accpet连接数
	HANDLE m_stopEvent;						// 通知工作者线程退出的事件
	HANDLE m_completionPort;				// 完成端口
	HANDLE *m_pWorkerThreads;				// 工作者线程的句柄指针
	unsigned int m_workerThreadNum;			// 工作者线程的数量
	IOSocketContext *m_pListenSocketContext;// 监听socket的Context上下文
	ULONG m_nConnectCounts;					// 当前的连接数量

	LPFN_ACCEPTEX			  m_fnAcceptEx;	// AcceptEx函数指针地址
	LPFN_GETACCEPTEXSOCKADDRS m_fnGetAcceptExSockAddrs; // GetAcceptExSockAddrs函数指针地址
};

#endif	// _TINY_IOCP_IOCPSERVER_ISERVER_H_
