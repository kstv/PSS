#include "ProConnectHandle.h"

CProConnectHandle::CProConnectHandle(void)
{
	m_szError[0]          = '\0';
	m_u4ConnectID         = 0;
	m_u4AllRecvCount      = 0;
	m_u4AllSendCount      = 0;
	m_u4AllRecvSize       = 0;
	m_u4AllSendSize       = 0;
	m_nIOCount            = 0;
	m_u4HandlerID         = 0;
	m_u2MaxConnectTime    = 0;
	m_u4SendThresHold     = MAX_MSG_SNEDTHRESHOLD;
	m_u2SendQueueMax      = MAX_MSG_SENDPACKET;
	m_u1ConnectState      = CONNECT_INIT;
	m_u1SendBuffState     = CONNECT_SENDNON;
	m_pPacketParse        = NULL;
	m_u4MaxPacketSize     = MAX_MSG_PACKETLENGTH;
	m_u8RecvQueueTimeCost = 0;
	m_u4RecvQueueCount    = 0;
	m_u8SendQueueTimeCost = 0;
	m_u4ReadSendSize      = 0;
	m_u4SuccessSendSize   = 0;
	m_u1IsActive          = 0;
	m_pBlockMessage       = NULL;
	m_u2SendQueueTimeout  = MAX_QUEUE_TIMEOUT * 1000;  //目前因为记录的是微秒，所以这里相应的扩大1000倍
	m_u2RecvQueueTimeout  = MAX_QUEUE_TIMEOUT * 1000;  //目前因为记录的是微秒，所以这里相应的扩大1000倍
	m_u2TcpNodelay        = TCP_NODELAY_ON;
	m_emStatus            = CLIENT_CLOSE_NOTHING;
	m_u4SendMaxBuffSize   = 5*1024;
	m_nHashID             = 0;
}

CProConnectHandle::~CProConnectHandle(void)
{
}

void CProConnectHandle::Init(uint16 u2HandlerID)
{
	m_u4HandlerID      = u2HandlerID;
	m_u2MaxConnectTime = App_MainConfig::instance()->GetMaxConnectTime();
	m_u4SendThresHold  = App_MainConfig::instance()->GetSendTimeout();
	m_u2SendQueueMax   = App_MainConfig::instance()->GetSendQueueMax();
	m_u4MaxPacketSize  = App_MainConfig::instance()->GetRecvBuffSize();
	m_u2TcpNodelay     = App_MainConfig::instance()->GetTcpNodelay();

	m_u2SendQueueTimeout = App_MainConfig::instance()->GetSendQueueTimeout() * 1000;
	if(m_u2SendQueueTimeout == 0)
	{
		m_u2SendQueueTimeout = MAX_QUEUE_TIMEOUT * 1000;
	}

	m_u2RecvQueueTimeout = App_MainConfig::instance()->GetRecvQueueTimeout() * 1000;
	if(m_u2RecvQueueTimeout <= 0)
	{
		m_u2RecvQueueTimeout = MAX_QUEUE_TIMEOUT * 1000;
	}

	m_u4SendMaxBuffSize  = App_MainConfig::instance()->GetBlockSize();
	m_emStatus           = CLIENT_CLOSE_NOTHING;
}

uint32 CProConnectHandle::GetHandlerID()
{
	return m_u4HandlerID;
}

const char* CProConnectHandle::GetError()
{
	return m_szError;
}

bool CProConnectHandle::Close(int nIOCount, int nErrno)
{
	m_ThreadWriteLock.acquire();
	if(nIOCount > m_nIOCount)
	{
		m_nIOCount = 0;
	}

	//OUR_DEBUG((LM_DEBUG, "[CProConnectHandle::Close]ConnectID=%d, m_nIOCount = %d, nIOCount = %d.\n", GetConnectID(), m_nIOCount, nIOCount));

	if(m_nIOCount > 0)
	{
		m_nIOCount -= nIOCount;
	}

	if(m_nIOCount == 0)
	{
		m_u1IsActive = 0;
	}
	m_ThreadWriteLock.release();

	if(m_nIOCount == 0)
	{
		m_ThreadWriteLock.acquire();
		//查看是否是IP追踪信息，是则记录
		//App_IPAccount::instance()->CloseIP((string)m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), m_u4AllRecvSize, m_u4AllSendSize);

		//调用连接断开消息，通知PacketParse接口
		App_PacketParseLoader::instance()->GetPacketParseInfo()->DisConnect(GetConnectID());

		//通知逻辑接口，连接已经断开
		OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::Close]Connectid=[%d] error(%d)...\n", GetConnectID(), nErrno));
		AppLogManager::instance()->WriteLog(LOG_SYSTEM_CONNECT, "Close Connection from [%s:%d] RecvSize = %d, RecvCount = %d, SendSize = %d, SendCount = %d, RecvQueueCount=%d, RecvQueueTimeCost=%I64dns, SendQueueTimeCost=%I64dns.", 
			m_addrRemote.get_host_addr(), 
			m_addrRemote.get_port_number(), 
			m_u4AllRecvSize, 
			m_u4AllRecvCount, 
			m_u4AllSendSize, 
			m_u4AllSendCount, 
			m_u4RecvQueueCount, 
			m_u8RecvQueueTimeCost, 
			m_u8SendQueueTimeCost);

		//组织数据
		_MakePacket objMakePacket;

		objMakePacket.m_u4ConnectID       = GetConnectID();
		objMakePacket.m_pPacketParse      = NULL;

		//发送客户端链接断开消息。
		ACE_Time_Value tvNow = ACE_OS::gettimeofday();
		if(false == App_MakePacket::instance()->PutMessageBlock(GetConnectID(), PACKET_CDISCONNECT, &objMakePacket, tvNow))
		{
			OUR_DEBUG((LM_ERROR, "[CProConnectHandle::Close] ConnectID = %d, PACKET_CONNECT is error.\n", GetConnectID()));
		}

		m_Reader.cancel();
		m_Writer.cancel();

		ACE_OS::shutdown(this->handle(), SD_BOTH);

		if(this->handle() != ACE_INVALID_HANDLE)
		{
			ACE_OS::closesocket(this->handle());
			this->handle(ACE_INVALID_HANDLE);
		}

		m_ThreadWriteLock.release();

		OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::Close] Close(%d) delete OK.\n", GetConnectID()));

		//删除存在列表中的对象引用
		App_ProConnectManager::instance()->Close(GetConnectID());

		//将对象指针放入空池中
		App_ProConnectHandlerPool::instance()->Delete(this);

		return true;
	}

	return false;
}

bool CProConnectHandle::ServerClose(EM_Client_Close_status emStatus)
{
	if(CLIENT_CLOSE_IMMEDIATLY == emStatus)
	{
		//组织数据
		_MakePacket objMakePacket;

		objMakePacket.m_u4ConnectID       = GetConnectID();
		objMakePacket.m_pPacketParse      = NULL;

		//发送服务器端链接断开消息。
		ACE_Time_Value tvNow = ACE_OS::gettimeofday();
		if(false == App_MakePacket::instance()->PutMessageBlock(GetConnectID(), PACKET_SDISCONNECT, &objMakePacket, tvNow))
		{
			OUR_DEBUG((LM_ERROR, "[CProConnectHandle::ServerClose]ConnectID = %d, PACKET_SDISCONNECT is error.\n", GetConnectID()));
			return false;
		}

		m_Reader.cancel();
		m_Writer.cancel();

		ACE_OS::shutdown(this->handle(), SD_BOTH);

		if(this->handle() != ACE_INVALID_HANDLE)
		{
			ACE_OS::closesocket(this->handle());
			this->handle(ACE_INVALID_HANDLE);
		}

		m_u1ConnectState = CONNECT_SERVER_CLOSE;

	}
	else
	{
		m_emStatus = emStatus;
	}

	return true;
}

void CProConnectHandle::SetConnectID(uint32 u4ConnectID)
{
	m_u4ConnectID = u4ConnectID;
}

uint32 CProConnectHandle::GetConnectID()
{
	return m_u4ConnectID;
}

void CProConnectHandle::addresses (const ACE_INET_Addr &remote_address, const ACE_INET_Addr &local_address)
{
	m_addrRemote = remote_address;
}

void CProConnectHandle::open(ACE_HANDLE h, ACE_Message_Block&)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGuard(m_ThreadWriteLock);
	//OUR_DEBUG((LM_INFO, "[CProConnectHandle::open] [0x%08x]Connection from [%s:%d]\n", this, m_addrRemote.get_host_addr(), m_addrRemote.get_port_number()));

	m_atvConnect      = ACE_OS::gettimeofday();
	m_atvInput        = ACE_OS::gettimeofday();
	m_atvOutput       = ACE_OS::gettimeofday();
	m_atvSendAlive    = ACE_OS::gettimeofday();

	m_u4AllRecvCount      = 0;
	m_u4AllSendCount      = 0;
	m_u4AllRecvSize       = 0;
	m_u4AllSendSize       = 0;
	m_u4RecvPacketCount   = 0;
	m_nIOCount            = 1;
	m_u8RecvQueueTimeCost = 0;
	m_u4RecvQueueCount    = 0;
	m_u8SendQueueTimeCost = 0;
	m_u4SuccessSendSize   = 0;
	m_u4ReadSendSize      = 0;
	m_emStatus            = CLIENT_CLOSE_NOTHING;
	m_blIsLog             = false;
	m_szConnectName[0]    = '\0';
	m_u1IsActive          = 1;

	if(App_ForbiddenIP::instance()->CheckIP(m_addrRemote.get_host_addr()) == false)
	{
		//在禁止列表中，不允许访问
		OUR_DEBUG((LM_ERROR, "[CConnectHandler::open]IP Forbidden(%s).\n", m_addrRemote.get_host_addr()));
		return;
	}


	//检查单位时间链接次数是否达到上限
	if(false == App_IPAccount::instance()->AddIP((string)m_addrRemote.get_host_addr()))
	{
		OUR_DEBUG((LM_ERROR, "[CProConnectHandle::open]IP connect frequently.\n", m_addrRemote.get_host_addr()));
		App_ForbiddenIP::instance()->AddTempIP(m_addrRemote.get_host_addr(), App_MainConfig::instance()->GetIPAlert()->m_u4IPTimeout);

		//发送告警邮件
		AppLogManager::instance()->WriteToMail(LOG_SYSTEM_CONNECT, 
			App_MainConfig::instance()->GetIPAlert()->m_u4MailID,
			"Alert IP",
			"[CConnectHandler::open] IP is more than IP Max,");		

		Close();
		return;
	}

	if(m_u2TcpNodelay == TCP_NODELAY_OFF)
	{
		//如果设置了禁用Nagle算法，则这里要禁用。
		int nOpt = 1; 
		ACE_OS::setsockopt(h, IPPROTO_TCP, TCP_NODELAY, (char* )&nOpt, sizeof(int)); 
	}

	//初始化检查器
	m_TimeConnectInfo.Init(App_MainConfig::instance()->GetClientDataAlert()->m_u4RecvPacketCount, 
		App_MainConfig::instance()->GetClientDataAlert()->m_u4RecvDataMax, 
		App_MainConfig::instance()->GetClientDataAlert()->m_u4SendPacketCount,
		App_MainConfig::instance()->GetClientDataAlert()->m_u4SendDataMax);

	this->handle(h);

	//默认别名是IP地址
	SetConnectName(m_addrRemote.get_host_addr());

	if(this->m_Reader.open(*this, h, 0, proactor()) == -1 || 
		this->m_Writer.open(*this, h, 0, proactor()) == -1)
	{
		OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::open] m_reader or m_reader == 0.\n"));	
		Close();
		return;
	}

	//写入连接日志
	AppLogManager::instance()->WriteLog(LOG_SYSTEM_CONNECT, "Connection from [%s:%d]To Server.",m_addrRemote.get_host_addr(), m_addrRemote.get_port_number());

	//ACE_Sig_Action writeAction((ACE_SignalHandler)SIG_IGN);
	//writeAction.register_action(SIGPIPE, 0);

	//int nTecvBuffSize = MAX_MSG_SOCKETBUFF;
	//ACE_OS::setsockopt(this->get_handle(), SOL_SOCKET, SO_RCVBUF, (char* )&nTecvBuffSize, sizeof(nTecvBuffSize));
	//ACE_OS::setsockopt(h, SOL_SOCKET, SO_SNDBUF, (char* )&nTecvBuffSize, sizeof(nTecvBuffSize));

	//将这个链接放入链接库
	if(false == App_ProConnectManager::instance()->AddConnect(this))
	{
		OUR_DEBUG((LM_ERROR, "%s.\n", App_ProConnectManager::instance()->GetError()));
		sprintf_safe(m_szError, MAX_BUFF_500, "%s", App_ProConnectManager::instance()->GetError());
		Close();
		return;
	}

	m_u1ConnectState = CONNECT_OPEN;

	//OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::open] Open(%d).\n", GetConnectID()));	

	m_pPacketParse = App_PacketParsePool::instance()->Create();
	if(NULL == m_pPacketParse)
	{
		OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::open] Open(%d) m_pPacketParse new error.\n", GetConnectID()));
		Close();
		return;
	}

	//告诉PacketParse连接应建立
	App_PacketParseLoader::instance()->GetPacketParseInfo()->Connect(GetConnectID(), GetClientIPInfo(), GetLocalIPInfo());

	//组织数据
	_MakePacket objMakePacket;

	objMakePacket.m_u4ConnectID       = GetConnectID();
	objMakePacket.m_pPacketParse      = NULL;

	//发送链接建立消息。
	ACE_Time_Value tvNow = ACE_OS::gettimeofday();
	if(false == App_MakePacket::instance()->PutMessageBlock(GetConnectID(), PACKET_CONNECT, &objMakePacket, tvNow))
	{
		OUR_DEBUG((LM_ERROR, "[CProConnectHandle::open] ConnectID = %d, PACKET_CONNECT is error.\n", GetConnectID()));
	}

	OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::open] Open(%d) [%s:%d](0x%08x).\n", GetConnectID(), m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), this));

	if(m_pPacketParse->GetPacketMode() == PACKET_WITHHEAD)
	{
		RecvClinetPacket(m_pPacketParse->GetPacketHeadSrcLen());
	}
	else
	{
		RecvClinetPacket(App_MainConfig::instance()->GetServerRecvBuff());
	}

	return;
}

void CProConnectHandle::handle_read_stream(const ACE_Asynch_Read_Stream::Result &result)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGuard(m_ThreadWriteLock);
	ACE_Message_Block& mb = result.message_block();
	uint32 u4PacketLen = (uint32)result.bytes_transferred();
	int nTran = (int)result.bytes_transferred();

	//OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::handle_read_stream](%d)  bytes_transferred=%d, bytes_to_read=%d.\n", GetConnectID(),  result.bytes_transferred(), result.bytes_to_read()));

	if(!result.success() || result.bytes_transferred() == 0)
	{
		//链接断开
		//清理PacketParse
		ClearPacketParse(mb);

		//关闭当前连接
		Close(2, errno);

		return;
	}

	m_atvInput = ACE_OS::gettimeofday();

	//如果是DEBUG状态，记录当前接受包的二进制数据
	if(App_MainConfig::instance()->GetDebug() == DEBUG_ON || m_blIsLog == true)
	{
		char szDebugData[MAX_BUFF_1024] = {'\0'};
		char szLog[10]  = {'\0'};
		int  nDebugSize = 0; 
		bool blblMore   = false;

		if(mb.length() >= MAX_BUFF_200)
		{
			nDebugSize = MAX_BUFF_200;
			blblMore   = true;
		}
		else
		{
			nDebugSize = (int)mb.length();
		}

		char* pData = mb.rd_ptr();
		for(int i = 0; i < nDebugSize; i++)
		{
			sprintf_safe(szLog, 10, "0x%02X ", (unsigned char)pData[i]);
			sprintf_safe(szDebugData + 5*i, MAX_BUFF_1024 - 5*i, "%s", szLog);
		}

		if(blblMore == true)
		{
			AppLogManager::instance()->WriteLog(LOG_SYSTEM_DEBUG_CLIENTRECV, "[(%s)%s:%d]%s.(数据包过长只记录前200字节)", m_szConnectName, m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), szDebugData);
		}
		else
		{
			AppLogManager::instance()->WriteLog(LOG_SYSTEM_DEBUG_CLIENTRECV, "[(%s)%s:%d]%s.", m_szConnectName, m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), szDebugData);
		}
	}

	if(m_pPacketParse->GetPacketMode() == PACKET_WITHHEAD)
	{
		if(result.bytes_transferred() < result.bytes_to_read())
		{
			//短读，继续读
			int nRead = (int)result.bytes_to_read() - (int)result.bytes_transferred();
			if(-1 == m_Reader.read(mb, nRead))
			{
				//清理PacketParse
				ClearPacketParse(mb);

				//关闭当前连接
				Close(2, errno);
				return;
			}

		}
		else if(mb.length() == m_pPacketParse->GetPacketHeadSrcLen() && m_pPacketParse->GetIsHandleHead())
		{
			//判断头的合法性
			_Head_Info objHeadInfo;
			bool blStateHead = App_PacketParseLoader::instance()->GetPacketParseInfo()->Parse_Packet_Head_Info(GetConnectID(), &mb, App_MessageBlockManager::instance(), &objHeadInfo);
			if(false == blStateHead)
			{
				//如果包头是非法的，则返回错误，断开连接。
				OUR_DEBUG((LM_ERROR, "[CConnectHandler::handle_read_stream]PacketHead is illegal.\n"));

				//清理PacketParse
				ClearPacketParse(mb);

				//关闭当前连接
				Close(2, errno);
				return;
			}
			else
			{
				m_pPacketParse->SetPacket_IsHandleHead(false);
				m_pPacketParse->SetPacket_Head_Message(objHeadInfo.m_pmbHead);
				m_pPacketParse->SetPacket_Head_Curr_Length(objHeadInfo.m_u4HeadCurrLen);
				m_pPacketParse->SetPacket_Body_Src_Length(objHeadInfo.m_u4BodySrcLen);
				m_pPacketParse->SetPacket_CommandID(objHeadInfo.m_u2PacketCommandID);
			}

			//这里添加只处理包头的数据
			//如果数据只有包头，不需要包体，在这里必须做一些处理，让数据只处理包头就扔到DoMessage()
			uint32 u4PacketBodyLen = m_pPacketParse->GetPacketBodySrcLen();

			if(u4PacketBodyLen == 0)
			{
				//如果只有包头没有包体，则直接丢到逻辑里处理
				if(false == CheckMessage())
				{
					Close(2);
					return;
				}

				m_pPacketParse = App_PacketParsePool::instance()->Create();
				if(NULL == m_pPacketParse)
				{
					OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::handle_read_stream] Open(%d) m_pPacketParse new error.\n", GetConnectID()));

					//组织数据
					_MakePacket objMakePacket;

					objMakePacket.m_u4ConnectID       = GetConnectID();
					objMakePacket.m_pPacketParse      = NULL;

					//因为是要关闭连接，所以要多关闭一次IO，对应Open设置的1的初始值
					//发送服务器端链接断开消息。
					ACE_Time_Value tvNow = ACE_OS::gettimeofday();
					if(false == App_MakePacket::instance()->PutMessageBlock(GetConnectID(), PACKET_SDISCONNECT, &objMakePacket, tvNow))
					{
						OUR_DEBUG((LM_ERROR, "[CProConnectHandle::open] ConnectID = %d, PACKET_CONNECT is error.\n", GetConnectID()));
					}

					Close(2);
					return;
				}
				Close();

				//接受下一个数据包
				RecvClinetPacket(m_pPacketParse->GetPacketHeadSrcLen());

			}
			else
			{
				//如果超过了最大包长度，为非法数据
				if(u4PacketBodyLen >= m_u4MaxPacketSize)
				{
					OUR_DEBUG((LM_ERROR, "[CProConnectHandle::handle_read_stream]u4PacketHeadLen(%d) more than %d.\n", u4PacketBodyLen, m_u4MaxPacketSize));

					//清理PacketParse
					ClearPacketParse(mb);

					//关闭当前连接
					Close(2, errno);
				}
				else
				{
					Close();
					RecvClinetPacket(u4PacketBodyLen);
				}
			}
		}
		else
		{
			//接受完整数据完成，开始分析完整数据包
			_Body_Info obj_Body_Info;
			bool blStateBody = App_PacketParseLoader::instance()->GetPacketParseInfo()->Parse_Packet_Body_Info(GetConnectID(), &mb, App_MessageBlockManager::instance(), &obj_Body_Info);
			if(false == blStateBody)
			{
				//如果数据包体非法，断开连接
				OUR_DEBUG((LM_ERROR, "[CProConnectHandle::handle_read_stream]SetPacketBody is illegal.\n"));

				//清理PacketParse
				ClearPacketParse(mb);

				//关闭当前连接
				Close(2, errno);
				return;
			}
			else
			{
				m_pPacketParse->SetPacket_Body_Message(obj_Body_Info.m_pmbBody);
				m_pPacketParse->SetPacket_Body_Curr_Length(obj_Body_Info.m_u4BodyCurrLen);
				if(obj_Body_Info.m_u2PacketCommandID > 0)
				{
					m_pPacketParse->SetPacket_CommandID(obj_Body_Info.m_u2PacketCommandID);
				}
			}

			if(false == CheckMessage())
			{
				Close(2);
				return;
			}

			m_pPacketParse = App_PacketParsePool::instance()->Create();
			if(NULL == m_pPacketParse)
			{
				OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::handle_read_stream] Open(%d) m_pPacketParse new error.\n", GetConnectID()));

				//组织数据
				_MakePacket objMakePacket;

				objMakePacket.m_u4ConnectID       = GetConnectID();
				objMakePacket.m_pPacketParse      = NULL;

				//因为是要关闭连接，所以要多关闭一次IO，对应Open设置的1的初始值
				//发送服务器端链接断开消息。
				ACE_Time_Value tvNow = ACE_OS::gettimeofday();
				if(false == App_MakePacket::instance()->PutMessageBlock(GetConnectID(), PACKET_SDISCONNECT, &objMakePacket, tvNow))
				{
					OUR_DEBUG((LM_ERROR, "[CProConnectHandle::open] ConnectID = %d, PACKET_CONNECT is error.\n", GetConnectID()));
				}

				Close(2);
				return;
			}
			Close();

			//接受下一个数据包
			RecvClinetPacket(m_pPacketParse->GetPacketHeadSrcLen());
		}
	}
	else
	{
		//以流模式解析
		while(true)
		{
			_Packet_Info obj_Packet_Info;
			uint8 n1Ret = App_PacketParseLoader::instance()->GetPacketParseInfo()->Parse_Packet_Stream(GetConnectID(), &mb, (IMessageBlockManager* )App_MessageBlockManager::instance(), &obj_Packet_Info);
			if(PACKET_GET_ENOUGTH == n1Ret)
			{
				m_pPacketParse->SetPacket_Head_Message(obj_Packet_Info.m_pmbHead);
				m_pPacketParse->SetPacket_Body_Message(obj_Packet_Info.m_pmbBody);
				m_pPacketParse->SetPacket_CommandID(obj_Packet_Info.m_u2PacketCommandID);
				m_pPacketParse->SetPacket_Head_Src_Length(obj_Packet_Info.m_u4HeadSrcLen);
				m_pPacketParse->SetPacket_Head_Curr_Length(obj_Packet_Info.m_u4HeadCurrLen);
				m_pPacketParse->SetPacket_Head_Src_Length(obj_Packet_Info.m_u4BodySrcLen);
				m_pPacketParse->SetPacket_Body_Curr_Length(obj_Packet_Info.m_u4BodyCurrLen);

				//已经接收了完整数据包，扔给工作线程去处理
				if(false == CheckMessage())
				{
					Close(2);
					return;
				}

				m_pPacketParse = App_PacketParsePool::instance()->Create();
				if(NULL == m_pPacketParse)
				{
					OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::handle_read_stream](%d) m_pPacketParse new error.\n", GetConnectID()));

					//组织数据
					_MakePacket objMakePacket;

					objMakePacket.m_u4ConnectID       = GetConnectID();
					objMakePacket.m_pPacketParse      = NULL;

					//因为是要关闭连接，所以要多关闭一次IO，对应Open设置的1的初始值
					//发送服务器端链接断开消息。
					ACE_Time_Value tvNow = ACE_OS::gettimeofday();
					if(false == App_MakePacket::instance()->PutMessageBlock(GetConnectID(), PACKET_SDISCONNECT, &objMakePacket, tvNow))
					{
						OUR_DEBUG((LM_ERROR, "[CProConnectHandle::open] ConnectID = %d, PACKET_CONNECT is error.\n", GetConnectID()));
					}

					Close(2);
					return;
				}

				//看看是否接收完成了
				if(mb.length() == 0)
				{
					mb.release();
					break;
				}
				else
				{
					//还有数据，继续分析
					continue;
				}
			}
			else if(PACKET_GET_NO_ENOUGTH == n1Ret)
			{
				//接收的数据不完整，需要继续接收
				break;
			}
			else
			{
				//数据包为错误包，丢弃处理
				m_pPacketParse->Clear();

				Close(2);
				return;
			}
		}

		Close();
		//接受下一个数据包
		RecvClinetPacket(App_MainConfig::instance()->GetServerRecvBuff());
	}

	return;
}

void CProConnectHandle::handle_write_stream(const ACE_Asynch_Write_Stream::Result &result)
{
	if(!result.success() || result.bytes_transferred()==0)
	{
		//发送失败
		int nErrno = errno;
		OUR_DEBUG ((LM_DEBUG,"[CConnectHandler::handle_write_stream] Connectid=[%d] begin(%d)...\n",GetConnectID(), nErrno));

		AppLogManager::instance()->WriteLog(LOG_SYSTEM_CONNECT, "WriteError [%s:%d] nErrno = %d  result.bytes_transferred() = %d, ",
			m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), nErrno, 
			result.bytes_transferred());

		OUR_DEBUG((LM_DEBUG,"[CConnectHandler::handle_write_stream] Connectid=[%d] finish ok...\n", GetConnectID()));
		m_atvOutput = ACE_OS::gettimeofday();
		//App_MessageBlockManager::instance()->Close(&result.message_block());
		//错误消息回调
		App_MakePacket::instance()->PutSendErrorMessage(GetConnectID(), &result.message_block(), m_atvOutput);

		//App_MessageBlockManager::instance()->Close(&result.message_block());

		return;
	}
	else
	{
		//发送成功
		m_ThreadWriteLock.acquire();
		m_atvOutput = ACE_OS::gettimeofday();
		App_MessageBlockManager::instance()->Close(&result.message_block());
		m_u4AllSendSize += (uint32)result.bytes_to_write();

		//如果需要统计信息
		//App_IPAccount::instance()->UpdateIP((string)m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), m_u4AllRecvSize, m_u4AllSendSize);
		m_ThreadWriteLock.release();

		//记录发送字节数
		m_u4SuccessSendSize += (uint32)result.bytes_to_write();

		//查看是否需要关闭
		if(CLIENT_CLOSE_SENDOK == m_emStatus)
		{
			//查看是否所有字节都发送完毕，都发送完毕调用服务器关闭接口
			if(m_u4ReadSendSize - m_u4SuccessSendSize == 0)
			{
				ServerClose(CLIENT_CLOSE_IMMEDIATLY);
			}
		}

		return;
	}
}

bool CProConnectHandle::SetRecvQueueTimeCost(uint32 u4TimeCost)
{
	m_ThreadWriteLock.acquire();
	m_nIOCount++;
	m_ThreadWriteLock.release();

	//如果超过阀值，则记录到日志中去
	if((uint32)(m_u2RecvQueueTimeout * 1000) <= u4TimeCost)
	{
		AppLogManager::instance()->WriteLog(LOG_SYSTEM_RECVQUEUEERROR, "[TCP]IP=%s,Prot=%d,Timeout=[%d].", GetClientIPInfo().m_szClientIP, GetClientIPInfo().m_nPort, u4TimeCost);
	}

	//OUR_DEBUG((LM_ERROR, "[CProConnectHandle::SetRecvQueueTimeCost]m_u4RecvQueueCount=%d.\n", m_u4RecvQueueCount));
	m_u4RecvQueueCount++;

	//测试了几天，感觉这个时间意义，因为获取队列的处理时间片可能很耗时，导致一批数据的阶段性时间增长
	//只要记录超时的数据即可
	//m_u8RecvQueueTimeCost += u4TimeCost;

	Close();
	return true;
}

bool CProConnectHandle::SetSendQueueTimeCost(uint32 u4TimeCost)
{
	m_ThreadWriteLock.acquire();
	m_nIOCount++;
	m_ThreadWriteLock.release();

	//如果超过阀值，则记录到日志中去
	if((uint32)(m_u2SendQueueTimeout * 1000) <= u4TimeCost)
	{
		AppLogManager::instance()->WriteLog(LOG_SYSTEM_SENDQUEUEERROR, "[TCP]IP=%s,Prot=%d,Timeout=[%d].", GetClientIPInfo().m_szClientIP, GetClientIPInfo().m_nPort, u4TimeCost);

		//组织数据
		_MakePacket objMakePacket;

		objMakePacket.m_u4ConnectID       = GetConnectID();
		objMakePacket.m_pPacketParse      = NULL;

		//告诉插件连接发送超时阀值报警
		ACE_Time_Value tvNow = ACE_OS::gettimeofday();
		if(false == App_MakePacket::instance()->PutMessageBlock(GetConnectID(), PACKET_SEND_TIMEOUT, &objMakePacket, tvNow))
		{
			OUR_DEBUG((LM_ERROR, "[CProConnectHandle::open] ConnectID = %d, PACKET_CONNECT is error.\n", GetConnectID()));
		}
	}

	//m_u8SendQueueTimeCost += u4TimeCost;

	Close();
	return true;
}

uint8 CProConnectHandle::GetConnectState()
{
	return m_u1ConnectState;
}

uint8 CProConnectHandle::GetSendBuffState()
{
	return m_u1SendBuffState;
}

bool CProConnectHandle::SendMessage(uint16 u2CommandID, IBuffPacket* pBuffPacket, uint8 u1State, uint8 u1SendType, uint32& u4PacketSize, bool blDelete)
{
	OUR_DEBUG((LM_DEBUG,"[CConnectHandler::SendMessage]Connectid=%d,m_nIOCount=%d.\n", GetConnectID(), m_nIOCount));
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGuard(m_ThreadWriteLock);	
	//OUR_DEBUG((LM_DEBUG,"[CConnectHandler::SendMessage]Connectid=%d,m_nIOCount=%d 1.\n", GetConnectID(), m_nIOCount));

	//如果当前连接已被别的线程关闭，则这里不做处理，直接退出
	if(m_u1IsActive == 0)
	{
		if(blDelete == true)
		{					
			App_BuffPacketManager::instance()->Delete(pBuffPacket);
		}
		return false;
	}

	ACE_Message_Block* pMbData = NULL;

	if(NULL == pBuffPacket)
	{
		OUR_DEBUG((LM_DEBUG,"[CConnectHandler::SendMessage] Connectid=[%d] pBuffPacket is NULL.\n", GetConnectID()));
		return false;
	}

	//如果不是直接发送数据，则拼接数据包
	if(u1State == PACKET_SEND_CACHE)
	{
		//先判断要发送的数据长度，看看是否可以放入缓冲，缓冲是否已经放满。
		uint32 u4SendPacketSize = 0;
		if(u1SendType == SENDMESSAGE_NOMAL)
		{
			u4SendPacketSize = App_PacketParseLoader::instance()->GetPacketParseInfo()->Make_Send_Packet_Length(GetConnectID(), pBuffPacket->GetPacketLen(), u2CommandID);
		}
		else
		{
			u4SendPacketSize = (uint32)m_pBlockMessage->length();
		}
		u4PacketSize = u4SendPacketSize;

		if(u4SendPacketSize + (uint32)m_pBlockMessage->length() >= m_u4SendMaxBuffSize)
		{
			OUR_DEBUG((LM_DEBUG,"[CConnectHandler::SendMessage] Connectid=[%d] m_pBlockMessage is not enougth.\n", GetConnectID()));
			if(blDelete = true)
			{
				App_BuffPacketManager::instance()->Delete(pBuffPacket);
			}
			return false;
		}
		else
		{
			//添加进缓冲区
			ACE_Message_Block* pMbBufferData = NULL;

			//SENDMESSAGE_NOMAL是需要包头的时候，否则，不组包直接发送
			if(u1SendType == SENDMESSAGE_NOMAL)
			{
				//这里组成返回数据包
				App_PacketParseLoader::instance()->GetPacketParseInfo()->Make_Send_Packet(GetConnectID(), pBuffPacket->GetData(), pBuffPacket->GetPacketLen(), m_pBlockMessage, u2CommandID);
			}
			else
			{
				//如果不是SENDMESSAGE_NOMAL，则直接组包
				memcpy_safe((char* )pBuffPacket->GetData(), pBuffPacket->GetPacketLen(), (char* )m_pBlockMessage->wr_ptr(), pBuffPacket->GetPacketLen());
				m_pBlockMessage->wr_ptr(pBuffPacket->GetPacketLen());
			}
		}

		if(blDelete = true)
		{
			App_BuffPacketManager::instance()->Delete(pBuffPacket);
		}

		return true;
	}
	else
	{
		//先判断是否要组装包头，如果需要，则组装在m_pBlockMessage中
		uint32 u4SendPacketSize = 0;
		if(u1SendType == SENDMESSAGE_NOMAL)
		{
			u4SendPacketSize = App_PacketParseLoader::instance()->GetPacketParseInfo()->Make_Send_Packet_Length(GetConnectID(), pBuffPacket->GetPacketLen(), u2CommandID);

			if(u4SendPacketSize >= m_u4SendMaxBuffSize)
			{
				OUR_DEBUG((LM_DEBUG,"[CConnectHandler::SendMessage](%d) u4SendPacketSize is more than(%d)(%d).\n", GetConnectID(), u4SendPacketSize, m_u4SendMaxBuffSize));
				if(blDelete == true)
				{
					//删除发送数据包 
					App_BuffPacketManager::instance()->Delete(pBuffPacket);
				}
				return false;
			}

			App_PacketParseLoader::instance()->GetPacketParseInfo()->Make_Send_Packet(GetConnectID(), pBuffPacket->GetData(), pBuffPacket->GetPacketLen(), m_pBlockMessage, u2CommandID);
			//这里MakePacket已经加了数据长度，所以在这里不再追加
		}
		else
		{
			u4SendPacketSize = (uint32)pBuffPacket->GetPacketLen();

			if(u4SendPacketSize >= m_u4SendMaxBuffSize)
			{
				OUR_DEBUG((LM_DEBUG,"[CConnectHandler::SendMessage](%d) u4SendPacketSize is more than(%d)(%d).\n", GetConnectID(), u4SendPacketSize, m_u4SendMaxBuffSize));
				if(blDelete == true)
				{
					//删除发送数据包 
					App_BuffPacketManager::instance()->Delete(pBuffPacket);
				}
				return false;
			}

			memcpy_safe((char* )pBuffPacket->GetData(), pBuffPacket->GetPacketLen(), (char* )m_pBlockMessage->wr_ptr(), pBuffPacket->GetPacketLen());
			m_pBlockMessage->wr_ptr(pBuffPacket->GetPacketLen());
		}

		//如果之前有缓冲数据，则和缓冲数据一起发送
		u4PacketSize = m_pBlockMessage->length();

		//如果之前有缓冲数据，则和缓冲数据一起发送
		if(m_pBlockMessage->length() > 0)
		{
			//因为是异步发送，发送的数据指针不可以立刻释放，所以需要在这里创建一个新的发送数据块，将数据考入
			pMbData = App_MessageBlockManager::instance()->Create((uint32)m_pBlockMessage->length());
			if(NULL == pMbData)
			{
				OUR_DEBUG((LM_DEBUG,"[CConnectHandler::SendMessage] Connectid=[%d] pMbData is NULL.\n", GetConnectID()));
				App_BuffPacketManager::instance()->Delete(pBuffPacket);
				return false;
			}

			memcpy_safe(m_pBlockMessage->rd_ptr(), m_pBlockMessage->length(), pMbData->wr_ptr(), m_pBlockMessage->length());
			pMbData->wr_ptr(m_pBlockMessage->length());
			//放入完成，则清空缓存数据，使命完成
			m_pBlockMessage->reset();
		}

		if(blDelete = true)
		{
			App_BuffPacketManager::instance()->Delete(pBuffPacket);
		}

		//判断是否发送完成后关闭连接
		if(PACKET_SEND_FIN_CLOSE == u1State)
		{
			m_emStatus = CLIENT_CLOSE_SENDOK;
		}

		return PutSendPacket(pMbData);
	}
}

bool CProConnectHandle::CheckAlive(ACE_Time_Value& tvNow)
{
	//ACE_Time_Value tvNow = ACE_OS::gettimeofday();
	ACE_Time_Value tvIntval(tvNow - m_atvInput);
	if(tvIntval.sec() > m_u2MaxConnectTime)
	{
		//如果超过了最大时间，则服务器关闭链接
		OUR_DEBUG ((LM_ERROR, "[CProConnectHandle::CheckAlive] Connectid=%d Server Close!\n", GetConnectID()));

		return false;
	}
	else
	{
		return true;
	}
}

bool CProConnectHandle::PutSendPacket(ACE_Message_Block* pMbData)
{
	if(NULL == pMbData)
	{
		return false;
	}

	//如果是DEBUG状态，记录当前发送包的二进制数据
	if(App_MainConfig::instance()->GetDebug() == DEBUG_ON || m_blIsLog == true)
	{
		char szDebugData[MAX_BUFF_1024] = {'\0'};
		char szLog[10]  = {'\0'};
		int  nDebugSize = 0; 
		bool blblMore   = false;

		if(pMbData->length() >= MAX_BUFF_200)
		{
			nDebugSize = MAX_BUFF_200;
			blblMore   = true;
		}
		else
		{
			nDebugSize = (int)pMbData->length();
		}

		char* pData = pMbData->rd_ptr();
		for(int i = 0; i < nDebugSize; i++)
		{
			sprintf_safe(szLog, 10, "0x%02X ", (unsigned char)pData[i]);
			sprintf_safe(szDebugData + 5*i, MAX_BUFF_1024 -  5*i, "0x%02X ", (unsigned char)pData[i]);
		}

		if(blblMore == true)
		{
			AppLogManager::instance()->WriteLog(LOG_SYSTEM_DEBUG_CLIENTSEND, "[(%s)%s:%d]%s.(数据包过长只记录前200字节)", m_szConnectName, m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), szDebugData);
		}
		else
		{
			AppLogManager::instance()->WriteLog(LOG_SYSTEM_DEBUG_CLIENTSEND, "[(%s)%s:%d]%s.", m_szConnectName, m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), szDebugData);
		}
	}

	//OUR_DEBUG ((LM_ERROR, "[CConnectHandler::PutSendPacket] Connectid=%d, m_nIOCount=%d!\n", GetConnectID(), m_nIOCount));
	//统计发送数量
	ACE_Date_Time dtNow;
	if(false == m_TimeConnectInfo.SendCheck((uint8)dtNow.minute(), 1, pMbData->length()))
	{
		//超过了限定的阀值，需要关闭链接，并记录日志
		AppLogManager::instance()->WriteToMail(LOG_SYSTEM_CONNECTABNORMAL, 
			App_MainConfig::instance()->GetClientDataAlert()->m_u4MailID, 
			"Alert",
			"[TCP]IP=%s,Prot=%d,SendPacketCount=%d, SendSize=%d.", 
			m_addrRemote.get_host_addr(), 
			m_addrRemote.get_port_number(), 
			m_TimeConnectInfo.m_u4SendPacketCount, 
			m_TimeConnectInfo.m_u4SendSize);

		//设置封禁时间
		App_ForbiddenIP::instance()->AddTempIP(m_addrRemote.get_host_addr(), App_MainConfig::instance()->GetIPAlert()->m_u4IPTimeout);
		OUR_DEBUG((LM_ERROR, "[CProConnectHandle::PutSendPacket] ConnectID = %d, Send Data is more than limit.\n", GetConnectID()));

		App_MessageBlockManager::instance()->Close(pMbData);
		return false;
	}

	//异步发送方法
	if(NULL != pMbData)
	{
		//记录水位标
		m_u4ReadSendSize += pMbData->length();

		//比较水位标，是否超过一定数值，也就是说发快收慢的时候，如果超过一定数值，断开连接
		if(m_u4ReadSendSize - m_u4SuccessSendSize >= App_MainConfig::instance()->GetSendDataMask())
		{
			OUR_DEBUG ((LM_ERROR, "[CProConnectHandle::PutSendPacket]ConnectID = %d, SingleConnectMaxSendBuffer is more than(%d)!\n", GetConnectID(), m_u4ReadSendSize - m_u4SuccessSendSize));
			AppLogManager::instance()->WriteLog(LOG_SYSTEM_SENDQUEUEERROR, "]Connection from [%s:%d], SingleConnectMaxSendBuffer is more than(%d)!.", m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), m_u4ReadSendSize - m_u4SuccessSendSize);
			App_MessageBlockManager::instance()->Close(pMbData);
			return false;
		}

		//OUR_DEBUG ((LM_ERROR, "[CConnectHandler::PutSendPacket] Connectid=%d, length=%d!\n", GetConnectID(), pMbData->length()));
		if(0 != m_Writer.write(*pMbData, pMbData->length()))
		{
			OUR_DEBUG ((LM_ERROR, "[CProConnectHandle::PutSendPacket] Connectid=%d mb=%d m_writer.write error(%d)!\n", GetConnectID(),  pMbData->length(), errno));
			App_MessageBlockManager::instance()->Close(pMbData);
			return false;
		}
		else
		{
			OUR_DEBUG ((LM_ERROR, "[CProConnectHandle::PutSendPacket](%s:%d) Send(%d) OK!\n", m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), pMbData->length()));
			m_u4AllSendCount += 1;
			m_atvOutput      = ACE_OS::gettimeofday();
			return true;
		}
	}
	else
	{
		OUR_DEBUG ((LM_ERROR,"[CProConnectHandle::PutSendPacket] Connectid=%d mb is NULL!\n", GetConnectID()));;
		return false;
	}
}

bool CProConnectHandle::RecvClinetPacket(uint32 u4PackeLen)
{
	m_nIOCount++;
	//OUR_DEBUG((LM_ERROR, "[CProConnectHandle::RecvClinetPacket]Connectid=%d, m_nIOCount=%d.\n", GetConnectID(), m_nIOCount));

	ACE_Message_Block* pmb = NULL;
	pmb = App_MessageBlockManager::instance()->Create(u4PackeLen);
	if(pmb == NULL)
	{
		AppLogManager::instance()->WriteLog(LOG_SYSTEM_CONNECT, "Close Connection from [%s:%d] RecvSize = %d, RecvCount = %d, SendSize = %d, SendCount = %d, RecvQueueCount=%d, RecvQueueTimeCost=%I64d, SendQueueTimeCost=%I64d.",m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), m_u4AllRecvSize, m_u4AllRecvCount, m_u4AllSendSize, m_u4AllSendCount, m_u4RecvQueueCount, m_u8RecvQueueTimeCost, m_u8SendQueueTimeCost);
		OUR_DEBUG((LM_ERROR, "[CProConnectHandle::RecvClinetPacket] pmb new is NULL.\n"));
		ClearPacketParse(*pmb);
		Close(2);
		return false;
	}

	if(m_Reader.read(*pmb, u4PackeLen) == -1)
	{
		//如果读失败，则关闭连接。
		AppLogManager::instance()->WriteLog(LOG_SYSTEM_CONNECT, "Close Connection from [%s:%d] RecvSize = %d, RecvCount = %d, SendSize = %d, SendCount = %d, RecvQueueCount=%d, RecvQueueTimeCost=%I64d, SendQueueTimeCost=%I64d.",m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), m_u4AllRecvSize, m_u4AllRecvCount, m_u4AllSendSize, m_u4AllSendCount, m_u4RecvQueueCount, m_u8RecvQueueTimeCost, m_u8SendQueueTimeCost);
		OUR_DEBUG((LM_ERROR, "[CProConnectHandle::RecvClinetPacket] m_reader.read is error(%d)(%d).\n", GetConnectID(), errno));
		ClearPacketParse(*pmb);
		Close(2);
		return false;
	}

	return true;
}

bool CProConnectHandle::CheckMessage()
{
	if(m_pPacketParse->GetMessageHead() != NULL)
	{
		//m_ThreadWriteLock.acquire();
		if(m_pPacketParse->GetMessageBody() == NULL)
		{
			m_u4AllRecvSize += (uint32)m_pPacketParse->GetMessageHead()->length();
		}
		else
		{
			m_u4AllRecvSize += (uint32)m_pPacketParse->GetMessageHead()->length() + (uint32)m_pPacketParse->GetMessageBody()->length();
		}
		m_u4AllRecvCount++;

		//如果有需要监控的IP，则记录字节流信息
		//App_IPAccount::instance()->UpdateIP((string)m_addrRemote.get_host_addr(), m_addrRemote.get_port_number(), m_u4AllRecvSize, m_u4AllSendSize);
		//m_ThreadWriteLock.release();

		ACE_Date_Time dtNow;
		if(false == m_TimeConnectInfo.RecvCheck((uint8)dtNow.minute(), 1, m_u4AllRecvSize))
		{
			//超过了限定的阀值，需要关闭链接，并记录日志
			AppLogManager::instance()->WriteToMail(LOG_SYSTEM_CONNECTABNORMAL, 
				App_MainConfig::instance()->GetClientDataAlert()->m_u4MailID,
				"Alert", 
				"[TCP]IP=%s,Prot=%d,PacketCount=%d, RecvSize=%d.", 
				m_addrRemote.get_host_addr(), 
				m_addrRemote.get_port_number(), 
				m_TimeConnectInfo.m_u4RecvPacketCount, 
				m_TimeConnectInfo.m_u4RecvSize);


			App_PacketParsePool::instance()->Delete(m_pPacketParse);
			//设置封禁时间
			App_ForbiddenIP::instance()->AddTempIP(m_addrRemote.get_host_addr(), App_MainConfig::instance()->GetIPAlert()->m_u4IPTimeout);
			OUR_DEBUG((LM_ERROR, "[CProConnectHandle::CheckMessage] ConnectID = %d, PutMessageBlock is check invalid.\n", GetConnectID()));
			return false;
		}

		//组织数据
		_MakePacket objMakePacket;

		objMakePacket.m_pPacketParse      = m_pPacketParse;
		if(ACE_OS::strcmp("INADDR_ANY", m_szLocalIP) == 0)
		{
			objMakePacket.m_AddrListen.set(m_u4LocalPort);
		}
		else
		{
			objMakePacket.m_AddrListen.set(m_u4LocalPort, m_szLocalIP);
		}

		//将数据Buff放入消息体中，传递给MakePacket处理。
		ACE_Time_Value tvNow = ACE_OS::gettimeofday();
		if(false == App_MakePacket::instance()->PutMessageBlock(GetConnectID(), PACKET_PARSE, &objMakePacket, tvNow))
		{
			OUR_DEBUG((LM_ERROR, "[CProConnectHandle::CheckMessage] ConnectID = %d, PutMessageBlock is error.\n", GetConnectID()));
		}

		//OUR_DEBUG((LM_ERROR, "[CProConnectHandle::CheckMessage] ConnectID = %d, put OK.\n", GetConnectID()));

		//清理用完的m_pPacketParse
		App_PacketParsePool::instance()->Delete(m_pPacketParse);
	}
	else
	{
		OUR_DEBUG((LM_ERROR, "[CProConnectHandle::CheckMessage] ConnectID = %d, m_pPacketParse is NULL.\n", GetConnectID()));
	}

	return true;
}

_ClientConnectInfo CProConnectHandle::GetClientInfo()
{
	_ClientConnectInfo ClientConnectInfo;

	ClientConnectInfo.m_blValid             = true;
	ClientConnectInfo.m_u4ConnectID         = GetConnectID();
	ClientConnectInfo.m_addrRemote          = m_addrRemote;
	ClientConnectInfo.m_u4RecvCount         = m_u4AllRecvCount;
	ClientConnectInfo.m_u4SendCount         = m_u4AllSendCount;
	ClientConnectInfo.m_u4AllRecvSize       = m_u4AllSendSize;
	ClientConnectInfo.m_u4AllSendSize       = m_u4AllSendSize;
	ClientConnectInfo.m_u4BeginTime         = (uint32)m_atvConnect.sec();
	ClientConnectInfo.m_u4AliveTime         = (uint32)(ACE_OS::gettimeofday().sec()  -  m_atvConnect.sec());
	ClientConnectInfo.m_u4RecvQueueCount    = m_u4RecvQueueCount;
	ClientConnectInfo.m_u8RecvQueueTimeCost = m_u8RecvQueueTimeCost;
	ClientConnectInfo.m_u8SendQueueTimeCost = m_u8SendQueueTimeCost;

	return ClientConnectInfo;
}

_ClientIPInfo CProConnectHandle::GetClientIPInfo()
{
	_ClientIPInfo ClientIPInfo;
	sprintf_safe(ClientIPInfo.m_szClientIP, MAX_BUFF_50, "%s", m_addrRemote.get_host_addr());
	ClientIPInfo.m_nPort = (int)m_addrRemote.get_port_number();
	return ClientIPInfo;
}

_ClientIPInfo CProConnectHandle::GetLocalIPInfo()
{
	_ClientIPInfo ClientIPInfo;
	sprintf_safe(ClientIPInfo.m_szClientIP, MAX_BUFF_50, "%s", m_szLocalIP);
	ClientIPInfo.m_nPort = (int)m_u4LocalPort;
	return ClientIPInfo;
}

void CProConnectHandle::ClearPacketParse(ACE_Message_Block& mbCurrBlock)
{
	if(NULL != m_pPacketParse && m_pPacketParse->GetMessageHead() != NULL)
	{
		App_MessageBlockManager::instance()->Close(m_pPacketParse->GetMessageHead());
	}

	if(NULL != m_pPacketParse && m_pPacketParse->GetMessageBody() != NULL)
	{
		App_MessageBlockManager::instance()->Close(m_pPacketParse->GetMessageBody());
	}

	if(NULL != m_pPacketParse)
	{
		if(NULL != &mbCurrBlock && &mbCurrBlock != m_pPacketParse->GetMessageHead() && &mbCurrBlock != m_pPacketParse->GetMessageBody())
		{
			//OUR_DEBUG((LM_DEBUG,"[CProConnectHandle::handle_read_stream] Message_block release.\n"));
			App_MessageBlockManager::instance()->Close(&mbCurrBlock);
		}

		App_PacketParsePool::instance()->Delete(m_pPacketParse);
		m_pPacketParse = NULL;
	}
	else
	{
		if(NULL != &mbCurrBlock)
		{
			App_MessageBlockManager::instance()->Close(&mbCurrBlock);
		}
	}
}

char* CProConnectHandle::GetConnectName()
{
	return m_szConnectName;
}

void CProConnectHandle::SetConnectName( const char* pName )
{
	sprintf_safe(m_szConnectName, MAX_BUFF_100, "%s", pName);
}

void CProConnectHandle::SetIsLog(bool blIsLog)
{
	m_blIsLog = blIsLog;
}

bool CProConnectHandle::GetIsLog()
{
	return m_blIsLog;
}

void CProConnectHandle::SetHashID(int nHashID)
{
	m_nHashID = nHashID;
}

int CProConnectHandle::GetHashID()
{
	return m_nHashID;
}

void CProConnectHandle::SetLocalIPInfo(const char* pLocalIP, uint32 u4LocalPort)
{
	sprintf_safe(m_szLocalIP, MAX_BUFF_50, "%s", pLocalIP);
	m_u4LocalPort = u4LocalPort;
}

void CProConnectHandle::PutSendPacketError(ACE_Message_Block* pMbData)
{

}

void CProConnectHandle::SetSendCacheManager(ISendCacheManager* pSendCacheManager)
{
	m_pBlockMessage = pSendCacheManager->GetCacheData(GetConnectID());
	if(NULL == m_pBlockMessage)
	{
		OUR_DEBUG((LM_ERROR, "[CProConnectHandle::SetSendCacheManager] ConnectID = %d, m_pBlockMessage is NULL.\n", GetConnectID()));
	}
}

//***************************************************************************
CProConnectManager::CProConnectManager(void)
{
	m_u4TimeCheckID      = 0;
	m_szError[0]         = '\0';
	m_blRun              = false;

	m_u4TimeConnect      = 0;
	m_u4TimeDisConnect   = 0;

	m_tvCheckConnect     = ACE_OS::gettimeofday();

	m_SendMessagePool.Init();
}

CProConnectManager::~CProConnectManager(void)
{
	CloseAll();
}

void CProConnectManager::CloseAll()
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);

	msg_queue()->deactivate();

	KillTimer();
	vector<CProConnectHandle*> vecCloseConnectHandler;
	for(int i = 0; i < m_objHashConnectList.Get_Count(); i++)
	{
		CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Index(i);
		if(pConnectHandler != NULL)
		{
			vecCloseConnectHandler.push_back(pConnectHandler);
			m_u4TimeDisConnect++;

			//加入链接统计功能
			//App_ConnectAccount::instance()->AddDisConnect();
		}
	}

	//开始关闭所有连接
	for(int i = 0; i < (int)vecCloseConnectHandler.size(); i++)
	{
		CProConnectHandle* pConnectHandler = vecCloseConnectHandler[i];
		pConnectHandler->Close();
	}

	//删除hash表空间
	m_objHashConnectList.Close();

	//删除缓冲对象
	m_SendCacheManager.Close();
}

bool CProConnectManager::Close(uint32 u4ConnectID)
{
	//客户端关闭
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	//OUR_DEBUG((LM_ERROR, "[CProConnectHandle::Close](%d)Begin.\n", u4ConnectID));

	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	m_objHashConnectList.Del_Hash_Data(szConnectID);
	m_u4TimeDisConnect++;

	//回收发送内存块
	m_SendCacheManager.FreeCacheData(u4ConnectID);

	//加入链接统计功能
	App_ConnectAccount::instance()->AddDisConnect();

	//OUR_DEBUG((LM_ERROR, "[CProConnectHandle::Close](%d)End.\n", u4ConnectID));
	return true;
}

bool CProConnectManager::CloseConnect(uint32 u4ConnectID, EM_Client_Close_status emStatus)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	//服务器关闭
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);
	if(pConnectHandler != NULL)
	{
		pConnectHandler->ServerClose(emStatus);

		m_u4TimeDisConnect++;

		//回收发送内存块
		m_SendCacheManager.FreeCacheData(u4ConnectID);

		//加入链接统计功能
		App_ConnectAccount::instance()->AddDisConnect();
		return true;
		m_objHashConnectList.Del_Hash_Data(szConnectID);
	}
	else
	{
		sprintf_safe(m_szError, MAX_BUFF_500, "[CProConnectManager::CloseConnect] ConnectID[%d] is not find.", u4ConnectID);
		return true;
	}
}

bool CProConnectManager::AddConnect(uint32 u4ConnectID, CProConnectHandle* pConnectHandler)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	//OUR_DEBUG((LM_ERROR, "[CProConnectHandle::AddConnect](%d)Begin.\n", u4ConnectID));

	if(pConnectHandler == NULL)
	{
		sprintf_safe(m_szError, MAX_BUFF_500, "[CProConnectManager::AddConnect] pConnectHandler is NULL.");
		return false;		
	}

	//m_ThreadWriteLock.acquire();
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pCurrConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);
	if(NULL != pCurrConnectHandler)
	{
		sprintf_safe(m_szError, MAX_BUFF_500, "[CProConnectManager::AddConnect] ConnectID[%d] is exist.", u4ConnectID);
		m_ThreadWriteLock.release();
		return false;
	}

	pConnectHandler->SetConnectID(u4ConnectID);
	pConnectHandler->SetSendCacheManager((ISendCacheManager* )&m_SendCacheManager);

	//加入Hash数组
	m_objHashConnectList.Add_Hash_Data(szConnectID, pConnectHandler);
	m_u4TimeConnect++;

	//加入链接统计功能
	App_ConnectAccount::instance()->AddConnect();

	//m_ThreadWriteLock.release();

	//OUR_DEBUG((LM_ERROR, "[CProConnectHandle::AddConnect](%d)End.\n", u4ConnectID));

	return true;
}

bool CProConnectManager::SendMessage(uint32 u4ConnectID, IBuffPacket* pBuffPacket, uint16 u2CommandID, uint8 u1SendState, uint8 u1SendType, ACE_Time_Value& tvSendBegin, bool blDelete)
{
	m_ThreadWriteLock.acquire();
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);
	m_ThreadWriteLock.release();

	OUR_DEBUG((LM_ERROR,"[CProConnectManager::SendMessage] (%d) Send Begin 1(0x%08x).\n", u4ConnectID, pConnectHandler));

	uint32 u4CommandSize = pBuffPacket->GetPacketLen();

	if(NULL != pConnectHandler)
	{
		uint32 u4PacketSize  = 0;
		pConnectHandler->SendMessage(u2CommandID, pBuffPacket, u1SendState, u1SendType, u4PacketSize, blDelete);
		m_CommandAccount.SaveCommandData(u2CommandID, (uint64)0, PACKET_TCP, u4PacketSize, u4CommandSize, COMMAND_TYPE_OUT);
		return true;
	}
	else
	{
		sprintf_safe(m_szError, MAX_BUFF_500, "[CProConnectManager::SendMessage] ConnectID[%d] is not find.", u4ConnectID);
		App_BuffPacketManager::instance()->Delete(pBuffPacket);
		return true;
	}
	return true;
}

bool CProConnectManager::PostMessage(uint32 u4ConnectID, IBuffPacket* pBuffPacket, uint8 u1SendType, uint16 u2CommandID, uint8 u1SendState, bool blDelete)
{
	//OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage]BEGIN.\n"));
	OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] (%d) Send Begin.\n", u4ConnectID));

	//放入发送队列
	_SendMessage* pSendMessage = m_SendMessagePool.Create();

	if(NULL == pBuffPacket)
	{
		OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] pBuffPacket is NULL.\n"));
		return false;
	}

	ACE_Message_Block* mb = pSendMessage->GetQueueMessage();

	if(NULL != mb)
	{
		if(NULL == pSendMessage)
		{
			OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] new _SendMessage is error.\n"));
			if(blDelete == true)
			{
				App_BuffPacketManager::instance()->Delete(pBuffPacket);
			}
			return false;
		}

		pSendMessage->m_u4ConnectID = u4ConnectID;
		pSendMessage->m_pBuffPacket = pBuffPacket;
		pSendMessage->m_nEvents     = u1SendType;
		pSendMessage->m_u2CommandID = u2CommandID;
		pSendMessage->m_u1SendState = u1SendState;
		pSendMessage->m_blDelete    = blDelete;
		pSendMessage->m_tvSend      = ACE_OS::gettimeofday();

		//判断队列是否是已经最大
		int nQueueCount = (int)msg_queue()->message_count();
		if(nQueueCount >= (int)MAX_MSG_THREADQUEUE)
		{
			OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] Queue is Full nQueueCount = [%d].\n", nQueueCount));
			if(blDelete == true)
			{
				App_BuffPacketManager::instance()->Delete(pBuffPacket);
			}
			return false;
		}

		ACE_Time_Value xtime = ACE_OS::gettimeofday() + ACE_Time_Value(0, m_u4SendQueuePutTime);
		if(this->putq(mb, &xtime) == -1)
		{
			OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] Queue putq  error nQueueCount = [%d] errno = [%d].\n", nQueueCount, errno));
			if(blDelete == true)
			{
				App_BuffPacketManager::instance()->Delete(pBuffPacket);
			}
			return false;
		}
	}
	else
	{
		OUR_DEBUG((LM_ERROR,"[CMessageService::PutMessage] mb new error.\n"));
		if(blDelete == true)
		{
			App_BuffPacketManager::instance()->Delete(pBuffPacket);
		}
		return false;
	}

	return true;
}

const char* CProConnectManager::GetError()
{
	return m_szError;
}

bool CProConnectManager::StartTimer()
{
	//启动发送线程
	if(0 != open())
	{
		OUR_DEBUG((LM_ERROR, "[CProConnectManager::StartTimer]Open() is error.\n"));
		return false;
	}

	//避免定时器重复启动
	KillTimer();
	OUR_DEBUG((LM_ERROR, "CProConnectManager::StartTimer()-->begin....\n"));

	//检测链接发送存活包数
	uint16 u2CheckAlive = App_MainConfig::instance()->GetCheckAliveTime();
	m_u4TimeCheckID = App_TimerManager::instance()->schedule(this, (void *)NULL, ACE_OS::gettimeofday() + ACE_Time_Value(u2CheckAlive), ACE_Time_Value(u2CheckAlive));
	if(-1 == m_u4TimeCheckID)
	{
		OUR_DEBUG((LM_ERROR, "CProConnectManager::StartTimer()--> Start thread m_u4TimeCheckID error.\n"));
		return false;
	}
	else
	{
		OUR_DEBUG((LM_ERROR, "CProConnectManager::StartTimer()--> Start thread time OK.\n"));
		return true;
	}
}

bool CProConnectManager::KillTimer()
{
	if(m_u4TimeCheckID > 0)
	{
		App_TimerManager::instance()->cancel(m_u4TimeCheckID);
		m_u4TimeCheckID = 0;
	}

	return true;
}

int CProConnectManager::handle_timeout(const ACE_Time_Value &tv, const void *arg)
{
	//ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	ACE_Time_Value tvNow = ACE_OS::gettimeofday();
	vector<CProConnectHandle*> vecDelProConnectHandle;
	//为了防止多线程下的链接删除问题，先把所有的链接ID读出来，再做遍历操作，减少线程竞争的机会。
	if(m_objHashConnectList.Get_Used_Count() != 0)
	{
		m_ThreadWriteLock.acquire();
		for(int i = 0; i < m_objHashConnectList.Get_Count(); i++)
		{
			CProConnectHandle* pConnectHandler = (CProConnectHandle* )m_objHashConnectList.Get_Index(i);
			if(pConnectHandler != NULL)
			{
				if(false == pConnectHandler->CheckAlive(tvNow))
				{
					vecDelProConnectHandle.push_back(pConnectHandler);
				}
			}
		}
		m_ThreadWriteLock.release();
	}

	for(uint32 i = 0; i < (uint32)vecDelProConnectHandle.size(); i++)
	{
		//关闭引用关系
		Close(vecDelProConnectHandle[i]->GetConnectID());
		//回收发送内存块
		m_SendCacheManager.FreeCacheData(vecDelProConnectHandle[i]->GetConnectID());
		vecDelProConnectHandle[i]->ServerClose(CLIENT_CLOSE_IMMEDIATLY);
	}

	//判定是否应该记录链接日志
	ACE_Time_Value tvInterval(tvNow - m_tvCheckConnect);
	if(tvInterval.sec() >= MAX_MSG_HANDLETIME)
	{
		AppLogManager::instance()->WriteLog(LOG_SYSTEM_CONNECT, "[CProConnectManager]CurrConnectCount = %d,TimeInterval=%d, TimeConnect=%d, TimeDisConnect=%d.", 
			GetCount(), MAX_MSG_HANDLETIME, m_u4TimeConnect, m_u4TimeDisConnect);

		//重置单位时间连接数和断开连接数
		m_u4TimeConnect    = 0;
		m_u4TimeDisConnect = 0;
		m_tvCheckConnect   = tvNow;
	}

	//检测连接总数是否超越监控阀值
	if(App_MainConfig::instance()->GetConnectAlert()->m_u4ConnectAlert > 0)
	{
		if(GetCount() > (int)App_MainConfig::instance()->GetConnectAlert()->m_u4ConnectAlert)
		{
			AppLogManager::instance()->WriteToMail(LOG_SYSTEM_CONNECT,
				App_MainConfig::instance()->GetConnectAlert()->m_u4MailID,
				(char* )"Alert",
				"[CProConnectManager]active ConnectCount is more than limit(%d > %d).", 
				GetCount(),
				App_MainConfig::instance()->GetConnectAlert()->m_u4ConnectAlert);
		}
	}

	//检测单位时间连接数是否超越阀值
	int nCheckRet = App_ConnectAccount::instance()->CheckConnectCount();
	if(nCheckRet == 1)
	{
		AppLogManager::instance()->WriteToMail(LOG_SYSTEM_CONNECT,
			App_MainConfig::instance()->GetConnectAlert()->m_u4MailID,
			"Alert",
			"[CProConnectManager]CheckConnectCount is more than limit(%d > %d).", 
			App_ConnectAccount::instance()->GetCurrConnect(),
			App_ConnectAccount::instance()->GetConnectMax());
	}
	else if(nCheckRet == 2)
	{
		AppLogManager::instance()->WriteToMail(LOG_SYSTEM_CONNECT,
			App_MainConfig::instance()->GetConnectAlert()->m_u4MailID,
			"Alert",
			"[CProConnectManager]CheckConnectCount is little than limit(%d < %d).", 
			App_ConnectAccount::instance()->GetCurrConnect(),
			App_ConnectAccount::instance()->Get4ConnectMin());
	}

	//检测单位时间连接断开数是否超越阀值
	nCheckRet = App_ConnectAccount::instance()->CheckDisConnectCount();
	if(nCheckRet == 1)
	{
		AppLogManager::instance()->WriteToMail(LOG_SYSTEM_CONNECT,
			App_MainConfig::instance()->GetConnectAlert()->m_u4MailID,
			"Alert",
			"[CProConnectManager]CheckDisConnectCount is more than limit(%d > %d).", 
			App_ConnectAccount::instance()->GetCurrConnect(),
			App_ConnectAccount::instance()->GetDisConnectMax());
	}
	else if(nCheckRet == 2)
	{
		AppLogManager::instance()->WriteToMail(LOG_SYSTEM_CONNECT,
			App_MainConfig::instance()->GetConnectAlert()->m_u4MailID,
			"Alert",
			"[CProConnectManager]CheckDisConnectCount is little than limit(%d < %d).", 
			App_ConnectAccount::instance()->GetCurrConnect(),
			App_ConnectAccount::instance()->GetDisConnectMin());
	}

	return 0;
}

int CProConnectManager::GetCount()
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	return (int)m_objHashConnectList.Get_Used_Count(); 
}

int CProConnectManager::open(void* args)
{
	m_blRun = true;
	msg_queue()->high_water_mark(MAX_MSG_MASK);
	msg_queue()->low_water_mark(MAX_MSG_MASK);

	OUR_DEBUG((LM_INFO,"[CProConnectManager::open] m_u4HighMask = [%d] m_u4LowMask = [%d]\n", MAX_MSG_MASK, MAX_MSG_MASK));
	if(activate(THR_NEW_LWP | THR_JOINABLE | THR_INHERIT_SCHED | THR_SUSPENDED, MAX_MSG_THREADCOUNT) == -1)
	{
		OUR_DEBUG((LM_ERROR, "[CProConnectManager::open] activate error ThreadCount = [%d].", MAX_MSG_THREADCOUNT));
		m_blRun = false;
		return -1;
	}

	m_u4SendQueuePutTime = App_MainConfig::instance()->GetSendQueuePutTime() * 1000;

	resume();

	return 0;
}

int CProConnectManager::svc (void)
{
	ACE_Message_Block* mb = NULL;
	ACE_Time_Value xtime;

	while(IsRun())
	{
		mb = NULL;
		//xtime = ACE_OS::gettimeofday() + ACE_Time_Value(0, MAX_MSG_PUTTIMEOUT);
		if(getq(mb, 0) == -1)
		{
			OUR_DEBUG((LM_INFO,"[CProConnectManager::svc] getq is error[%d]!\n", errno));
			m_blRun = false;
			break;
		}
		if (mb == NULL)
		{
			continue;
		}
		_SendMessage* msg = *((_SendMessage**)mb->base());
		if (! msg)
		{
			continue;
		}

		//处理发送数据
		SendMessage(msg->m_u4ConnectID, msg->m_pBuffPacket, msg->m_u2CommandID, msg->m_u1SendState, msg->m_nEvents, msg->m_tvSend, msg->m_blDelete);
		m_SendMessagePool.Delete(msg);

	}

	OUR_DEBUG((LM_INFO,"[CProConnectManager::svc] svc finish!\n"));
	return 0;
}

bool CProConnectManager::IsRun()
{
	return m_blRun;
}

int CProConnectManager::close(u_long)
{
	m_blRun = false;
	OUR_DEBUG((LM_INFO,"[CProConnectManager::close] close().\n"));
	return 0;
}

void CProConnectManager::GetConnectInfo(vecClientConnectInfo& VecClientConnectInfo)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);

	for(int i = 0; i < m_objHashConnectList.Get_Count(); i++)
	{
		CProConnectHandle* pConnectHandler = (CProConnectHandle* )m_objHashConnectList.Get_Index(i);
		if(pConnectHandler != NULL)
		{
			VecClientConnectInfo.push_back(pConnectHandler->GetClientInfo());
		}
	}
}

void CProConnectManager::SetRecvQueueTimeCost(uint32 u4ConnectID, uint32 u4TimeCost)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);
	if(NULL != pConnectHandler)
	{
		pConnectHandler->SetRecvQueueTimeCost(u4TimeCost);
	}

}

_ClientIPInfo CProConnectManager::GetClientIPInfo(uint32 u4ConnectID)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);
	if(NULL != pConnectHandler)
	{
		return pConnectHandler->GetClientIPInfo();
	}
	else
	{
		_ClientIPInfo ClientIPInfo;
		return ClientIPInfo;
	}
}

_ClientIPInfo CProConnectManager::GetLocalIPInfo(uint32 u4ConnectID)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);
	if(NULL != pConnectHandler)
	{
		return pConnectHandler->GetLocalIPInfo();
	}
	else
	{
		_ClientIPInfo ClientIPInfo;
		return ClientIPInfo;
	}
}

bool CProConnectManager::PostMessageAll( IBuffPacket* pBuffPacket, uint8 u1SendType, uint16 u2CommandID, uint8 u1SendState, bool blDelete)
{
	m_ThreadWriteLock.acquire();
	vector<CProConnectHandle*> objveCProConnectManager;
	for(int i = 0; i < m_objHashConnectList.Get_Count(); i++)
	{
		CProConnectHandle* pProConnectHandle = m_objHashConnectList.Get_Index(i);
		if(NULL != pProConnectHandle)
		{
			objveCProConnectManager.push_back(pProConnectHandle);
		}
	}
	m_ThreadWriteLock.release();

	uint32 u4ConnectID = 0;
	for(uint32 i = 0; i < (uint32)objveCProConnectManager.size(); i++)
	{
		IBuffPacket* pCurrBuffPacket = App_BuffPacketManager::instance()->Create();
		if(NULL == pCurrBuffPacket)
		{
			OUR_DEBUG((LM_INFO, "[CProConnectManager::PostMessage]pCurrBuffPacket is NULL.\n"));
			App_BuffPacketManager::instance()->Delete(pBuffPacket);
			return false;
		}

		pCurrBuffPacket->WriteStream(pBuffPacket->GetData(), pBuffPacket->GetPacketLen());

		//放入发送队列
		_SendMessage* pSendMessage = m_SendMessagePool.Create();

		ACE_Message_Block* mb = pSendMessage->GetQueueMessage();

		if(NULL != mb)
		{
			if(NULL == pSendMessage)
			{
				OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] new _SendMessage is error.\n"));
				App_BuffPacketManager::instance()->Delete(pBuffPacket);
				return false;
			}

			pSendMessage->m_u4ConnectID = objveCProConnectManager[i]->GetConnectID();
			pSendMessage->m_pBuffPacket = pCurrBuffPacket;
			pSendMessage->m_nEvents     = u1SendType;
			pSendMessage->m_u2CommandID = u2CommandID;
			pSendMessage->m_u1SendState = u1SendState;
			pSendMessage->m_blDelete    = blDelete;
			pSendMessage->m_tvSend      = ACE_OS::gettimeofday();

			//判断队列是否是已经最大
			int nQueueCount = (int)msg_queue()->message_count();
			if(nQueueCount >= (int)MAX_MSG_THREADQUEUE)
			{
				OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] Queue is Full nQueueCount = [%d].\n", nQueueCount));
				if(blDelete == true)
				{
					App_BuffPacketManager::instance()->Delete(pBuffPacket);
				}
				m_SendMessagePool.Delete(pSendMessage);
				return false;
			}

			ACE_Time_Value xtime = ACE_OS::gettimeofday() + ACE_Time_Value(0, MAX_MSG_PUTTIMEOUT);
			if(this->putq(mb, &xtime) == -1)
			{
				OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] Queue putq  error nQueueCount = [%d] errno = [%d].\n", nQueueCount, errno));
				if(blDelete == true)
				{
					App_BuffPacketManager::instance()->Delete(pBuffPacket);
				}
				m_SendMessagePool.Delete(pSendMessage);
				return false;
			}
		}
		else
		{
			OUR_DEBUG((LM_ERROR,"[CProConnectManager::PutMessage] mb new error.\n"));
			if(blDelete == true)
			{
				App_BuffPacketManager::instance()->Delete(pBuffPacket);
			}
			return false;
		}
	}

	return true;
}

bool CProConnectManager::SetConnectName( uint32 u4ConnectID, const char* pName )
{
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);
	if(NULL != pConnectHandler)
	{
		pConnectHandler->SetConnectName(pName);
		return true;
	}
	else
	{
		return false;
	}
}

bool CProConnectManager::SetIsLog( uint32 u4ConnectID, bool blIsLog )
{
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);
	if(NULL != pConnectHandler)
	{
		pConnectHandler->SetIsLog(blIsLog);
		return true;
	}
	else
	{
		return false;
	}
}

void CProConnectManager::GetClientNameInfo(const char* pName, vecClientNameInfo& objClientNameInfo)
{
	for(int i = 0; i < m_objHashConnectList.Get_Count(); i++)
	{
		CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Index(i);
		if(NULL != pConnectHandler && ACE_OS::strcmp(pConnectHandler->GetConnectName(), pName) == 0)
		{
			_ClientNameInfo ClientNameInfo;
			ClientNameInfo.m_nConnectID = (int)pConnectHandler->GetConnectID();
			sprintf_safe(ClientNameInfo.m_szName, MAX_BUFF_100, "%s", pConnectHandler->GetConnectName());
			sprintf_safe(ClientNameInfo.m_szClientIP, MAX_BUFF_50, "%s", pConnectHandler->GetClientIPInfo().m_szClientIP);
			ClientNameInfo.m_nPort =  pConnectHandler->GetClientIPInfo().m_nPort;
			if(pConnectHandler->GetIsLog() == true)
			{
				ClientNameInfo.m_nLog = 1;
			}
			else
			{
				ClientNameInfo.m_nLog = 0;
			}

			objClientNameInfo.push_back(ClientNameInfo);
		}
	}
}

void CProConnectManager::Init(uint16 u2Index)
{
	//按照线程初始化统计模块的名字
	char szName[MAX_BUFF_50] = {'\0'};
	sprintf_safe(szName, MAX_BUFF_50, "发送线程(%d)", u2Index);
	m_CommandAccount.InitName(szName, App_MainConfig::instance()->GetMaxCommandCount());

	//初始化统计模块功能
	m_CommandAccount.Init(App_MainConfig::instance()->GetCommandAccount(), 
		App_MainConfig::instance()->GetCommandFlow(), 
		App_MainConfig::instance()->GetPacketTimeOut());

	//初始化发送缓冲池
	m_SendCacheManager.Init(App_MainConfig::instance()->GetBlockCount(), App_MainConfig::instance()->GetBlockSize());

	//初始化Hash表
	uint16 u2PoolSize = App_MainConfig::instance()->GetMaxHandlerCount();
	size_t nArraySize = (sizeof(_Hash_Table_Cell<CProConnectHandle>)) * u2PoolSize;
	char* pHashBase = new char[nArraySize];
	m_objHashConnectList.Init(pHashBase, (int)u2PoolSize);
}

_CommandData* CProConnectManager::GetCommandData(uint16 u2CommandID)
{
	return m_CommandAccount.GetCommandData(u2CommandID);
}

uint32 CProConnectManager::GetCommandFlowAccount()
{
	return m_CommandAccount.GetFlowOut();
}

EM_Client_Connect_status CProConnectManager::GetConnectState(uint32 u4ConnectID)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGrard(m_ThreadWriteLock);
	char szConnectID[10] = {'\0'};
	sprintf_safe(szConnectID, 10, "%d", u4ConnectID);
	CProConnectHandle* pConnectHandler = m_objHashConnectList.Get_Hash_Box_Data(szConnectID);

	if(NULL != pConnectHandler)
	{
		return CLIENT_CONNECT_EXIST;
	}
	else
	{
		return CLIENT_CONNECT_NO_EXIST;
	}
}

//*********************************************************************************

CProConnectHandlerPool::CProConnectHandlerPool(void)
{
	//ConnectID计数器从1开始
	m_u4CurrMaxCount = 1;
}

CProConnectHandlerPool::~CProConnectHandlerPool(void)
{
	Close();
}

void CProConnectHandlerPool::Init(int nObjcetCount)
{
	Close();

	//初始化HashTable
	size_t nArraySize = (sizeof(_Hash_Table_Cell<CProConnectHandle>)) * nObjcetCount;
	char* pHashBase = new char[nArraySize];
	m_objHashHandleList.Init(pHashBase, (int)nObjcetCount);

	for(int i = 0; i < nObjcetCount; i++)
	{
		CProConnectHandle* pHandler = new CProConnectHandle();
		if(NULL != pHandler)
		{
			pHandler->Init(m_u4CurrMaxCount);

			//将ID和Handler指针的关系存入hashTable
			char szHandlerID[10] = {'\0'};
			sprintf_safe(szHandlerID, 10, "%d", m_u4CurrMaxCount);
			int nHashPos = m_objHashHandleList.Add_Hash_Data(szHandlerID, pHandler);
			if(-1 != nHashPos)
			{
				pHandler->SetHashID(nHashPos);
			}
			m_u4CurrMaxCount++;
		}
	}

	//检查是否所有的hash表都被填满了
	for(int i = 0; i < m_objHashHandleList.Get_Count(); i++)
	{
		if(NULL == m_objHashHandleList.Get_Index(i))
		{
			OUR_DEBUG((LM_INFO, "[CProConnectHandlerPool::Init]ERROR(%d) is NULL.\n", i));
		}
	}

	//用户hash表的查找空余对象
	m_u4CulationIndex = 0;
}

void CProConnectHandlerPool::Close()
{
	//清理所有已存在的指针
	for(int i = 0; i < m_objHashHandleList.Get_Count(); i++)
	{
		CProConnectHandle* pHandler = m_objHashHandleList.Get_Index(i);
		SAFE_DELETE(pHandler);
	}

	m_u4CurrMaxCount  = 1;
	m_u4CulationIndex = 0;
}

int CProConnectHandlerPool::GetUsedCount()
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGuard(m_ThreadWriteLock);

	return m_objHashHandleList.Get_Count() - m_objHashHandleList.Get_Used_Count();
}

int CProConnectHandlerPool::GetFreeCount()
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGuard(m_ThreadWriteLock);

	return (int)m_objHashHandleList.Get_Used_Count();
}

CProConnectHandle* CProConnectHandlerPool::Create()
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGuard(m_ThreadWriteLock);

	CProConnectHandle* pHandler = NULL;

	//在Hash表中弹出一个已使用的数据
	//判断循环指针是否已经找到了尽头，如果是则从0开始继续
	if(m_u4CulationIndex + 1 >= m_u4CurrMaxCount - 1)
	{
		m_u4CulationIndex = 0;
	}

	//第一次寻找，从当前位置往后找
	for(int i = (int)m_u4CulationIndex; i < m_objHashHandleList.Get_Count(); i++)
	{
		pHandler = m_objHashHandleList.Get_Index(i);
		if(NULL != pHandler)
		{
			//已经找到了，返回指针
			char szHandlerID[10] = {'\0'};
			sprintf_safe(szHandlerID, 10, "%d", pHandler->GetHandlerID());
			//int nDelPos = m_objHashHandleList.Del_Hash_Data(szHandlerID);
			int nDelPos = m_objHashHandleList.Set_Index_Clear(i);
			if(-1 == nDelPos)
			{
				OUR_DEBUG((LM_INFO, "[CProConnectHandlerPool::Create]szHandlerID=%s, nPos=%d, nDelPos=%d, (0x%08x).\n", szHandlerID, i, nDelPos, pHandler));
			}
			else
			{
				//OUR_DEBUG((LM_INFO, "[CProConnectHandlerPool::Create]szHandlerID=%s, nPos=%d, nDelPos=%d, (0x%08x).\n", szHandlerID, i, nDelPos, pHandler));
			}
			m_u4CulationIndex = i;
			return pHandler;
		}
	}

	//第二次寻找，从0到当前位置
	for(int i = 0; i < (int)m_u4CulationIndex; i++)
	{
		pHandler = m_objHashHandleList.Get_Index(i);
		if(NULL != pHandler)
		{
			//已经找到了，返回指针
			char szHandlerID[10] = {'\0'};
			sprintf_safe(szHandlerID, 10, "%d", pHandler->GetHandlerID());
			int nDelPos = m_objHashHandleList.Set_Index_Clear(i);
			if(-1 == nDelPos)
			{
				OUR_DEBUG((LM_INFO, "[CProConnectHandlerPool::Create]szHandlerID=%s, nPos=%d, nDelPos=%d, (0x%08x).\n", szHandlerID, i, nDelPos, pHandler));
			}
			else
			{
				//OUR_DEBUG((LM_INFO, "[CProConnectHandlerPool::Create]szHandlerID=%s, nPos=%d, nDelPos=%d, (0x%08x).\n", szHandlerID, i, nDelPos, pHandler));
			}
			m_u4CulationIndex = i;
			return pHandler;
		}
	}

	//没找到空余的
	return pHandler;
}

bool CProConnectHandlerPool::Delete(CProConnectHandle* pObject)
{
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGuard(m_ThreadWriteLock);

	if(NULL == pObject)
	{
		return false;
	}

	char szHandlerID[10] = {'\0'};
	sprintf_safe(szHandlerID, 10, "%d", pObject->GetHandlerID());
	//int nPos = m_objHashHandleList.Add_Hash_Data(szHandlerID, pObject);
	//这里因为内存是固定的，直接写会Hash原有位置
	int nPos = m_objHashHandleList.Set_Index(pObject->GetHashID(), szHandlerID, pObject);
	if(-1 == nPos)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectHandlerPool::Delete]szHandlerID=%s(0x%08x) nPos=%d.\n", szHandlerID, pObject, nPos));
		//m_objHashHandleList.Add_Hash_Data(szHandlerID, pObject);
	}
	else
	{
		OUR_DEBUG((LM_INFO, "[CProConnectHandlerPool::Delete]szHandlerID=%s(0x%08x) nPos=%d.\n", szHandlerID, pObject, nPos));
	}

	return true;
}

//==============================================================
CProConnectManagerGroup::CProConnectManagerGroup()
{
	m_objProConnnectManagerList = NULL;
	m_u4CurrMaxCount            = 0;
	m_u2ThreadQueueCount        = SENDQUEUECOUNT;
}

CProConnectManagerGroup::~CProConnectManagerGroup()
{
	OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::~CProConnectManagerGroup].\n"));
	Close();
}

void CProConnectManagerGroup::Close()
{
	if(NULL != m_objProConnnectManagerList)
	{
		for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
		{
			CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
			SAFE_DELETE(pConnectManager);
		}
	}
	SAFE_DELETE_ARRAY(m_objProConnnectManagerList);
	m_u2ThreadQueueCount = 0;
}

void CProConnectManagerGroup::Init(uint16 u2SendQueueCount)
{
	Close();

	m_objProConnnectManagerList = new CProConnectManager*[u2SendQueueCount];
	memset(m_objProConnnectManagerList, 0, sizeof(CProConnectManager*)*u2SendQueueCount);

	for(int i = 0; i < u2SendQueueCount; i++)
	{
		CProConnectManager* pConnectManager = new CProConnectManager();
		if(NULL != pConnectManager)	
		{
			//初始化统计器
			pConnectManager->Init((uint16)i);

			//加入数组
			m_objProConnnectManagerList[i] = pConnectManager;
			OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::Init]Creat %d SendQueue OK.\n", i));
		}
	}

	m_u2ThreadQueueCount = u2SendQueueCount;
}

uint32 CProConnectManagerGroup::GetGroupIndex()
{
	//根据链接获得命中，（简单球形命中算法）
	ACE_Guard<ACE_Recursive_Thread_Mutex> WGuard(m_ThreadWriteLock);
	m_u4CurrMaxCount++;
	return m_u4CurrMaxCount;
}

bool CProConnectManagerGroup::AddConnect(CProConnectHandle* pConnectHandler)
{
	bool blRet = false;
	int  nCount = 0;
	while(true)
	{
		//最多循环5次，如果5次还找不到则返回false。
		if(nCount >= 5)
		{
			return false;
		}

		uint32 u4ConnectID = GetGroupIndex();

		//判断命中到哪一个线程组里面去
		uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

		CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
		if(NULL == pConnectManager)
		{
			OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::AddConnect]No find send Queue object.\n"));
			return false;		
		}

		blRet = pConnectManager->AddConnect(u4ConnectID, pConnectHandler);
		if(true == blRet)
		{
			return true;
		}

		nCount++;
	}


	//OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::Init]u4ConnectID=%d, u2ThreadIndex=%d.\n", u4ConnectID, u2ThreadIndex));

	return false;
}

bool CProConnectManagerGroup::PostMessage(uint32 u4ConnectID, IBuffPacket* pBuffPacket, uint8 u1SendType, uint16 u2CommandID, uint8 u1SendState, bool blDelete)
{
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]No find send Queue object.\n"));
		return false;		
	}

	//OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]u4ConnectID=%d, u2ThreadIndex=%d.\n", u4ConnectID, u2ThreadIndex));

	return pConnectManager->PostMessage(u4ConnectID, pBuffPacket, u1SendType, u2CommandID, u1SendState, blDelete);
}

bool CProConnectManagerGroup::PostMessage( uint32 u4ConnectID, const char* pData, uint32 nDataLen, uint8 u1SendType, uint16 u2CommandID, uint8 u1SendState, bool blDelete)
{
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]No find send Queue object.\n"));

		if(blDelete == true)
		{
			SAFE_DELETE_ARRAY(pData);
		}

		return false;		
	}

	//OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]u4ConnectID=%d, u2ThreadIndex=%d.\n", u4ConnectID, u2ThreadIndex));
	IBuffPacket* pBuffPacket = App_BuffPacketManager::instance()->Create();
	if(NULL != pBuffPacket)
	{
		pBuffPacket->WriteStream(pData, nDataLen);

		if(blDelete == true)
		{
			SAFE_DELETE_ARRAY(pData);
		}

		return pConnectManager->PostMessage(u4ConnectID, pBuffPacket, u1SendType, u2CommandID, u1SendState, true);
	}
	else
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]pBuffPacket is NULL.\n"));

		if(blDelete == true)
		{
			SAFE_DELETE_ARRAY(pData);
		}

		return false;
	}

}

bool CProConnectManagerGroup::PostMessage( vector<uint32> vecConnectID, IBuffPacket* pBuffPacket, uint8 u1SendType, uint16 u2CommandID, uint8 u1SendState, bool blDelete)
{
	uint32 u4ConnectID = 0;
	for(uint32 i = 0; i < (uint32)vecConnectID.size(); i++)
	{
		//判断命中到哪一个线程组里面去
		u4ConnectID = vecConnectID[i];
		uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

		CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
		if(NULL == pConnectManager)
		{
			OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]No find send Queue object.\n"));
			continue;		
		}

		//为每一个Connect设置发送对象数据包
		IBuffPacket* pCurrBuffPacket = App_BuffPacketManager::instance()->Create();
		if(NULL == pCurrBuffPacket)
		{
			continue;
		}
		pCurrBuffPacket->WriteStream(pBuffPacket->GetData(), pBuffPacket->GetWriteLen());

		pConnectManager->PostMessage(u4ConnectID, pCurrBuffPacket, u1SendType, u2CommandID, u1SendState, true);
	}

	if(true == blDelete)
	{
		App_BuffPacketManager::instance()->Delete(pBuffPacket);
	}


	return true;
}

bool CProConnectManagerGroup::PostMessage( vector<uint32> vecConnectID, const char* pData, uint32 nDataLen, uint8 u1SendType, uint16 u2CommandID, uint8 u1SendState, bool blDelete)
{
	uint32 u4ConnectID = 0;

	for(uint32 i = 0; i < (uint32)vecConnectID.size(); i++)
	{
		//判断命中到哪一个线程组里面去
		u4ConnectID = vecConnectID[i];
		uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

		CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
		if(NULL == pConnectManager)
		{
			OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]No find send Queue object.\n"));
			continue;
		}

		//为每一个Connect设置发送对象数据包
		IBuffPacket* pBuffPacket = App_BuffPacketManager::instance()->Create();
		if(NULL == pBuffPacket)
		{
			continue;
		}
		pBuffPacket->WriteStream(pData, nDataLen);

		pConnectManager->PostMessage(u4ConnectID, pBuffPacket, u1SendType, u2CommandID, u1SendState, true);
	}

	if(true == blDelete)
	{
		SAFE_DELETE_ARRAY(pData);
	}

	return true;
}

bool CProConnectManagerGroup::CloseConnect(uint32 u4ConnectID, EM_Client_Close_status emStatus)
{
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::CloseConnect]No find send Queue object.\n"));
		return false;		
	}	

	return pConnectManager->CloseConnect(u4ConnectID, emStatus);
}

_ClientIPInfo CProConnectManagerGroup::GetClientIPInfo(uint32 u4ConnectID)
{
	_ClientIPInfo objClientIPInfo;
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::GetClientIPInfo]No find send Queue object.\n"));
		return objClientIPInfo;		
	}	

	return pConnectManager->GetClientIPInfo(u4ConnectID);	
}

_ClientIPInfo CProConnectManagerGroup::GetLocalIPInfo(uint32 u4ConnectID)
{
	_ClientIPInfo objClientIPInfo;
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::GetLocalIPInfo]No find send Queue object.\n"));
		return objClientIPInfo;		
	}	

	return pConnectManager->GetLocalIPInfo(u4ConnectID);	
}


void CProConnectManagerGroup::GetConnectInfo(vecClientConnectInfo& VecClientConnectInfo)
{
	VecClientConnectInfo.clear();

	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL != pConnectManager)
		{
			pConnectManager->GetConnectInfo(VecClientConnectInfo);
		}
	}
}

int CProConnectManagerGroup::GetCount()
{
	uint32 u4Count = 0;

	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL != pConnectManager)
		{
			u4Count += pConnectManager->GetCount();
		}
	}	

	return u4Count;
}

void CProConnectManagerGroup::CloseAll()
{
	uint32 u4Count = 0;

	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL != pConnectManager)
		{
			pConnectManager->CloseAll();
		}
	}	
}

bool CProConnectManagerGroup::StartTimer()
{
	uint32 u4Count = 0;

	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL != pConnectManager)
		{
			pConnectManager->StartTimer();
		}
	}

	return true;	
}

bool CProConnectManagerGroup::Close(uint32 u4ConnectID)
{
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::GetClientIPInfo]No find send Queue object.\n"));
		return false;		
	}	

	return pConnectManager->Close(u4ConnectID);
}

const char* CProConnectManagerGroup::GetError()
{
	return (char* )"";
}

void CProConnectManagerGroup::SetRecvQueueTimeCost(uint32 u4ConnectID, uint32 u4TimeCost)
{
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::GetClientIPInfo]No find send Queue object.\n"));
		return;		
	}		

	pConnectManager->SetRecvQueueTimeCost(u4ConnectID, u4TimeCost);
}

bool CProConnectManagerGroup::PostMessageAll( IBuffPacket* pBuffPacket, uint8 u1SendType, uint16 u2CommandID, uint8 u1SendState, bool blDelete)
{
	//全部群发
	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL == pConnectManager)
		{
			OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]No find send Queue object.\n"));
			continue;		
		}

		pConnectManager->PostMessageAll(pBuffPacket, u1SendType, u2CommandID, u1SendState, false);
	}

	if(true == blDelete)
	{
		//用完了就删除
		App_BuffPacketManager::instance()->Delete(pBuffPacket);
	}

	return true;
}

bool CProConnectManagerGroup::PostMessageAll( const char* pData, uint32 nDataLen, uint8 u1SendType, uint16 u2CommandID, uint8 u1SendState, bool blDelete)
{
	IBuffPacket* pBuffPacket = App_BuffPacketManager::instance()->Create();
	if(NULL == pBuffPacket)
	{
		OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessageAll]pBuffPacket is NULL.\n"));

		if(blDelete == true)
		{
			SAFE_DELETE_ARRAY(pData);
		}

		return false;
	}
	else
	{
		pBuffPacket->WriteStream(pData, nDataLen);
	}

	//全部群发
	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL == pConnectManager)
		{
			OUR_DEBUG((LM_INFO, "[CProConnectManagerGroup::PostMessage]No find send Queue object.\n"));
			continue;
		}

		pConnectManager->PostMessageAll(pBuffPacket, u1SendType, u2CommandID, u1SendState, false);
	}

	//用完了就删除
	App_BuffPacketManager::instance()->Delete(pBuffPacket);

	//用完了就删除
	if(true == blDelete)
	{
		SAFE_DELETE_ARRAY(pData);
	}

	return true;
}

bool CProConnectManagerGroup::SetConnectName( uint32 u4ConnectID, const char* pName )
{
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CConnectManagerGroup::CloseConnect]No find send Queue object.\n"));
		return false;		
	}	

	return pConnectManager->SetConnectName(u4ConnectID, pName);	
}

bool CProConnectManagerGroup::SetIsLog( uint32 u4ConnectID, bool blIsLog )
{
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CConnectManagerGroup::CloseConnect]No find send Queue object.\n"));
		return false;		
	}	

	return pConnectManager->SetIsLog(u4ConnectID, blIsLog);		
}

void CProConnectManagerGroup::GetClientNameInfo( const char* pName, vecClientNameInfo& objClientNameInfo )
{
	objClientNameInfo.clear();
	//全部查找
	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL != pConnectManager)
		{
			pConnectManager->GetClientNameInfo(pName, objClientNameInfo);
		}
	}	
}

void CProConnectManagerGroup::GetCommandData(uint16 u2CommandID, _CommandData& objCommandData)
{
	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL != pConnectManager)
		{
			_CommandData* pCommandData = pConnectManager->GetCommandData(u2CommandID);
			if(pCommandData != NULL)
			{
				objCommandData += (*pCommandData);
			}
		}
	}	
}

void CProConnectManagerGroup::GetCommandFlowAccount(_CommandFlowAccount& objCommandFlowAccount)
{
	for(uint16 i = 0; i < m_u2ThreadQueueCount; i++)
	{
		CProConnectManager* pConnectManager = m_objProConnnectManagerList[i];
		if(NULL != pConnectManager)
		{
			uint32 u4FlowOut =  pConnectManager->GetCommandFlowAccount();
			objCommandFlowAccount.m_u4FlowOut += u4FlowOut;
		}
	}	
}

EM_Client_Connect_status CProConnectManagerGroup::GetConnectState(uint32 u4ConnectID)
{
	//判断命中到哪一个线程组里面去
	uint16 u2ThreadIndex = u4ConnectID % m_u2ThreadQueueCount;

	CProConnectManager* pConnectManager = m_objProConnnectManagerList[u2ThreadIndex];
	if(NULL == pConnectManager)
	{
		OUR_DEBUG((LM_INFO, "[CConnectManagerGroup::CloseConnect]No find send Queue object.\n"));
		return CLIENT_CONNECT_NO_EXIST;		
	}

	return pConnectManager->GetConnectState(u4ConnectID);
}