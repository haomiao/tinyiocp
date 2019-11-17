#include "pch.h"
#include "iclient.h"
#include <mstcpip.h>
#include <WS2tcpip.h>

#pragma comment(lib, "WS2_32.lib")

IClient::IClient()
	: m_nPort(0)
	, m_completionPort(NULL)
	, m_pWorkerThreads(nullptr)
	, m_workerThreadNum(0)
	, m_pSocketContext(nullptr)
{
	WSADATA wsaData;
	::WSAStartup(MAKEWORD(2, 2), &wsaData);

	m_stopEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

IClient::~IClient()
{
	DisConnect();
	
	if (m_stopEvent)
	{
		::CloseHandle(m_stopEvent);
		m_stopEvent = NULL;
	}

	::WSACleanup();
}

bool IClient::Connect(const std::string & ipAddress, USHORT nPort)
{
	m_ipAddress = ipAddress;
	m_nPort = nPort;

	bool result = Init();
	if (!result)
	{
		UnInit();
	}
	return result;
}

bool IClient::DisConnect()
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

bool IClient::Send(const char *buffer, int nLen)
{
	if (!buffer)
	{
		return false;
	}

	IOOverlappedContext *pNewOverlappedContext = m_pSocketContext->NewIOOverlappedContext();
	pNewOverlappedContext->optType = IOCP_OPERATOR_TYPE::IOCP_OPT_SEND;
	pNewOverlappedContext->ioSocket = m_pSocketContext->connSocket;
	::memcpy_s(pNewOverlappedContext->wsaBuffer.buf, MAX_BUFFER_SIZE, buffer, nLen);
	pNewOverlappedContext->wsaBuffer.len = (ULONG)nLen;

	if (false == PostSend(m_pSocketContext, pNewOverlappedContext))
	{
		DoClose(m_pSocketContext);
		return false;
	}

	return true;
}

bool IClient::Init()
{
	if (m_stopEvent)
	{
		::ResetEvent(m_stopEvent);
	}

	if (!(InitIOCP() && InitConnectSocket()))
	{
		UnInit();
		return false;
	}

	return true;
}

bool IClient::UnInit()
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

	if (m_pSocketContext)
	{
		delete m_pSocketContext;
		m_pSocketContext = nullptr;
	}

	return true;
}

bool IClient::InitIOCP()
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
		m_pWorkerThreads[index] = ::CreateThread(0, 0, &IClient::WorkerThreadProc, (void *)this, 0, 0);
	}
	return true;
}

bool IClient::InitConnectSocket()
{
	// 生成用于通信的socket的Context
	m_pSocketContext = new IOSocketContext();
	m_pSocketContext->connSocket = ::WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pSocketContext->connSocket)
	{
		delete m_pSocketContext;
		m_pSocketContext = nullptr;

		return false;
	}

	// 填充地址信息
	sockaddr_in serverAddr;
	::memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	::inet_pton(AF_INET, m_ipAddress.c_str(), (PVOID)&serverAddr.sin_addr.s_addr);
	serverAddr.sin_port = ::htons(m_nPort);

	if (::connect(
		m_pSocketContext->connSocket,
		(struct sockaddr*)&serverAddr,
		sizeof(sockaddr_in)) == -1)
	{
		::closesocket(m_pSocketContext->connSocket);
		m_pSocketContext->connSocket = INVALID_SOCKET;

		delete m_pSocketContext;
		m_pSocketContext = nullptr;

		return false;
	}

	// 将socket绑定到完成端口中
	if (NULL == ::CreateIoCompletionPort(
		(HANDLE)m_pSocketContext->connSocket, m_completionPort, (ULONG_PTR)m_pSocketContext, 0))
	{
		::closesocket(m_pSocketContext->connSocket);
		m_pSocketContext->connSocket = INVALID_SOCKET;

		delete m_pSocketContext;
		m_pSocketContext = nullptr;

		return false;
	}

	IOOverlappedContext *pOverlappedContext = m_pSocketContext->NewIOOverlappedContext();
	pOverlappedContext->ioSocket = m_pSocketContext->connSocket;

	if (false == PostRecv(m_pSocketContext, pOverlappedContext))
	{
		m_pSocketContext->ReleaseIOOverlappedContext(pOverlappedContext);

		::closesocket(m_pSocketContext->connSocket);
		m_pSocketContext->connSocket = INVALID_SOCKET;

		delete m_pSocketContext;
		m_pSocketContext = nullptr;

		return false;
	}

	return true;
}

DWORD IClient::GetNumOfProcessors()
{
	SYSTEM_INFO si;
	::GetSystemInfo(&si);
	return si.dwNumberOfProcessors;
}

bool IClient::IsSocketAlive(SOCKET sock)
{
	return (::send(sock, "", 0, 0) >= 0);
}

bool IClient::PostRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
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

bool IClient::PostSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
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

bool IClient::DoRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
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

bool IClient::DoSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
{
	OnSend(pSocketContext, pOverlappedContext);
	return true;
}

bool IClient::DoClose(IOSocketContext *pSocketContext)
{
// 	if (pSocketContext)
// 	{
// 		delete pSocketContext;
// 	}

	return true;
}

DWORD WINAPI IClient::WorkerThreadProc(LPVOID lpParam)
{
	IClient *pThis = reinterpret_cast<IClient*>(lpParam);
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
					pThis->OnClosed(pSocketContext);
					pThis->DoClose(pSocketContext);
				}
				continue;
			}
			else if (ERROR_NETNAME_DELETED == dwErr)
			{
				pThis->OnError(pSocketContext, dwErr);
				pThis->DoClose(pSocketContext);
				continue;
			}
			else // others error
			{
				pThis->OnError(pSocketContext, dwErr);
				pThis->DoClose(pSocketContext);
				continue;
			}
		}
		else
		{
			// 若对端断开，则关闭连接
			if ((0 == dwBytes) && 
				(IOCP_OPERATOR_TYPE::IOCP_OPT_RECV == pOverlappedContext->optType ||
				IOCP_OPERATOR_TYPE::IOCP_OPT_SEND == pOverlappedContext->optType))
			{
				pThis->OnClosed(pSocketContext);
				pThis->DoClose(pSocketContext);
				continue;
			}
			else
			{
				switch (pOverlappedContext->optType)
				{
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
