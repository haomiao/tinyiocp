// iocpclient.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "pch.h"
#include <iostream>
#include "iclient.h"

class ConcreteClient : public IClient
{
public:

	ConcreteClient() {}
	~ConcreteClient() {}

public:

	virtual void OnEstablished(IOSocketContext *pSocketContext)
	{
		std::cout << "A connected!" << std::endl;
	}

	virtual void OnClosed(IOSocketContext *pSocketContext)
	{
		std::cout << "A connection has closed!" << std::endl;
	}

	virtual void OnError(IOSocketContext *pSocketContext, DWORD dwError)
	{
		printf("A connection error: %d\n", dwError);
	}

	virtual void OnRecv(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
	{
		printf("Received data: %s\n", pOverlappedContext->wsaBuffer.buf);
	}

	virtual void OnSend(IOSocketContext *pSocketContext, IOOverlappedContext *pOverlappedContext)
	{
		std::cout << "Send data succeeded!" << std::endl;
	}

};

int main()
{
	std::cout << "start client ......." << std::endl;
	ConcreteClient client;
	client.Connect("127.0.0.1", 9988);
	std::string strMsg1 = "Hello Server1!";
	client.Send(strMsg1.c_str(), strMsg1.length());

	::Sleep(1000);

	std::string strMsg2 = "Hello Server2!";
	client.Send(strMsg2.c_str(), strMsg2.length());

	::Sleep(1000);

	client.DisConnect();

	std::cout << "stop server ......" << std::endl;

	return 0;
}
