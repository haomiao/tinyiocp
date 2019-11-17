#include "pch.h"
#include "iserver.h"
#include <mstcpip.h>

#pragma comment(lib, "WS2_32.lib")

IServer::IServer()
	: m_nPort(0)
	, m_nMaxAcceptConn(0)
	, m_completionPort(NULL)
	, m_pWorkerThreads(nullptr)
	, m_workerThreadNum(0)
	, m_pListenSocketContext(nullptr)
	, m_nConnectCounts(0)
	, m_fnAcceptEx(nullptr)
	, m_fnGetAcceptExSockAddrs(nullptr)
{
	WSADATA wsaData;
	::WSAStartup(MAKEWORD(2, 2), &wsaData);

	m_stopEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

IServer::~IServer()
{
	Stop();
	
	if (m_stopEvent)
	{
		::CloseHandle(m_stopEvent);
		m_stopEvent = NULL;
	}

	::WSACleanup();
}

bool IServer::Start(USHORT nPort, unsigned int nMaxAcceptConn)
{
	m_nPort = nPort;
	m_nMaxAcceptConn = nMaxAcceptConn;

	bool result = Init();
	if (!result)
	{
		UnInit();
	}
	return result;
}

bool IServer::Stop()
{
	if (m_stopEvent)
	{
		::SetEvent(m_stopEvent);
	}

	for (unsigned int index = 0; index < m_workerThreadNum; ++index)
	{
		::PostQueuedCompletionStatus(m_completionPort, 0, EXIT_SERVER_CODE, nullptr);
	}

	if (m_pWorkerThreads && m_workerThreadNum)
	{
		::WaitForMultipleObjects(m_workerThreadNum, m_pWorkerThreads, TRUE, INFINITE);
	}

	UnInit();

	return true;
}

bool IServer::Send(IOSocketContext *pSocketContext, const char *buffer, int nLen)
{
	IOOverlappedContext *pNewOverlappedContext = pSocketContext->NewIOOverlappedContext();
	pNewOverlappedContext->optType = IOCP_OPERATOR_TYPE::IOCP_OPT_SEND;
	pNewOverlappedContext->ioSocket = pSocketContext->connSocket;
	::memcpy_s(pNewOverlappedContext->wsaBuffer.buf, MAX_BUFFER_SIZE, buffer, nLen);
	pNewOverlappedContext->wsaBuffer.len = (ULONG)nLen;

	if (false == PostSend(pSocketContext, pNewOverlappedContext))
	{
		DoClose(pSocketContext);
		return false;
	}

	return true;
}

ULONG IServer::GetConnectCounts() const
{
	return m_nConnectCounts;
}

bool IServer::Init()
{
	if (m_stopEvent)
	{
		::ResetEvent(m_stopEvent);
	}

	if (!(InitIOCP() && InitListenSocket()))
	{
		UnInit();
		return false;
	}

	return true;
}

bool IServer::UnInit()
{
	if (m_pWorkerThreads)
	{
		for (unsigned int index = 0; index < m_workerThreadNum; ++index)
		{
			if (m_pWorkerThreads[index] != INVALID_HANDLE_VALUE)
			{
				::CloseHandle(m_pWorkerThreads[index]);
				m_pWorkerThreads[index] = INVALID_HANDLE_VALUE;
			}
		}

		delete []m_pWorkerThreads;
		m_pWorkerThreads = nullptr;
	}

	if (m_completionPort)
	{
		::CloseHandle(m_completionPort);
		m_completionPort = NULL;
	}

	if (m_pListenSocketContext)
	{
		if (m_pListenSocketContext->connSocket != INVALID_SOCKET)
		{
			::closesocket(m_pListenSocketContext->connSocket);
			m_pListenSocketContext->connSocket = INVALID_SOCKET;
		}

		delete m_pListenSocketContext;
		m_pListenSocketContext = nullptr;
	}

	return true;
}

bool IServer::InitIOCP()
{
	// 初始化完成端口及工作线程集
	m_completionPort = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!m_completionPort)
	{
		return false;
	}

	m_workerThreadNum = 2 * GetNumOfProcessors() + 2;
	m_pWorkerThreads = new HANDLE[m_workerThreadNum];

	for (DWORD index = 0; index < m_workerThreadNum; ++index)
	{
		m_pWorkerThreads[index] = ::CreateThread(0, 0, &IServer::WorkerThreadProc, (void *)this, 0, 0);
	}
	return true;
}

bool IServer::InitListenSocket()
{
	// 生成用于监听的socket的Context
	m_pListenSocketContext = new IOSocketContext();
	m_pListenSocketContext->connSocket = ::WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pListenSocketContext->connSocket)
	{
		return false;
	}

	// 将监听socket绑定到完成端口中
	if (NULL == ::CreateIoCompletionPort(
		(HANDLE)m_pListenSocketContext->connSocket, m_completionPort, (ULONG_PTR)m_pListenSocketContext, 0))
	{
		::closesocket(m_pListenSocketContext->connSocket);
		m_pListenSocketContext->connSocket = INVALID_SOCKET;
		return false;
	}

	//服务器地址信息，用于绑定socket
	sockaddr_in serverAddr;
	::memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = ::htonl(INADDR_ANY);
	serverAddr.sin_port = ::htons(m_nPort);

	// 绑定地址和端口
	if (SOCKET_ERROR == ::bind(m_pListenSocketContext->connSocket, (sockaddr *)&serverAddr, sizeof(serverAddr)))
	{
		return false;
	}

	// 开始监听
	if (SOCKET_ERROR == ::listen(m_pListenSocketContext->connSocket, SOMAXCONN))
	{
		return false;
	}

	GUID guidAcceptEx = WSAID_ACCEPTEX;
	GUID guidGetAcceptSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
	DWORD dwBytes = 0;
	if (SOCKET_ERROR == ::WSAIoctl(
		m_pListenSocketContext->connSocket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidAcceptEx,
		sizeof(guidAcceptEx),
		&m_fnAcceptEx,
		sizeof(m_fnAcceptEx),
		&dwBytes,
		NULL,
		NULL))
	{
		UnInit();
		return false;
	}

	if (SOCKET_ERROR == ::WSAIoctl(
		m_pListenSocketContext->connSocket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidGetAcceptSockAddrs,
		sizeof(guidGetAcceptSockAddrs),
		&m_fnGetAcceptExSockAddrs,
		sizeof(m_fnGetAcceptExSockAddrs),
		&dwBytes,
		NULL,
		NULL))
	{
		UnInit();
		return false;
	}

	for (unsigned int index = 0; index < m_nMaxAcceptConn; ++index)
	{
		IOOverlappedContext *pOverlappedContext = m_pListenSocketContext->NewIOOverlappedContext();
		if (false == PostAccept(m_pListenSocketContext, pOverlappedContext))
		{
			m_pListenSocketContext->ReleaseIOOverlappedContext(pOverlappedContext);
			return false;
		}
	}
	return true;
}

DWORD IServer::GetNumOfProcessors()
{
	SYSTEM_INFO si;
	::GetSystemInfo(&si);
	return si.dwNumberOfProcessors;
}

bool IServer::IsSocketAlive(SOCKET sock)
{
	return (::send(sock, "", 0, 0) >= 0);
}

bool IServer::PostAccept(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
{
	DWORD dwBytes = 0;
	pOverlappedContext->optType = IOCP_OPERATOR_TYPE::IOCP_OPT_ACCPEPT;
	pOverlappedContext->ioSocket = ::WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == pOverlappedContext->ioSocket)
	{
		return false;
	}

	// 将接收缓冲置为0,令AcceptEx直接返回，而不是等待接收数据
	if (false == m_fnAcceptEx(
		m_pListenSocketContext->connSocket,
		pOverlappedContext->ioSocket,
		pOverlappedContext->wsaBuffer.buf,
		0,
		sizeof(sockaddr_in) + 16,
		sizeof(sockaddr_in) + 16,
		&dwBytes,
		&pOverlappedContext->wsaOverlapped))
	{
		if (WSA_IO_PENDING != ::WSAGetLastError())
		{
			return false;
		}
	}

	return true;
}

bool IServer::PostRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
{
	DWORD dwFlags = 0, dwBytes = 0;
	pOverlappedContext->ResetBufferAndOptType();
	pOverlappedContext->optType = IOCP_OPERATOR_TYPE::IOCP_OPT_RECV;

	if ((::WSARecv(
		pOverlappedContext->ioSocket,
		&pOverlappedContext->wsaBuffer,
		1,
		&dwBytes,
		&dwFlags,
		&pOverlappedContext->wsaOverlapped,
		NULL
		) == SOCKET_ERROR) && (WSA_IO_PENDING != ::WSAGetLastError()))
	{
		DoClose(pSocketContext);
		return false;
	}

	return true;
}

bool IServer::PostSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
{
	pOverlappedContext->optType = IOCP_OPERATOR_TYPE::IOCP_OPT_SEND;
	DWORD dwBytes = 0;
	DWORD dwFlags = 0;

	if ((::WSASend(
		pOverlappedContext->ioSocket,
		&pOverlappedContext->wsaBuffer,
		1,
		&dwBytes,
		dwFlags,
		&pOverlappedContext->wsaOverlapped,
		NULL
	) != NO_ERROR) && (::WSAGetLastError() != WSA_IO_PENDING))
	{
		DoClose(pSocketContext);
		return false;
	}

	return true;
}

bool IServer::DoAccpet(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
{
	SOCKADDR_IN *pClientAddr = nullptr;
	SOCKADDR_IN *pLocalAddr = nullptr;
	int clientAddrLen = sizeof(SOCKADDR_IN);
	int localAddrLen = clientAddrLen;

	// 获取地址信息
	m_fnGetAcceptExSockAddrs(
		pOverlappedContext->wsaBuffer.buf,
		0,
		localAddrLen,
		clientAddrLen,
		(LPSOCKADDR *)&pLocalAddr,
		&localAddrLen,
		(LPSOCKADDR *)&pClientAddr,
		&clientAddrLen
	);

	// 为新连接建立一个SocketContext 
	IOSocketContext *pNewSockContext = new IOSocketContext();
	pNewSockContext->connSocket = pOverlappedContext->ioSocket;
	memcpy_s(&(pNewSockContext->clientAddr), sizeof(SOCKADDR_IN), pClientAddr, sizeof(SOCKADDR_IN));

	// 将listenSocketContext的IOContext 重置后继续投递AcceptEx
	pOverlappedContext->ResetBufferAndOptType();
	if (false == PostAccept(m_pListenSocketContext, pOverlappedContext))
	{
		m_pListenSocketContext->ReleaseIOOverlappedContext(pOverlappedContext);
	}

	// 将新socket和完成端口绑定
	if (NULL == ::CreateIoCompletionPort(
		(HANDLE)pNewSockContext->connSocket,
		m_completionPort,
		(ULONG_PTR)pNewSockContext,
		0
	))
	{
		if (::WSAGetLastError() != ERROR_INVALID_PARAMETER)
		{
			DoClose(pNewSockContext);
			return false;
		}
	}

	// 设置tcp_keepalive
	tcp_keepalive alive_in;
	tcp_keepalive alive_out;
	alive_in.onoff = TRUE;
	alive_in.keepalivetime = 1000 * 60;
	alive_in.keepaliveinterval = 1000 * 10;
	unsigned long ulBytesReturn = 0;
	if (SOCKET_ERROR == ::WSAIoctl(
		pNewSockContext->connSocket,
		SIO_KEEPALIVE_VALS,
		&alive_in,
		sizeof(alive_in),
		&alive_out,
		sizeof(alive_out),
		&ulBytesReturn,
		nullptr,
		nullptr
	))
	{
		
	}

	OnEstablished(pNewSockContext);

	// 建立recv操作所需的ioContext，在新连接的socket上投递recv请求
	IOOverlappedContext *pNewOverlappedContext = pNewSockContext->NewIOOverlappedContext();
	pNewOverlappedContext->optType = IOCP_OPERATOR_TYPE::IOCP_OPT_RECV;
	pNewOverlappedContext->ioSocket = pNewSockContext->connSocket;

	// 投递recv请求
	if (false == PostRecv(pNewSockContext, pNewOverlappedContext))
	{
		DoClose(pSocketContext);
		return false;
	}

	return true;
}

bool IServer::DoRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
{
	OnRecv(pSocketContext, pOverlappedContext);
	pOverlappedContext->ResetBufferAndOptType();
	if (false == PostRecv(pSocketContext, pOverlappedContext))
	{
		DoClose(pSocketContext);
		return false;
	}

	return true;
}

bool IServer::DoSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
{
	OnSend(pSocketContext, pOverlappedContext);
	return true;
}

bool IServer::DoClose(IOSocketContext *pSocketContext)
{
	if (pSocketContext)
	{
		delete pSocketContext;
	}

	return true;
}

DWORD WINAPI IServer::WorkerThreadProc(LPVOID lpParam)
{
	IServer *pThis = reinterpret_cast<IServer*>(lpParam);
	if (!pThis)
	{
		return 0;
	}

	OVERLAPPED *pOverlapped = nullptr;
	IOSocketContext *pSocketContext = nullptr;
	DWORD dwBytes = 0;
	IOOverlappedContext *pOverlappedContext = nullptr;

	// 采用退出信号及退出事件的双保险方式，以确保退出所有工作者线程
	while (WAIT_OBJECT_0 != ::WaitForSingleObject(pThis->m_stopEvent, 0))
	{
		BOOL bRet = ::GetQueuedCompletionStatus(
			pThis->m_completionPort,
			&dwBytes,
			(PULONG_PTR)&pSocketContext,
			&pOverlapped,
			INFINITE
		);

		if (EXIT_SERVER_CODE == (ULONG_PTR)pSocketContext)
		{
			break;
		}

		// 获取到传入的重叠结构参数IOOverlappedContext
		pOverlappedContext = CONTAINING_RECORD(pOverlapped, IOOverlappedContext, wsaOverlapped);

		if (!bRet)
		{
			DWORD dwErr = ::WSAGetLastError();
			if (WAIT_TIMEOUT == dwErr)
			{
				if (!pThis->IsSocketAlive(pSocketContext->connSocket))
				{
					InterlockedDecrement(&pThis->m_nConnectCounts);

					pThis->OnClosed(pSocketContext);
					pThis->DoClose(pSocketContext);
				}
				continue;
			}
			else if (ERROR_NETNAME_DELETED == dwErr)
			{
				InterlockedDecrement(&pThis->m_nConnectCounts);

				pThis->OnError(pSocketContext, dwErr);
				pThis->DoClose(pSocketContext);
				continue;
			}
			else // others error
			{
				InterlockedDecrement(&pThis->m_nConnectCounts);

				pThis->OnError(pSocketContext, dwErr);
				pThis->DoClose(pSocketContext);
				continue;
			}
		}
		else
		{
			// 若客户端断开，则关闭连接
			if ((0 == dwBytes) && 
				(IOCP_OPERATOR_TYPE::IOCP_OPT_RECV == pOverlappedContext->optType ||
				IOCP_OPERATOR_TYPE::IOCP_OPT_SEND == pOverlappedContext->optType))
			{
				InterlockedDecrement(&pThis->m_nConnectCounts);

				pThis->OnClosed(pSocketContext);
				pThis->DoClose(pSocketContext);
				continue;
			}
			else
			{
				switch (pOverlappedContext->optType)
				{
				case IOCP_OPERATOR_TYPE::IOCP_OPT_ACCPEPT:
				{
					InterlockedIncrement(&pThis->m_nConnectCounts);

					pThis->DoAccpet(pSocketContext, pOverlappedContext);
				}
				break;
				case IOCP_OPERATOR_TYPE::IOCP_OPT_RECV:
				{
					pThis->DoRecv(pSocketContext, pOverlappedContext);
				}
				break;
				case IOCP_OPERATOR_TYPE::IOCP_OPT_SEND:
				{
					pThis->DoSend(pSocketContext, pOverlappedContext);
				}
				break;
				default:
					break;
				}
			}
		}
	}

	return 0;
}
