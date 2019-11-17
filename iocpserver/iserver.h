#ifndef _TINY_IOCP_IOCPSERVER_ISERVER_H_
#define _TINY_IOCP_IOCPSERVER_ISERVER_H_

#include <WinSock2.h>
#include <Windows.h>
#include <MSWSock.h>
#include <list>

#define MAX_BUFFER_SIZE  (1024 * 4)	// ��ɶ˿ڲ��������ݻ�������С(4K)
#define EXIT_SERVER_CODE (-1)		// ���ݸ�Worker�̵߳��˳��ź�

//	��ɶ˿�Ͷ�ݲ�������
enum class IOCP_OPERATOR_TYPE
{
	IOCP_OPT_NONE = 0,	
	IOCP_OPT_ACCPEPT,	// ��������
	IOCP_OPT_SEND,		// ��������
	IOCP_OPT_RECV,		// ��������
};

//	��ɶ˿�OVERLAPPED���ص��ṹ
struct IOOverlappedContext 
{
	WSAOVERLAPPED wsaOverlapped;	// �ص��ṹ����ĳ�Ա�ҷ����ڵ�һ��λ��
	SOCKET ioSocket;
	WSABUF wsaBuffer;
	IOCP_OPERATOR_TYPE optType;
	// TODO: Ҳ���Ը���������Ҫ�����ݳ�Ա

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

// �ؼ��ΰ�װ��
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


// �Զ���ģ����
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

// OverlappedContext�ص��ṹ����أ�����Ƶ������/�ͷ�IOOverlappedContext�Ĳ���
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


// ÿ�����Ӷ�Ӧ���׽��������Ľṹ����
class IOSocketContext
{
public:

	SOCKET connSocket;		// ���ӵ�socket
	SOCKADDR_IN clientAddr;	// ���ӵĿͻ��˵�ַ

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

	// ͬһsocket�ϵĶ��IO�ص������������ҹ����Щ��������������
	std::list<IOOverlappedContext*> m_overlappedContextList; 
	CriticalSectionLock m_criticalSectionLock;
};

// IOCP��ɶ˿ڷ���˳������
// �����ʵ�ֳ���ӿ�ʵ���Զ���ҵ�����߼�
class IServer
{
public:

	bool Start(USHORT nPort = 9988, unsigned int nMaxAcceptConn = 10);
	bool Stop();
	bool Send(IOSocketContext *pSocketContext, const char *buffer, int nLen);
	ULONG GetConnectCounts() const;

public:

	// �������ص�����������ɼ̳���д���ຯ������ʵ����Ӧ��ҵ�����߼�
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

	// Ͷ��IO����
	bool PostAccept(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool PostRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool PostSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);

	// IO������
	bool DoAccpet(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool DoRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool DoSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext);
	bool DoClose(IOSocketContext *pSocketContext);

	// �������̺߳���
	static DWORD WINAPI WorkerThreadProc(LPVOID lpParam);

protected:

	IServer();
	virtual ~IServer();

private:

	USHORT m_nPort;							// �����˿ں�
	unsigned int m_nMaxAcceptConn;			// ���Ͷ��Accpet������
	HANDLE m_stopEvent;						// ֪ͨ�������߳��˳����¼�
	HANDLE m_completionPort;				// ��ɶ˿�
	HANDLE *m_pWorkerThreads;				// �������̵߳ľ��ָ��
	unsigned int m_workerThreadNum;			// �������̵߳�����
	IOSocketContext *m_pListenSocketContext;// ����socket��Context������
	ULONG m_nConnectCounts;					// ��ǰ����������

	LPFN_ACCEPTEX			  m_fnAcceptEx;	// AcceptEx����ָ���ַ
	LPFN_GETACCEPTEXSOCKADDRS m_fnGetAcceptExSockAddrs; // GetAcceptExSockAddrs����ָ���ַ
};

#endif	// _TINY_IOCP_IOCPSERVER_ISERVER_H_
