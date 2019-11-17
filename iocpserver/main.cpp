// iocpserver.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "pch.h"
#include <iostream>
#include "iserver.h"

class ConcreteServer : public IServer
{
public:

	ConcreteServer() {}
	~ConcreteServer() {}

public:

	virtual void OnEstablished(IOSocketContext *pSocketContext)
	{
		printf("Accept a connection,current connects: %d\n",GetConnectCounts());
	}

	virtual void OnClosed(IOSocketContext *pSocketContext)
	{
		printf("A connection has closed,current connects: %d\n", GetConnectCounts());
	}

	virtual void OnError(IOSocketContext *pSocketContext, DWORD dwError)
	{
		printf("A connection error: %d,current connects: %d\n", dwError, GetConnectCounts());
	}

	virtual void OnRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
	{
		printf("Received data: %s\n", pOverlappedContext->wsaBuffer.buf);

		// Echo
		Send(pSocketContext, pOverlappedContext->wsaBuffer.buf, pOverlappedContext->wsaBuffer.len);
	}

	virtual void OnSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
	{
		printf("Send data succeeded!\n");
	}

};

int main()
{
    std::cout << "start server ......." << std::endl;
	ConcreteServer server;
	server.Start();

	HANDLE hEvent = ::CreateEvent(nullptr, FALSE, FALSE, L"ShutdownEvent");
	::WaitForSingleObject(hEvent, INFINITE);
	::CloseHandle(hEvent);

	server.Stop();

	std::cout << "stop server ......" << std::endl;

	return 0;
}
