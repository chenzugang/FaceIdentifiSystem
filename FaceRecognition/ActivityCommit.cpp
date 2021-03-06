﻿#include "stdafx.h"
#include "ActivityCommit.h"
#include "ActiveReporter.h"
#include "ActiveUploader.h"

#include "document.h" 
#include "prettywriter.h"
#include "stringbuffer.h"
#include <Poco/Data/Session.h>
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/SQLite/SQLiteException.h>
#include <Poco/Exception.h>
#include <Poco/Path.h>
#include <Poco/Logger.h>
#include <Poco/Net/ICMPClient.h>

using Poco::Path;
using Poco::Data::Session;
using Poco::Data::Statement;
using Poco::Net::ICMPClient;

using namespace Poco::Data::Keywords;
using namespace rapidjson;

struct alert_table
{
	int id;
	std::string filepath;
	int type;
	std::string timestamp;
	int userid;
	int deviceid;
};

ActivityCommit::ActivityCommit():
_activity(this, &ActivityCommit::runActivity),
_dbSession(Session("SQLite", "facerecog.db")),
_server_addr("202.103.191.73")
{
}


ActivityCommit::~ActivityCommit()
{
}

void ActivityCommit::start()
{
	_activity.start();
}

void ActivityCommit::stop()
{
	_activity.stop();
	_activity.wait(3000);//15s 中止时间
}

#include <Poco/Net/IPAddress.h>
using Poco::Net::IPAddress;

bool ActivityCommit::check_network_status()
{
	//检查网络是否正常状态
	//ping 202.103.191.73 是否正常
	/*ICMPClient imcp(IPAddress::IPv4);
	bool ret = (imcp.ping("localhost") > 0);
	poco_information_f1(Poco::Logger::get("FileLogger"), "check_network_status %d", ret);*/
	return true;
}

void ActivityCommit::runActivity()
{
	while (!_activity.isStopped())
	{
		DUITRACE("ActivityCommit::runActivity");
		poco_information(Poco::Logger::get("FileLogger"), "ActivityCommit::runActivity");
		try
		{
			//Session session("SQLite", "facerecog.db");
			if (!_dbSession.isConnected())
				_dbSession.reconnect();

			Statement select(_dbSession);
			alert_table alert;
			select << "SELECT id, filepath, type, timestamp, UserInfoID, DeviceID FROM alert WHERE hasUpload = 0",
				into(alert.id),
				into(alert.filepath),
				into(alert.type),
				into(alert.timestamp),
				into(alert.userid),
				into(alert.deviceid),
				range(0, 1);

			while (!select.done())
			{
				if (_activity.isStopped()) break;

				if (select.execute())
				{
					if (check_network_status())
					{
						//仅当网络正常时，上传数据
						poco_information(Poco::Logger::get("FileLogger"), "runActivity : login to 202.103.191.73");
						FTPClientSession _ftp("202.103.191.73", FTPClientSession::FTP_PORT, "ftpalert", "1");
						ActiveUploader ur(_ftp);
						ActiveResult<bool> result = ur.upload(alert.filepath);
						post_alert_data(alert);
						update_alert_upload_status(alert.id);
						result.wait();
					}
				}
			}
		}
		catch (Poco::Exception& e)
		{
			poco_information_f1(Poco::Logger::get("FileLogger"), "ActivityCommit::runActivity::Exception %s", e.displayText());
			DUITRACE("ActivityCommit::runActivity::Exception : %s",e.displayText().c_str());
		}
		Thread::sleep(2000);
	}
}

void ActivityCommit::update_alert_upload_status(int uid)
{
	try
	{
		Statement update(_dbSession);
		update << "UPDATE alert SET hasUpload = 1 WHERE id = ?",
			use(uid),
			now;
	}
	catch (Poco::Data::SQLite::DBLockedException& e)
	{
		poco_information_f1(Poco::Logger::get("FileLogger"), "update_alert_upload_status %s", e.displayText());
		/*DUITRACE(e.displayText().c_str());
		Thread::sleep(1000);
		Statement update(_dbSession);
		update << "UPDATE alert SET hasUpload = 1 WHERE id = ?",
			use(uid),
			now;*/
	}
}

void ActivityCommit::post_alert_data(alert_table& alert)
{

	HTTPClientSession session("202.103.191.73");
	ActiveReporter rp(session);

	/*
	std::string body("body={\"api\":\"wn_add\", \"filepath\":\"http://202.103.191.73/api/3870861421055882.jpg\",\"type\":1,\"timestamp\":\"20161019221808\",\"UserInfoID\":1,\"DeviceID\":1,\"summary\":\"test\"}");
	request.setContentLength((int)body.length());
	*/
	std::string server_dir("http://202.103.191.73/photos/");
	Path p(alert.filepath);
	server_dir += p.getFileName();

	Document d;
	d.SetObject();
	d.AddMember("api", "wn_add", d.GetAllocator());
	d.AddMember("filepath", StringRef(server_dir.c_str()), d.GetAllocator());
	d.AddMember("type", alert.type, d.GetAllocator());
	d.AddMember("timestamp", StringRef(alert.timestamp.c_str()), d.GetAllocator());
	d.AddMember("UserInfoID", alert.userid, d.GetAllocator());
	d.AddMember("DeviceID", alert.deviceid, d.GetAllocator());
	d.AddMember("summary", "test", d.GetAllocator());
	StringBuffer sb;
	Writer<StringBuffer> writer(sb);
	d.Accept(writer);

	ActiveResult<std::string> result = rp.report(sb.GetString());
	result.wait();
	std::string received = result.data();
	DUITRACE("HTTP RECEIVED : %s", received);
	poco_information_f1(Poco::Logger::get("FileLogger"), "HTTP RECEIVED : %s", received);

	if (received.empty())
		throw Poco::Exception("HTTP RECEIVED FAILED - NO RECEIVED!");
	
	Document document;
	received = received.substr(3, std::string::npos);
	if (document.Parse(received.c_str()).HasParseError() || !document.HasMember("id"))
	{
		throw Poco::Exception("HTTP RECEIVED PARSE FAILED -  NO ID!");
	}
}